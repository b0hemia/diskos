/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "sdio.h"
#include "ipc.h"
#include "musicdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* Lyrics view: fetches lyrics online from lrclib.net by track + artist (no API
 * key), falling back to a sibling .lrc on the SD card. The blocking wget runs on
 * a detached thread; the result is applied on the main thread by lyrics_poll(). */

static lv_obj_t *g_scroll;
static lv_obj_t *g_text;

/* worker->main handoff guarded by a mutex (volatile gives no cross-thread memory
 * ordering - on the dual-core X2000 the main thread could see g_lready set before the
 * g_lbuf writes are visible). The lock release/acquire pairs publish with consume. */
static pthread_mutex_t g_ly_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_lbuf[8192];          /* result text (lyrics or message)  (see handoff note) */
static int g_lready = 0;           /* a fetch finished                 (guarded) */
static int g_linflight = 0;        /* a fetch thread is running        (guarded) */
static int g_lphase = 0;           /* 0=checking local .lrc, 1=searching online (guarded) */
static unsigned g_req = 0;         /* bumps on each lyrics_open         (main thread only) */
static unsigned g_done_req = 0;    /* request id the finished fetch was for (guarded) */
static char g_q_title[512], g_q_artist[512];  /* url-encoded query (main thread only); 3x MDB_STR(160)
                                               * expansion for %-encoded non-ASCII/CJK titles + margin */
static char g_path[256];                       /* current track path (main thread only) */

/* Per-fetch inputs snapshotted on the MAIN thread before pthread_create and handed to
 * the worker as its arg, so the worker never reads the g_req / g_q_title / g_q_artist /
 * g_path globals directly (they are main-thread-only; a re-open could otherwise race
 * the worker's reads of them). */
typedef struct { unsigned req; char title[512], artist[512], path[256]; } ly_job_t;  /* enc query, see g_q_* */


static void urlenc(const char *s, char *out, int cap)
{
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    for(; *s && o < cap - 4; s++){
        unsigned char c = (unsigned char)*s;
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')
            out[o++] = (char)c;
        else { out[o++]='%'; out[o++]=hex[c>>4]; out[o++]=hex[c&15]; }
    }
    out[o] = 0;
}

/* audio path -> sibling .lrc path */
static void derive_lrc(const char *audio, char *out, int cap)
{
    snprintf(out, cap, "%s", audio);
    char *slash = strrchr(out, '/');
    char *dot   = strrchr(out, '.');
    if(dot && (!slash || dot > slash)) snprintf(dot, cap - (int)(dot - out), ".lrc");
    else { int n = (int)strlen(out); snprintf(out + n, cap - n, ".lrc"); }
}

/* strip [..] timestamp/metadata groups from an .lrc, into out */
static int strip_lrc(FILE *f, char *out, int cap)
{
    int len = 0; char line[512];
    while(fgets(line, sizeof line, f) && len < cap - 256){
        char *p = line;
        while(*p==' '||*p=='\t') p++;
        while(*p=='['){ char *c = strchr(p, ']'); if(!c) break; p = c + 1; }
        char *nl = strpbrk(p, "\r\n"); if(nl) *nl = 0;
        if(*p) len += snprintf(out + len, cap - len, "%s\n", p);
    }
    return len;
}

/* parse 4 hex digits -> value, or -1 if any is not hex */
static int hex4(const char *s){
    int v = 0;
    for(int i=0;i<4;i++){
        char c = s[i]; int d;
        if(c>='0'&&c<='9') d = c-'0';
        else if(c>='a'&&c<='f') d = c-'a'+10;
        else if(c>='A'&&c<='F') d = c-'A'+10;
        else return -1;
        v = (v<<4) | d;
    }
    return v;
}
/* encode a Unicode codepoint as UTF-8 into out (up to 4 bytes); returns bytes written (0 if no room) */
static int utf8_enc(unsigned cp, char *out, int cap){
    if(cp < 0x80)   { if(cap<1) return 0; out[0]=(char)cp; return 1; }
    if(cp < 0x800)  { if(cap<2) return 0; out[0]=(char)(0xC0|(cp>>6));  out[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if(cp < 0x10000){ if(cap<3) return 0; out[0]=(char)(0xE0|(cp>>12)); out[1]=(char)(0x80|((cp>>6)&0x3F)); out[2]=(char)(0x80|(cp&0x3F)); return 3; }
    if(cap<4) return 0;
    out[0]=(char)(0xF0|(cp>>18)); out[1]=(char)(0x80|((cp>>12)&0x3F)); out[2]=(char)(0x80|((cp>>6)&0x3F)); out[3]=(char)(0x80|(cp&0x3F)); return 4;
}

/* pull the first "plainLyrics":"..." JSON string out of resp into out (unescaped) */
static int json_plain_lyrics(const char *resp, char *out, int cap)
{
    const char *p = strstr(resp, "\"plainLyrics\":\"");
    if(!p) return 0;
    p += 15;
    int len = 0;
    while(*p && len < cap - 2){
        if(*p == '\\'){
            p++; char e = *p;
            if(e=='n') out[len++]='\n';
            else if(e=='t') out[len++]=' ';
            else if(e=='"') out[len++]='"';
            else if(e=='\\') out[len++]='\\';
            else if(e=='/') out[len++]='/';
            else if(e=='r') { /* drop */ }
            else if(e=='u'){   /* \uXXXX -> UTF-8 (CJK etc.); combine surrogate pairs */
                int u = (p[1]&&p[2]&&p[3]&&p[4]) ? hex4(p+1) : -1;
                if(u >= 0){
                    unsigned cp = (unsigned)u;
                    p += 4;        /* consume the 4 hex digits (trailing p++ steps off the last one) */
                    if(cp>=0xD800 && cp<=0xDBFF && p[1]=='\\' && p[2]=='u' &&
                       p[3]&&p[4]&&p[5]&&p[6]){
                        int lo = hex4(p+3);
                        if(lo>=0xDC00 && lo<=0xDFFF){
                            cp = 0x10000u + ((cp-0xD800u)<<10) + (unsigned)(lo-0xDC00);
                            p += 6;   /* consume the "\uXXXX" low surrogate too */
                        }
                    }
                    /* drop any leftover lone/unpaired surrogate (0xD800..0xDFFF) - it
                     * has no valid UTF-8 encoding; only emit real scalar values. */
                    if(!(cp>=0xD800 && cp<=0xDFFF)) len += utf8_enc(cp, out+len, cap-1-len);
                } else {
                    out[len++]='u';   /* malformed \u -> keep the literal */
                }
            }
            else if(e) out[len++]=e;
            if(*p) p++;
        } else if(*p == '"'){
            break;            /* end of the JSON string */
        } else {
            out[len++] = *p++;
        }
    }
    out[len] = 0;
    return len;
}

static void *lyrics_thread(void *arg)
{
    ly_job_t *job = (ly_job_t *)arg;   /* our private snapshot; no shared-global reads */
    char cmd[1200];   /* holds two %-encoded query fields (2x512) + the wget URL template */
    static char resp[32768];           /* single worker at a time (inflight guard) -> static ok */
    resp[0] = 0;
    g_lbuf[0] = 0;                      /* sole writer until we publish; main reads after the lock */

    /* 1) local sibling .lrc FIRST - instant for offline users who sideloaded lyrics (no 12s wait) */
    if(job->path[0] && sd_io_begin()){
        char lrc[320]; derive_lrc(job->path, lrc, sizeof lrc);
        FILE *f = fopen(lrc, "r");
        if(f){ strip_lrc(f, g_lbuf, sizeof g_lbuf); fclose(f); }
        sd_io_end();
    }

    /* 2) fall back to online (lrclib) by track + artist */
    if(!g_lbuf[0] && job->title[0]){
        pthread_mutex_lock(&g_ly_mu); g_lphase = 1; pthread_mutex_unlock(&g_ly_mu);   /* -> poll shows "Searching online..." */
        if(job->artist[0])
            snprintf(cmd, sizeof cmd,
                "wget -qO- -T 12 'https://lrclib.net/api/search?track_name=%s&artist_name=%s' 2>/dev/null",
                job->title, job->artist);
        else
            snprintf(cmd, sizeof cmd,
                "wget -qO- -T 12 'https://lrclib.net/api/search?track_name=%s' 2>/dev/null", job->title);
        FILE *f = popen(cmd, "r");
        if(f){ int n = fread(resp, 1, sizeof(resp)-1, f); resp[n>0?n:0] = 0; pclose(f); }
        else fprintf(stderr, "lyrics popen failed: %s\n", strerror(errno));
        json_plain_lyrics(resp, g_lbuf, sizeof g_lbuf);
        if(f && !g_lbuf[0]) fprintf(stderr, "lyrics: lrclib returned no plainLyrics for '%s'\n", job->title);
    }

    if(!g_lbuf[0])
        snprintf(g_lbuf, sizeof g_lbuf, "No lyrics found.");

    /* publish atomically (pairs with lyrics_poll's lock so g_lbuf is visible) */
    pthread_mutex_lock(&g_ly_mu);
    g_done_req = job->req;
    g_linflight = 0;
    g_lready = 1;
    pthread_mutex_unlock(&g_ly_mu);
    free(job);
    return NULL;
}

/* Claim the single fetch slot and launch a worker with a snapshot of the current
 * request (req + query + path), all read on the main thread before pthread_create. */
static void start_fetch(void)
{
    int start = 0;
    pthread_mutex_lock(&g_ly_mu);
    if(!g_linflight){ g_linflight = 1; g_lready = 0; g_lphase = 0; start = 1; }
    pthread_mutex_unlock(&g_ly_mu);
    if(!start) return;
    ly_job_t *job = malloc(sizeof *job);
    if(!job){
        pthread_mutex_lock(&g_ly_mu); g_linflight = 0; pthread_mutex_unlock(&g_ly_mu);
        if(g_text) lv_label_set_text(g_text, "Lyrics unavailable.");   /* don't leave "Fetching..." stuck */
        return;
    }
    job->req = g_req;
    snprintf(job->title,  sizeof job->title,  "%s", g_q_title);
    snprintf(job->artist, sizeof job->artist, "%s", g_q_artist);
    snprintf(job->path,   sizeof job->path,   "%s", g_path);
    pthread_t th;
    if(pthread_create(&th, NULL, lyrics_thread, job) == 0) pthread_detach(th);
    else {
        free(job);
        pthread_mutex_lock(&g_ly_mu); g_linflight = 0; pthread_mutex_unlock(&g_ly_mu);
        if(g_text) lv_label_set_text(g_text, "Lyrics unavailable.");
    }
}

void lyrics_open(void)
{
    g_req++;   /* new request - supersedes any in-flight fetch for a previous track */
    /* resolve title / artist / path: live state, else resume track from DB */
    track_state_t st; ipc_get_state(&st);
    char title[200] = "", artist[200] = "";
    g_path[0] = 0;
    if(st.path[0]) snprintf(g_path, sizeof g_path, "%s", st.path);
    if(st.title[0])  snprintf(title,  sizeof title,  "%s", st.title);
    if(st.artist[0]) snprintf(artist, sizeof artist, "%s", st.artist);
    if(!title[0]){
        mdb_song_t cur; int pm, pl;
        if(mdb_current_play(&cur, &pm, &pl)){
            snprintf(title,  sizeof title,  "%s", cur.title);
            snprintf(artist, sizeof artist, "%s", cur.artist);
            if(!g_path[0]) mdb_song_path(cur.id, g_path, sizeof g_path);
        }
    }
    urlenc(title,  g_q_title,  sizeof g_q_title);
    urlenc(artist, g_q_artist, sizeof g_q_artist);

    lv_label_set_text(g_text, "Checking SD card...");
    lv_obj_scroll_to_y(g_scroll, 0, LV_ANIM_OFF);
    screen_show(SCR_LYRICS);

    if(g_q_title[0]) start_fetch();
    else             lv_label_set_text(g_text, "No track playing.");
}

void lyrics_poll(lv_timer_t *t)
{
    (void)t;
    /* If the track changed while the lyrics screen is open, reload for the new track
     * (g_path is the track we last fetched for). lyrics_open() re-resolves the live
     * track, bumps g_req to supersede any in-flight fetch, and updates g_path so this
     * fires once per change. Only while SCR_LYRICS is visible - no wasted fetches. */
    if(screen_current() == SCR_LYRICS){
        track_state_t st; ipc_get_state(&st);
        if(st.path[0] && strcmp(st.path, g_path) != 0){ lyrics_open(); return; }
    }
    int ready = 0, inflight, phase; unsigned done = 0;
    pthread_mutex_lock(&g_ly_mu);
    if(g_lready){ g_lready = 0; ready = 1; done = g_done_req; }
    inflight = g_linflight; phase = g_lphase;
    pthread_mutex_unlock(&g_ly_mu);
    if(!ready){
        /* still fetching: reflect the phase so the wait isn't a silent "Checking SD card..." */
        if(inflight && g_text) lv_label_set_text(g_text, phase ? "Searching online..." : "Checking SD card...");
        return;
    }
    if(done == g_req){
        /* result is for the current track; g_lbuf is visible (published under the lock)
         * and no worker is running, so reading it here is race-free. */
        lv_label_set_text(g_text, g_lbuf);
        lv_obj_scroll_to_y(g_scroll, 0, LV_ANIM_OFF);
    } else if(g_q_title[0]){
        /* finished fetch was for a superseded track - fetch the current one
         * (start_fetch no-ops if a newer fetch is already running). */
        (void)inflight;
        start_fetch();
    }
}

void lyrics_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    ui_header(root, "Lyrics");   /* shared standard header */

    g_scroll = lv_obj_create(root);
    lv_obj_remove_style_all(g_scroll);
    lv_obj_set_pos(g_scroll, 40, 76); lv_obj_set_size(g_scroll, 280, 250);
    lv_obj_set_style_pad_bottom(g_scroll, 44, 0);   /* last lyric lines scroll clear of the round bezel */
    lv_obj_set_style_bg_opa(g_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(g_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_scroll, LV_SCROLLBAR_MODE_OFF);

    g_text = lv_label_create(g_scroll);
    lv_obj_set_width(g_text, 280);
    lv_label_set_long_mode(g_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_text, ui_font_cjk(16), 0);   /* CJK lyrics render via Source Han Sans fallback */
    lv_obj_set_style_text_color(g_text, lv_color_hex(0xE5E5EA), 0);
    lv_label_set_text(g_text, "");
}
