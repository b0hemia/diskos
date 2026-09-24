/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include <stdatomic.h>
/* First-run / rescan music scanner for diskOS.
 *
 * The stock V2.09 scanner (tag 0622) is a no-op - it never populates song.db from the SD.
 * So diskOS scans itself: walk the SD for audio files, read their tags, and rebuild the
 * SONG table of /usr/data/fiio/db/song.db (the same DB the library UI reads). Playlists,
 * favourites and resume state (CUSTOM_PLAYLIST/PLAYLIST_INFO/MY_LOVE/MEMORY_PLAY) are keyed
 * by PATH and left intact, so a rescan doesn't lose them.
 *
 * Tag support: MP3 ID3v2.2/2.3/2.4 (title/artist/album/genre) + ID3v1 fallback; FLAC via its
 * VORBIS_COMMENT block; WAV by filename. Anything without usable tags falls back to the
 * filename (minus extension) as the title. m4a/aac/ogg/ape/dsf are not yet indexed.
 *
 * Runs on a detached worker thread; progress is published under a mutex for the UI to poll.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include "sqlite3.h"
#include "scanner.h"
#include "musicdb.h"   /* DISKOS_AUDIOBOOKS gate: whether .m4b files are indexed */

#ifndef DB_PATH
#define DB_PATH   "/usr/data/fiio/db/song.db"
#endif
#ifndef SCAN_ROOT
#define SCAN_ROOT "/tmp/sdcard"
#endif
#define MAXPATH   1024
#define TAGLEN    256

/* ---- progress (published to the LVGL thread) ---- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_active;          /* a scan is running */
static int g_done;            /* files inserted so far */
static int g_total;           /* files found (0 until the walk completes) */
static int g_finished_seq;    /* bumped when a scan finishes (UI edge-detect) */
static int g_no_sd;           /* last scan aborted because the SD wasn't mounted */
static _Atomic int g_abort;   /* set from the UI thread to stop a running walk (SD revoked); atomic, not volatile: two threads */
static int g_scan_err;        /* worker-thread only: any I/O or DB-insert failure during the walk
                               * -> the rebuild is incomplete and must NOT commit over the library */
static int g_skipped_result; /* completed scan snapshot under g_mu */
static int g_skipped;         /* files skipped this scan (unstat-able junk / overlong path) - logged, not fatal */
static int g_unsupported;     /* audio-ish files present but not indexable (AAC/M4A/OGG/...) - drives a UI notice */
static int g_prune_blocked;   /* an OVERLONG (unknowable) path was skipped -> its row can't be preserved in
                               * `seen`, so the vanished-row prune must not run this scan. ENOENT/unreadable
                               * audio files ARE preserved (added to seen), so they don't block the prune. */

int scanner_active(void){ pthread_mutex_lock(&g_mu); int a=g_active; pthread_mutex_unlock(&g_mu); return a; }
void scanner_progress(int *done, int *total){
    pthread_mutex_lock(&g_mu);
    if(done)*done=g_done; if(total)*total=g_total;
    pthread_mutex_unlock(&g_mu);
}
int scanner_take_finished(void){
    pthread_mutex_lock(&g_mu);
    static int last=0; int f = (g_finished_seq!=last); last=g_finished_seq;
    pthread_mutex_unlock(&g_mu);
    return f;
}
/* 1 if the most recent scan did nothing because no SD was mounted (library was kept). */
int scanner_no_sd(void){ pthread_mutex_lock(&g_mu); int v=g_no_sd; pthread_mutex_unlock(&g_mu); return v; }
int scanner_skipped(void){ pthread_mutex_lock(&g_mu); int v=g_skipped_result; pthread_mutex_unlock(&g_mu); return v; }
int scanner_unsupported(void){ pthread_mutex_lock(&g_mu); int v=g_unsupported; pthread_mutex_unlock(&g_mu); return v; }

/* ---- helpers ---- */
static void str_trim(char *s){
    char *p=s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s,p,strlen(p)+1);
    int n=(int)strlen(s);
    while(n>0 && (unsigned char)s[n-1]<=' ') s[--n]=0;
}
/* NAME_CODE/TITLE_CODE/... : first 4 chars packed big-endian, 31-bit (matches the stock DB) */
static int code4(const char *s){
    char b[5]="\0\0\0\0"; int j=0;
    for(const char *p=s?s:""; *p && j<4; p++) b[j++]=(char)tolower((unsigned char)*p);
    unsigned v=0; for(int i=0;i<4;i++) v=(v<<8)|((unsigned char)b[i]);
    return (int)(v & 0x7fffffff);
}
static int has_ext(const char *name, const char *ext){
    size_t nl=strlen(name), el=strlen(ext);
    return nl>el && !strcasecmp(name+nl-el, ext);
}
/* Audio files diskOS indexes. MP3 (ID3) + FLAC (VORBIS_COMMENT) + M4A/M4B (MP4 iTunes atoms) get real
 * tags; WAV falls back to the filename. aac/ogg/ape/dsf need their own parsers - a documented limitation. */
static int is_audio(const char *name){
    if(has_ext(name,".m4b")) return DISKOS_AUDIOBOOKS;   /* books indexed only when the audiobook feature is enabled */
    return has_ext(name,".mp3") || has_ext(name,".flac") || has_ext(name,".wav")
        || has_ext(name,".m4a");
}
/* Audio-ish files we do NOT index yet (no parser). Counted during the walk so the UI can tell a user
 * whose library is all AAC/ALAC/etc WHY it looks empty, instead of a bare "No music found". */
static int is_unsupported_audio(const char *name){
    return has_ext(name,".aac") || has_ext(name,".ogg") || has_ext(name,".oga")
        || has_ext(name,".opus")|| has_ext(name,".ape") || has_ext(name,".dsf") || has_ext(name,".dff")
        || has_ext(name,".aif") || has_ext(name,".aiff")|| has_ext(name,".wma") || has_ext(name,".alac")
        || has_ext(name,".wv");  /* WavPack: not indexed (unsupported); stock V2.57 also dropped its decoder */
}

/* ---- text encoding -> UTF-8 (bounded) ---- */
static void put_u8(char **o, char *end, unsigned cp){
    char *p=*o;
    if(cp<0x80){ if(p<end) *p++=(char)cp; }
    else if(cp<0x800){ if(p+1<end){ *p++=(char)(0xC0|(cp>>6)); *p++=(char)(0x80|(cp&0x3F)); } }
    else { if(p+2<end){ *p++=(char)(0xE0|(cp>>12)); *p++=(char)(0x80|((cp>>6)&0x3F)); *p++=(char)(0x80|(cp&0x3F)); } }
    *o=p;
}
/* Decode an ID3 text-frame body (enc byte already consumed by caller). enc: 0=Latin1,
 * 1=UTF-16 w/ BOM, 2=UTF-16BE, 3=UTF-8. Writes a NUL-terminated UTF-8 string into out. */
static void id3_decode(int enc, const unsigned char *in, int n, char *out, int cap){
    char *o=out, *end=out+cap-1;
    if(enc==0){                                  /* Latin-1 */
        for(int i=0;i<n && in[i];i++) put_u8(&o,end,in[i]);
    } else if(enc==3){                           /* UTF-8 (copy, stop at NUL) */
        for(int i=0;i<n && in[i];i++){ if(o<end) *o++=(char)in[i]; }
    } else {                                     /* UTF-16 (1=BOM, 2=BE) */
        int be = (enc==2), i=0;
        if(enc==1 && n>=2){ if(in[0]==0xFF && in[1]==0xFE) be=0; else if(in[0]==0xFE && in[1]==0xFF) be=1; i=2; }
        for(; i+1<n; i+=2){
            unsigned u = be ? (in[i]<<8|in[i+1]) : (in[i+1]<<8|in[i]);
            if(u==0) break;
            if(u>=0xD800 && u<=0xDBFF && i+3<n){  /* surrogate pair */
                unsigned lo = be ? (in[i+2]<<8|in[i+3]) : (in[i+3]<<8|in[i+2]);
                if(lo>=0xDC00 && lo<=0xDFFF){ u=0x10000+((u-0xD800)<<10)+(lo-0xDC00); i+=2;
                    char *p=o; if(p+3<end){ *p++=(char)(0xF0|(u>>18)); *p++=(char)(0x80|((u>>12)&0x3F)); *p++=(char)(0x80|((u>>6)&0x3F)); *p++=(char)(0x80|(u&0x3F)); } o=p; continue; }
            }
            if(u>=0xD800 && u<=0xDFFF) continue;  /* lone surrogate */
            put_u8(&o,end,u);
        }
    }
    *o=0; str_trim(out);
}
/* "(13)" or "13" style ID3 numeric genre -> leave as-is if it's plain text; we don't map the
 * 148 legacy IDs (rarely used in modern tags), just strip a leading "(N)" wrapper. */
static void genre_clean(char *g){
    if(g[0]=='(' ){ char *e=strchr(g,')'); if(e && e[1]) memmove(g, e+1, strlen(e+1)+1); }
    str_trim(g);
}

/* unsigned shifts: p[0]<<24 with the byte >=128 would shift into the int sign bit (signed-overflow UB). */
static uint32_t be32(const unsigned char *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]; }
static uint32_t synch32(const unsigned char *p){ return ((uint32_t)p[0]<<21)|((uint32_t)p[1]<<14)|((uint32_t)p[2]<<7)|(uint32_t)p[3]; }

/* leading positive integer of an ID3/Vorbis track/disc value ("5" or "5/12" -> 5, "01" -> 1). Returns 0
 * for junk, overflow, or a non-positive value, so a bogus tag can never store garbage or clobber a good
 * existing value (the UPDATE keeps the old value when this is 0). */
static int tag_int(const char *s){
    if(!s) return 0;
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if(end == s || errno == ERANGE || v <= 0 || v > INT_MAX) return 0;
    return (int)v;
}

/* Read ID3v2 text frames (TIT2/TPE1/TALB/TCON/TRCK/TPOS/TPE2, or the v2.2 3-char ids). Returns 1 if the
 * header was present. Fields it doesn't find are left untouched. */
static int id3v2_read(FILE *f, char *title,char *artist,char *album,char *genre,
                      char *album_artist, int *track, int *disc, int *rerr){
    unsigned char h[10];
    if(fread(h,1,10,f)!=10) return 0;
    if(memcmp(h,"ID3",3)!=0) return 0;
    int ver=h[3];
    int unsync=(h[5]&0x80)!=0, exthdr=(h[5]&0x40)!=0;
    long tagsize = synch32(h+6);
    if(tagsize<=0 || tagsize>20*1024*1024) return 1;
    unsigned char *buf=malloc((size_t)tagsize);
    if(!buf){ if(rerr)*rerr=1; return 1; }                                     /* OOM: signal a real read failure (ferror won't catch it) */
    if(fread(buf,1,(size_t)tagsize,f)!=(size_t)tagsize){ free(buf); return 1; }   /* short read: ferror() in tags_from_file catches a real IO error */
    long p=0;
    if(exthdr && ver>=3 && tagsize>=6){           /* skip the extended header if present */
        if(ver>=4){ long es=synch32(buf);      if(es>0 && es<=tagsize) p+=es; }        /* v2.4: size incl itself */
        else      { long es=be32(buf);         if(es>0 && es<=tagsize-4) p+=4+es; }    /* v2.3: excl the 4 size bytes */
    }
    (void)unsync;
    int idlen = (ver==2)?3:4, fhdr=(ver==2)?6:10;
    while(p + fhdr <= tagsize){
        char id[5]={0}; memcpy(id, buf+p, idlen);
        if(id[0]==0) break;                       /* padding */
        long fsize;
        if(ver==2) fsize = (buf[p+3]<<16)|(buf[p+4]<<8)|buf[p+5];
        else if(ver==4) fsize = synch32(buf+p+4);
        else fsize = be32(buf+p+4);
        if(fsize<=0 || fsize > tagsize - p - fhdr) break;   /* overflow-safe (tagsize-p-fhdr >= 0) */
        const unsigned char *body = buf+p+fhdr;
        if(id[0]=='T' && fsize>=1){               /* text frame: enc byte + text */
            int enc=body[0];
            char val[TAGLEN]; id3_decode(enc, body+1, (int)fsize-1, val, sizeof val);
            if(val[0]){
                const char *k = id;
                if(!strcmp(k,"TIT2")||!strcmp(k,"TT2")) snprintf(title,TAGLEN,"%s",val);
                else if(!strcmp(k,"TPE1")||!strcmp(k,"TP1")) snprintf(artist,TAGLEN,"%s",val);
                else if(!strcmp(k,"TALB")||!strcmp(k,"TAL")) snprintf(album,TAGLEN,"%s",val);
                else if(!strcmp(k,"TCON")||!strcmp(k,"TCO")){ snprintf(genre,TAGLEN,"%s",val); genre_clean(genre); }
                else if(!strcmp(k,"TRCK")||!strcmp(k,"TRK")) *track = tag_int(val);  /* "5" or "5/12" */
                else if(!strcmp(k,"TPOS")||!strcmp(k,"TPA")) *disc  = tag_int(val);  /* part-of-set */
                else if(!strcmp(k,"TPE2")||!strcmp(k,"TP2")){ if(!album_artist[0]) snprintf(album_artist,TAGLEN,"%s",val); }
            }
        }
        p += fhdr + fsize;
    }
    free(buf);
    return 1;
}
/* ID3v1: last 128 bytes "TAG" + 30+30+30 title/artist/album (Latin-1). Fallback only. */
static void id3v1_read(FILE *f, char *title,char *artist,char *album, int *rerr){
    /* fseek does NOT set the stream error indicator, so ferror() won't catch a seek IO failure.
     * A file smaller than 128 bytes fails this seek with EINVAL (benign -> just no ID3v1); only a
     * real media error (EIO) should preserve the existing row. */
    if(fseek(f,-128,SEEK_END)!=0){ if(rerr && errno==EIO) *rerr=1; return; }
    unsigned char t[128];
    if(fread(t,1,128,f)!=128) return;
    if(memcmp(t,"TAG",3)!=0) return;
    char tmp[64];
    if(!title[0]){  id3_decode(0, t+3,  30, tmp,sizeof tmp); if(tmp[0]) snprintf(title, TAGLEN,"%s",tmp); }
    if(!artist[0]){ id3_decode(0, t+33, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(artist,TAGLEN,"%s",tmp); }
    if(!album[0]){  id3_decode(0, t+63, 30, tmp,sizeof tmp); if(tmp[0]) snprintf(album, TAGLEN,"%s",tmp); }
}

static uint32_t le32(const unsigned char *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* FLAC: "fLaC" magic + metadata blocks; parse the VORBIS_COMMENT block (type 4) for
 * TITLE/ARTIST/ALBUM/GENRE (UTF-8, little-endian lengths). Bounded like the ID3 path. */
static void flac_read(FILE *f, char *title,char *artist,char *album,char *genre,
                      char *album_artist, int *track, int *disc, int *rerr){
    unsigned char magic[4];
    if(fread(magic,1,4,f)!=4 || memcmp(magic,"fLaC",4)!=0) return;
    for(int guard=0; guard<256; guard++){
        unsigned char h[4];
        if(fread(h,1,4,f)!=4) return;
        int last = h[0]&0x80, type = h[0]&0x7f;
        uint32_t len = ((uint32_t)h[1]<<16)|((uint32_t)h[2]<<8)|h[3];
        if(type==4){                                    /* VORBIS_COMMENT */
            if(len<8 || len>1024*1024) return;
            unsigned char *b=malloc(len); if(!b){ if(rerr)*rerr=1; return; }   /* OOM: signal a real read failure (ferror won't catch it) */
            if(fread(b,1,len,f)!=len){ free(b); return; }                      /* short read: ferror() in tags_from_file distinguishes IO error from EOF */
            /* overflow-safe bounds: len>=8 guaranteed above. Need 4(vendor-len field, = b[0..3])
             * + vlen(vendor) + 4(comment count) <= len, i.e. vlen <= len-8. Use subtraction so a
             * hostile vlen like 0xFFFFFFFA can't wrap an addition past the guard. */
            uint32_t vlen=le32(b);
            if(vlen > len-8){ free(b); return; }
            uint32_t off=4+vlen;                          /* off <= len-4, so le32(b+off) is in-bounds */
            uint32_t cnt=le32(b+off); off+=4;
            for(uint32_t i=0;i<cnt && off+4<=len;i++){
                uint32_t clen=le32(b+off); off+=4;
                if(clen>len-off) break;                  /* overflow-safe (len-off >= 0) */
                char kv[1088];
                if(clen < sizeof kv){
                    memcpy(kv,b+off,clen); kv[clen]=0;
                    char *eq=strchr(kv,'=');
                    if(eq){ *eq=0; const char *k=kv, *v=eq+1;
                        if(!strcasecmp(k,"TITLE")  && !title[0])  snprintf(title, TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"ARTIST") && !artist[0]) snprintf(artist,TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"ALBUM")  && !album[0])  snprintf(album, TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"GENRE")  && !genre[0])  snprintf(genre, TAGLEN,"%s",v);
                        else if(!strcasecmp(k,"TRACKNUMBER") && !*track) *track = tag_int(v);
                        else if(!strcasecmp(k,"DISCNUMBER")  && !*disc)  *disc  = tag_int(v);
                        else if((!strcasecmp(k,"ALBUMARTIST")||!strcasecmp(k,"ALBUM ARTIST")) && !album_artist[0])
                            snprintf(album_artist,TAGLEN,"%s",v);
                    }
                }
                off+=clen;
            }
            free(b);
            return;                                      /* got the comment block */
        }
        if(last) return;
        if(fseek(f,(long)len,SEEK_CUR)!=0){ if(rerr && errno==EIO) *rerr=1; return; }   /* seek IO error (ferror won't catch it) */
    }
}

/* ---- MP4 / M4A (iTunes-style metadata) ----
 * Walk the atom tree moov -> udta -> meta -> ilst and read the text/number tag atoms. Fully bounded:
 * every atom size is validated against its parent before use, so a malformed/hostile file can't read
 * out of the buffer. Like the other readers, duration is not extracted (DURATION is stored 0). */

/* Find the first child atom of `type` in buf[pos..end). On success set cs/ce to its PAYLOAD range and
 * return 1. Handles 32-bit, 64-bit (size==1) and to-end (size==0) atoms; rejects anything that overruns. */
static int mp4_child(const unsigned char *buf, long pos, long end, const char *type, long *cs, long *ce){
    while(pos + 8 <= end){
        uint64_t sz = be32(buf + pos);
        long hdr = 8;
        if(sz == 1){                                    /* 64-bit extended size */
            if(pos + 16 > end) break;
            sz = ((uint64_t)be32(buf + pos + 8) << 32) | be32(buf + pos + 12);
            hdr = 16;
        } else if(sz == 0){                             /* runs to the end of the parent */
            sz = (uint64_t)(end - pos);
        }
        if(sz < (uint64_t)hdr) break;                   /* smaller than its own header */
        if(sz > (uint64_t)(end - pos)) break;           /* overruns the parent */
        if(memcmp(buf + pos + 4, type, 4) == 0){ *cs = pos + hdr; *ce = pos + (long)sz; return 1; }
        pos += (long)sz;
    }
    return 0;
}

static void mp4_str(const unsigned char *v, long vn, char *out){
    long n = vn < TAGLEN - 1 ? vn : TAGLEN - 1;         /* iTunes text atoms are UTF-8 */
    if(n < 0) n = 0;
    memcpy(out, v, (size_t)n); out[n] = 0; str_trim(out);
}

/* `key` = the 4-byte ilst atom id; v/vn = the value inside its `data` atom (past the 8-byte type/flags). */
static void mp4_take(const unsigned char *key, const unsigned char *v, long vn,
                     char *title,char *artist,char *album,char *genre,char *album_artist,int *track,int *disc){
    if(vn <= 0) return;
    if     (key[0]==0xA9 && key[1]=='n'&&key[2]=='a'&&key[3]=='m' && !title[0])        mp4_str(v,vn,title);
    else if(key[0]==0xA9 && key[1]=='A'&&key[2]=='R'&&key[3]=='T' && !artist[0])       mp4_str(v,vn,artist);
    else if(key[0]==0xA9 && key[1]=='a'&&key[2]=='l'&&key[3]=='b' && !album[0])        mp4_str(v,vn,album);
    else if(key[0]==0xA9 && key[1]=='g'&&key[2]=='e'&&key[3]=='n' && !genre[0])        mp4_str(v,vn,genre);
    else if(!memcmp(key,"aART",4) && !album_artist[0])                                 mp4_str(v,vn,album_artist);
    else if(!memcmp(key,"trkn",4) && vn>=4){ int t = ((int)v[2]<<8)|v[3]; if(t>0) *track = t; }  /* binary: [2 rsv][2 num][2 tot] */
    else if(!memcmp(key,"disk",4) && vn>=4){ int d = ((int)v[2]<<8)|v[3]; if(d>0) *disc  = d; }
}

/* Stream-walk the atoms in the file range [start,end) looking for a child of `type`, WITHOUT reading
 * any atom body: siblings (a multi-MB mdat or a long book's trak sample tables) are seek-skipped, so
 * this costs a handful of 8-byte header reads no matter how big the file is. On success set *cpay (the
 * child's PAYLOAD file offset) + *cpsz (payload size) and return 1. Handles 32-bit, 64-bit (size==1)
 * and to-end (size==0) atoms; guards every offset against overflow/rewind so a hostile size can't loop
 * forever or rewind. fseek/ftell failures set rerr (ferror never catches those). */
/* off_t (64-bit on musl for every arch) + fseeko/ftello so a book >=2GiB is walked correctly instead of
 * failing the whole tag/chapter read on a 32-bit `long` file offset. */
static int mp4_find_child(FILE *f, off_t start, off_t end, const char *type,
                          off_t *cpay, uint64_t *cpsz, int *rerr){
    if(fseeko(f, start, SEEK_SET) != 0){ if(rerr) *rerr = 1; return 0; }
    for(;;){
        off_t here = ftello(f);
        if(here < 0){ if(rerr) *rerr = 1; return 0; }
        if(here >= end || end - here < 8) break;          /* no room for another header (end-here avoids here+8 overflow near the max) */
        unsigned char h[16];
        if(fread(h,1,8,f) != 8) break;                   /* clean EOF/short (ferror catches real IO) */
        uint64_t sz = be32(h); int hdr = 8;
        if(sz == 1){ if(end - here < 16 || fread(h+8,1,8,f)!=8) break; sz = ((uint64_t)be32(h+8)<<32)|be32(h+12); hdr = 16; }
        else if(sz == 0) sz = (uint64_t)(end - here);    /* runs to the end of the parent */
        if(sz < (uint64_t)hdr) break;                    /* smaller than its own header */
        if(sz > (uint64_t)(end - here)) break;           /* overruns the parent */
        if(memcmp(h+4,type,4)==0){ *cpay = here + hdr; *cpsz = sz - (uint64_t)hdr; return 1; }
        if(sz > (uint64_t)(INT64_MAX - here)) break;     /* next offset would overflow/rewind */
        if(fseeko(f, here + (off_t)sz, SEEK_SET) != 0){ if(rerr) *rerr = 1; return 0; }
    }
    return 0;
}

/* Locate moov/udta and load ONLY the udta payload into a fresh malloc'd buffer (caller frees). All the
 * tags + chapters + cover live under udta; the multi-MB sample tables (trak) never touch RAM. This keeps
 * the allocation tiny (a few KB, plus an embedded cover if present) even for a 20-hour book, instead of
 * loading a whole many-MB moov on a ~19MB-free device. Returns 1 with out_buf + out_sz, else 0. */
static int mp4_load_udta(FILE *f, unsigned char **out_buf, uint64_t *out_sz, int *rerr){
    if(fseeko(f, 0, SEEK_END) != 0){ if(rerr) *rerr = 1; return 0; }
    off_t fsz = ftello(f);
    if(fsz < 0){ if(rerr) *rerr = 1; return 0; }   /* ftello error -> a real IO failure, not "no metadata" */
    if(fsz < 8) return 0;
    off_t moov_pay; uint64_t moov_psz;
    if(!mp4_find_child(f, 0, fsz, "moov", &moov_pay, &moov_psz, rerr)) return 0;
    off_t moov_end = moov_pay + (off_t)moov_psz;         /* find_child already bounded moov to the file */
    off_t udta_pay; uint64_t udta_sz;
    if(!mp4_find_child(f, moov_pay, moov_end, "udta", &udta_pay, &udta_sz, rerr)) return 0;
    if(udta_sz == 0) return 0;                           /* genuinely no udta -> no tags (filename fallback is correct) */
    if(udta_sz > 4u*1024*1024){ if(rerr) *rerr = 1; return 0; }  /* too big to load (e.g. a music m4a with multi-MB cover art) -> a READ failure, not "no tags": preserve the existing row instead of clobbering good title/artist/album with the filename fallback */
    unsigned char *buf = malloc((size_t)udta_sz);
    if(!buf){ if(rerr) *rerr = 1; return 0; }            /* OOM: a real read failure (don't clobber the row) */
    if(fseeko(f, udta_pay, SEEK_SET) != 0){ if(rerr) *rerr = 1; free(buf); return 0; }
    if(fread(buf,1,(size_t)udta_sz,f) != (size_t)udta_sz){ free(buf); return 0; }
    *out_buf = buf; *out_sz = udta_sz;
    return 1;
}

static void mp4_read(FILE *f, char *title,char *artist,char *album,char *genre,
                     char *album_artist, int *track, int *disc, int *rerr){
    unsigned char *buf; uint64_t udta_sz;
    if(!mp4_load_udta(f, &buf, &udta_sz, rerr)) return;
    long mts,mte, is,ie;
    if(mp4_child(buf, 0, (long)udta_sz, "meta", &mts, &mte) &&        /* buf IS the udta payload */
       /* the iTunes `meta` atom carries 4 version/flags bytes before its children; some writers omit them */
       (mp4_child(buf, mts+4, mte, "ilst", &is, &ie) || mp4_child(buf, mts, mte, "ilst", &is, &ie))){
        long p = is;
        while(p + 8 <= ie){
            uint64_t sz = be32(buf+p); long hdr = 8;
            if(sz == 1){ if(p+16 > ie) break; sz = ((uint64_t)be32(buf+p+8)<<32)|be32(buf+p+12); hdr = 16; }
            else if(sz == 0) sz = (uint64_t)(ie - p);
            if(sz < (uint64_t)hdr || sz > (uint64_t)(ie - p)) break;
            long dts, dte;
            if(mp4_child(buf, p+hdr, p+(long)sz, "data", &dts, &dte) && dte - dts >= 8)
                mp4_take(buf+p+4, buf+dts+8, dte-dts-8, title,artist,album,genre,album_artist,track,disc);
            p += (long)sz;
        }
    }
    free(buf);
}

/* Read Nero 'chpl' chapters (moov/udta/chpl) from an M4B/M4A into out[0..max); returns the count (0 if
 * none/unsupported/malformed). Handles chpl version 0 (count at payload+4) and version 1 (count at
 * payload+8); every field is bounds-checked against the atom. Start times are 100ns ticks -> ms. Titles
 * are length-prefixed UTF-8 (no NUL), truncated only at a code-point boundary; an empty title becomes
 * "Chapter N". QuickTime chapter-track chapters are NOT read here (a later stage). */
static int scan_read_chapters_leased(const char *path, chapter_t *out, int max){
    if(!path || !out || max <= 0) return 0;
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    unsigned char *buf; uint64_t udta_sz; int rerr = 0;
    int ok = mp4_load_udta(f, &buf, &udta_sz, &rerr);
    fclose(f);
    if(!ok) return 0;
    int n = 0;
    long cs, ce;
    if(mp4_child(buf, 0, (long)udta_sz, "chpl", &cs, &ce) && ce - cs >= 5){   /* buf IS the udta payload */
        int ver = buf[cs];
        long cnt_off = (ver == 1) ? 8 : (ver == 0) ? 4 : -1;   /* count position by version; reject others */
        if(cnt_off >= 0 && ce - cs > cnt_off){
            int count = buf[cs + cnt_off];
            long q = cs + cnt_off + 1;
            int bad = 0;
            long prev_ms = -1;                                 /* chapters must be chronological */
            for(int i = 0; i < count; i++){                    /* validate EVERY declared entry */
                if(q + 9 > ce){ bad = 1; break; }              /* 8-byte start + 1-byte length */
                uint64_t ticks = ((uint64_t)be32(buf+q) << 32) | be32(buf+q+4); q += 8;
                int tl = buf[q]; q += 1;
                if(q + tl > ce){ bad = 1; break; }
                uint64_t ms64 = ticks / 10000;                 /* 100ns ticks -> ms */
                if(ms64 > (uint64_t)LONG_MAX){ bad = 1; break; }  /* unrepresentable on a 32-bit long */
                if((long)ms64 < prev_ms){ bad = 1; break; }    /* out-of-order start -> reject the whole list (the UI assumes ascending) */
                prev_ms = (long)ms64;
                if(n < max){                                   /* store up to max, but keep validating the rest */
                    out[n].start_ms = (long)ms64;
                    int cl = tl;
                    if(cl > CHAP_TITLE - 1){ cl = CHAP_TITLE - 1; while(cl > 0 && (buf[q+cl] & 0xC0) == 0x80) cl--; }  /* don't split UTF-8 */
                    memcpy(out[n].title, buf+q, (size_t)cl); out[n].title[cl] = 0;
                    str_trim(out[n].title);
                    if(!out[n].title[0]) snprintf(out[n].title, CHAP_TITLE, "Chapter %d", n+1);
                    n++;
                }
                q += tl;
            }
            if(bad) n = 0;                                     /* a malformed/truncated list -> report none, not a partial one */
        }
    }
    free(buf);
    return n;
}

/* Read the narrator of an m4b into out[0..cap): the iTunes composer atom (©wrt), the near-universal
 * convention for an audiobook's narrator. Returns 1 if a non-empty value was found. Reuses the udta-only
 * loader + bounded ilst walk. */
static int scan_read_narrator_leased(const char *path, char *out, int cap){
    if(!path || !out || cap <= 0) return 0;
    out[0] = 0;
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    unsigned char *buf; uint64_t udta_sz; int rerr = 0;
    int ok = mp4_load_udta(f, &buf, &udta_sz, &rerr);
    fclose(f);
    if(!ok) return 0;
    long mts, mte, is, ie; int found = 0;
    if(mp4_child(buf, 0, (long)udta_sz, "meta", &mts, &mte) &&
       (mp4_child(buf, mts+4, mte, "ilst", &is, &ie) || mp4_child(buf, mts, mte, "ilst", &is, &ie))){
        long p = is;
        while(p + 8 <= ie){
            uint64_t sz = be32(buf+p); long hdr = 8;
            if(sz == 1){ if(p+16 > ie) break; sz = ((uint64_t)be32(buf+p+8)<<32)|be32(buf+p+12); hdr = 16; }
            else if(sz == 0) sz = (uint64_t)(ie - p);
            if(sz < (uint64_t)hdr || sz > (uint64_t)(ie - p)) break;
            const unsigned char *key = buf + p + 4;
            if(key[0]==0xA9 && key[1]=='w' && key[2]=='r' && key[3]=='t'){   /* ©wrt = composer = narrator */
                long dts, dte;
                if(mp4_child(buf, p+hdr, p+(long)sz, "data", &dts, &dte) && dte - dts >= 8){
                    long vn = dte - dts - 8; if(vn > cap-1) vn = cap-1; if(vn < 0) vn = 0;
                    memcpy(out, buf+dts+8, (size_t)vn); out[vn] = 0; str_trim(out);
                    found = out[0] != 0;
                }
                break;
            }
            p += (long)sz;
        }
    }
    free(buf);
    return found;
}

/* Read tags + fill a filename fallback. Return code drives how the caller writes the row:
 *   1  = tags read (or a no-tag container like WAV) -> safe to INSERT a new row OR UPDATE in place.
 *   0  = a tag-bearing file that could NOT be OPENED (transient I/O) -> preserve any existing row and
 *        do NOT insert a new one (title..genre are left empty; a file we can't open at all is skipped).
 *  -1  = OPENED but its tags could not be LOADED (a mid-read I/O error, or oversized/hostile atoms):
 *        title..genre ARE filled with the filename fallback. The caller must NOT overwrite an existing
 *        row (never clobber good tags), but may INSERT a fallback row when the path is new so the file
 *        is still playable. */
static int tags_from_file(const char *path, const char *fname,
                          char *title,char *artist,char *album,char *genre,
                          char *album_artist, int *track, int *disc){
    title[0]=artist[0]=album[0]=genre[0]=album_artist[0]=0; *track=0; *disc=0;
    int rerr=0;
    FILE *f=fopen(path,"rb");
    if(!f && (has_ext(fname,".flac") || has_ext(fname,".mp3") || has_ext(fname,".m4a") || has_ext(fname,".m4b")))
        return 0;   /* can't even open a tag-bearing file -> skip it entirely (preserve existing, no insert) */
    if(f){
        if(has_ext(fname,".flac")) flac_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        else if(has_ext(fname,".mp3")){ id3v2_read(f,title,artist,album,genre,album_artist,track,disc,&rerr); id3v1_read(f,title,artist,album,&rerr); }
        else if(has_ext(fname,".m4a") || has_ext(fname,".m4b")) mp4_read(f,title,artist,album,genre,album_artist,track,disc,&rerr);
        /* .wav (and any other accepted container) -> filename fallback below; no tag probing,
         * so a WAV whose last 128 bytes happen to start with "TAG" isn't misread as ID3v1. */
        if(ferror(f)) rerr=1;   /* ANY stream IO error during tag reads (headers/seeks/body) -> a read failure,
                                 * not "no tags" (feof/clean EOF does NOT set this, so a short valid file still
                                 * falls back to filename). Catches the header/seek paths rerr doesn't. */
        fclose(f);
    }
    str_trim(album_artist);   /* whitespace-only album-artist -> empty, so it never overwrites a good value */
    if(!title[0]){                               /* filename (minus extension) */
        snprintf(title,TAGLEN,"%s",fname);
        char *dot=strrchr(title,'.'); if(dot) *dot=0;
        str_trim(title);
    }
    if(!artist[0]) snprintf(artist,TAGLEN,"%s","Unknown artist");
    if(!album[0])  snprintf(album, TAGLEN,"%s","Unknown album");
    if(!genre[0])  snprintf(genre, TAGLEN,"%s","Unknown genre");
    return rerr ? -1 : 1;   /* -1: fallback-only (preserve existing, insert-if-new); 1: normal upsert */
}

/* ---- SQLite: schema + insert ---- */
static const char *SCHEMA_SONG =
    "CREATE TABLE IF NOT EXISTS SONG (ID INTEGER PRIMARY KEY autoincrement, PATH TEXT, NAME TEXT,"
    "TITLE TEXT, ALBUM TEXT, ARTIST TEXT, GENRE TEXT, DISC INT, TRACK INT, IS_CUE INT, IS_ISO INT,"
    "IS_DSD INT, OFFSET BIGINT, DURATION BIGINT, NAME_CODE INT, TITLE_CODE INT, ALBUM_CODE INT,"
    "ARTIST_CODE INT, GENRE_CODE INT, ADD_TIME INT8, SAMPLE_RATE INT, BIT_PER_SAMPLE INT, CHANNELS INT,"
    "BIT_RATE INT, SONG_MIMETYPE TEXT, SONG_PRODUCTION_YEAR TEXT, IS_SELECT INT, ALBUM_ARTIST TEXT,"
    /* IS_M3U/M3U_PATH match the V2.28 stock SONG superset (its playback SQL SELECTs them); ACCENT is
     * our private per-song art-accent cache. TEXT for ALBUM_ARTIST matches stock (was wrongly INT). */
    "ALBUM_ARTIST_CODE INT, IS_M3U INT DEFAULT 0, M3U_PATH TEXT DEFAULT '', ACCENT INTEGER DEFAULT 0);";
static const char *INS_SONG =
    "INSERT INTO SONG (PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
    "DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,"
    "BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,"
    /* Explicit ?N params so the existing bind_tags indices are unchanged: 1=PATH, 2..6=tags, 7..11=codes,
     * 12=ADD_TIME; 13=DISC, 14=TRACK, 15=ALBUM_ARTIST (NULL when absent), 16=ALBUM_ARTIST_CODE. DISC/TRACK/
     * ALBUM_ARTIST were hardcoded 0/0/NULL before (the 2026-09-05 audit flagged the missing metadata).
     * IS_M3U/M3U_PATH/ACCENT are omitted so they take their schema DEFAULTs. */
    "ALBUM_ARTIST_CODE) VALUES (?1,?2,?3,?4,?5,?6,?13,?14,0,0,0,0,0,?7,?8,?9,?10,?11,?12,0,0,0,0,'','',0,?15,?16);";
/* MERGE (not delete+reinsert): UPDATE an existing PATH's metadata in place so its ID and ACCENT are
 * preserved - MEMORY_PLAY.MUSIC_ID (resume) and MY_LOVE.ID (favourites) are ID-keyed, so a delete+
 * reinsert with fresh autoincrement IDs used to break resume + favourites and wipe the art-accent cache
 * on every rescan. ADD_TIME is intentionally left untouched here (keep the original add time). */
static const char *UPD_SONG =
    /* Explicit ?N: 1..5=tags, 6..10=codes, 11=PATH (WHERE); 12=DISC, 13=TRACK, 14=ALBUM_ARTIST,
     * 15=ALBUM_ARTIST_CODE. Now re-populates disc/track/album-artist on every rescan too (was omitted,
     * so existing rows never gained the metadata). */
    "UPDATE SONG SET NAME=?1,TITLE=?2,ALBUM=?3,ARTIST=?4,GENRE=?5,"
    "NAME_CODE=?6,TITLE_CODE=?7,ALBUM_CODE=?8,ARTIST_CODE=?9,GENRE_CODE=?10,"
    /* Only FILL IN disc/track/album-artist we actually extracted; keep any existing (e.g. stock-written)
     * value when our read yields nothing, so a rescan never clobbers richer metadata with 0/NULL. */
    "DISC=CASE WHEN ?12>0 THEN ?12 ELSE DISC END,"
    "TRACK=CASE WHEN ?13>0 THEN ?13 ELSE TRACK END,"
    "ALBUM_ARTIST=CASE WHEN ?14 IS NOT NULL THEN ?14 ELSE ALBUM_ARTIST END,"
    "ALBUM_ARTIST_CODE=CASE WHEN ?14 IS NOT NULL THEN ?15 ELSE ALBUM_ARTIST_CODE END "
    "WHERE PATH=?11;";
/* per-connection temp table of PATHs seen this scan; drives the post-walk delete of vanished songs. */
static const char *SEEN_DDL = "CREATE TEMP TABLE IF NOT EXISTS seen(PATH TEXT PRIMARY KEY);";
static const char *SEEN_INS = "INSERT OR IGNORE INTO seen(PATH) VALUES(?);";
/* Books whose tags were SUCCESSFULLY read this scan (tags_from_file rc==1). The BOOKS relocate refreshes an
 * existing book's metadata ONLY for these paths, so a later good read fixes stale/fallback tags while a read
 * FAILURE (filename fallback) never clobbers previously-good metadata. */
static const char *BOOKOK_DDL = "CREATE TEMP TABLE IF NOT EXISTS book_ok(PATH TEXT PRIMARY KEY);";
static const char *BOOKOK_INS = "INSERT OR IGNORE INTO book_ok(PATH) VALUES(?);";

static sqlite3 *g_db;
static sqlite3_stmt *g_ins, *g_upd, *g_seen, *g_cuechk, *g_bookok;
/* L39: a CUE/ISO backing file (e.g. one .flac holding many virtual tracks) has DB rows that share its
 * PATH but carry IS_CUE=1/IS_ISO=1 + per-track TITLE/TRACK/OFFSET. A rescan must NOT overwrite those
 * (UPDATE ... WHERE PATH=? would hit them) nor add a duplicate plain row - so we skip such a path. */
static const char *CUE_CHECK =
    "SELECT 1 FROM SONG WHERE PATH=?1 AND (COALESCE(IS_CUE,0)=1 OR COALESCE(IS_ISO,0)=1) LIMIT 1;";

/* True iff SONG has column `col`. Used to PROVE the V2.28-required columns really exist after the
 * migration ALTERs before we rebuild the library - a silently-failed ALTER (SQLITE_BUSY/FULL/IOERR)
 * must not produce a library the stock V2.28 player can't query. PRAGMA table_info is universally
 * supported (no dependency on the pragma-function feature). */
static int song_has_col(sqlite3 *db, const char *col){
    sqlite3_stmt *st = NULL; int found = 0;
    if(sqlite3_prepare_v2(db, "PRAGMA table_info(SONG);", -1, &st, NULL) == SQLITE_OK){
        while(sqlite3_step(st) == SQLITE_ROW){
            const unsigned char *n = sqlite3_column_text(st, 1);   /* col 1 = name */
            if(n && strcmp((const char*)n, col) == 0){ found = 1; break; }
        }
        sqlite3_finalize(st);
    }
    return found;
}

/* bind the 5 text tags + their 5 codes (fname/title/album/artist/genre) to a prepared stmt starting at
 * parameter index `p0` (used for both the UPDATE's SET list and the INSERT's value list). */
static void bind_tags(sqlite3_stmt *s, int p0, const char *fname, const char *title,
                      const char *album, const char *artist, const char *genre){
    sqlite3_bind_text(s, p0+0, fname, -1, SQLITE_TRANSIENT);   /* NAME = filename */
    sqlite3_bind_text(s, p0+1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+2, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+3, artist,-1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, p0+4, genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (s, p0+5, code4(fname));
    sqlite3_bind_int (s, p0+6, code4(title));
    sqlite3_bind_int (s, p0+7, code4(album));
    sqlite3_bind_int (s, p0+8, code4(artist));
    sqlite3_bind_int (s, p0+9, code4(genre));
}
/* bind DISC, TRACK, ALBUM_ARTIST, ALBUM_ARTIST_CODE at params d0..d0+3. Album-artist absent -> NULL. */
static void bind_meta(sqlite3_stmt *s, int d0, int disc, int track, const char *aa){
    sqlite3_bind_int(s, d0+0, disc);
    sqlite3_bind_int(s, d0+1, track);
    if(aa && aa[0]){ sqlite3_bind_text(s, d0+2, aa, -1, SQLITE_TRANSIENT); sqlite3_bind_int(s, d0+3, code4(aa)); }
    else          { sqlite3_bind_null(s, d0+2);                            sqlite3_bind_int(s, d0+3, 0); }
}
/* Record a path in `seen` WITHOUT touching its row - used to PRESERVE an existing row for a file we
 * couldn't index this pass (ENOENT / unreadable), so the vanished-row prune doesn't delete it. If we
 * can't even record it, block the prune (conservative: never delete a file we failed to preserve). */
static void mark_seen(const char *path){
    if(!g_seen){ g_prune_blocked = 1; return; }
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen) != SQLITE_DONE) g_prune_blocked = 1;
}
/* Merge one file into SONG: record it as seen, then UPDATE the existing PATH in place (preserving ID +
 * ACCENT), or INSERT a new row if the PATH is new. Any DB error sets g_scan_err (blocks the commit). */
static void upsert_song(const char *path, const char *fname){
    if(g_scan_err) return;   /* an earlier write failed -> don't add writes to a possibly rolled-back txn */
    char title[TAGLEN],artist[TAGLEN],album[TAGLEN],genre[TAGLEN],album_artist[TAGLEN];
    int track=0, disc=0;
    int tst = tags_from_file(path, fname, title, artist, album, genre, album_artist, &track, &disc);
    if(tst == 0){
        mark_seen(path);   /* couldn't open this pass -> keep any existing row; don't clobber good tags */
        g_skipped++;
        return;
    }
    /* record as seen (drives the post-walk delete of paths that vanished from the SD) */
    sqlite3_reset(g_seen); sqlite3_clear_bindings(g_seen);
    sqlite3_bind_text(g_seen, 1, path, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(g_seen)!=SQLITE_DONE){ g_scan_err=1; return; }
    /* Track books we read REAL tags for this scan (rc==1), so the post-walk relocate can refresh their
     * BOOKS metadata without a fallback read ever clobbering previously-good tags. */
    if(tst == 1 && has_ext(fname, ".m4b") && g_bookok){
        sqlite3_reset(g_bookok); sqlite3_clear_bindings(g_bookok);
        sqlite3_bind_text(g_bookok, 1, path, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(g_bookok)!=SQLITE_DONE){ g_scan_err=1; return; }
    }
    /* L39: if this path already backs stock CUE/ISO virtual tracks, leave them entirely alone (marked
     * seen above so they survive the prune) - do NOT overwrite their per-track metadata via UPDATE, and
     * do NOT insert a duplicate plain row. */
    sqlite3_reset(g_cuechk); sqlite3_clear_bindings(g_cuechk);
    if(sqlite3_bind_text(g_cuechk, 1, path, -1, SQLITE_TRANSIENT)!=SQLITE_OK){ g_scan_err=1; return; }
    int cchk = sqlite3_step(g_cuechk);
    if(cchk==SQLITE_ROW){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); return; }  /* CUE/ISO backing -> preserve */
    if(cchk!=SQLITE_DONE){ g_scan_err=1; return; }   /* query error -> fail closed, never risk the UPDATE on unknown state */
    if(tst < 0){
        /* tags could not be LOADED (oversized/hostile atoms, mid-read IO) but the fallback is filled:
         * NEVER overwrite an existing row's good tags. Insert a filename-fallback row only when the path
         * is new, so a new file (e.g. an m4a with an oversized embedded cover) is still indexed + playable. */
        sqlite3_stmt *chk = NULL; int exists = 0;
        if(sqlite3_prepare_v2(g_db, "SELECT 1 FROM SONG WHERE PATH=?1 LIMIT 1;", -1, &chk, NULL) != SQLITE_OK){ g_scan_err=1; return; }
        if(sqlite3_bind_text(chk, 1, path, -1, SQLITE_TRANSIENT) != SQLITE_OK){ sqlite3_finalize(chk); g_scan_err=1; return; }
        int erc = sqlite3_step(chk);
        sqlite3_finalize(chk);
        if(erc == SQLITE_ROW) exists = 1;
        else if(erc != SQLITE_DONE){ g_scan_err=1; return; }   /* query error -> fail closed; never risk a duplicate INSERT on unknown state */
        if(exists){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); return; }  /* preserve the existing row */
        /* new path -> fall through to the shared INSERT below with the filename fallback */
    } else {
        /* tst == 1: UPDATE in place (keeps ID/ACCENT/ADD_TIME). params 1..10 = tags, 11 = PATH (WHERE). */
        sqlite3_reset(g_upd); sqlite3_clear_bindings(g_upd);
        bind_tags(g_upd, 1, fname, title, album, artist, genre);
        sqlite3_bind_text(g_upd, 11, path, -1, SQLITE_TRANSIENT);
        bind_meta(g_upd, 12, disc, track, album_artist);
        if(sqlite3_step(g_upd)!=SQLITE_DONE){ g_scan_err=1; return; }
        if(sqlite3_changes(g_db) > 0){                     /* existing PATH updated in place */
            pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    /* new PATH -> INSERT (new autoincrement ID, ACCENT = schema default 0). params: 1=PATH, 2..11 tags, 12=ADD_TIME */
    sqlite3_reset(g_ins); sqlite3_clear_bindings(g_ins);
    sqlite3_bind_text(g_ins, 1, path, -1, SQLITE_TRANSIENT);
    bind_tags(g_ins, 2, fname, title, album, artist, genre);
    sqlite3_bind_int64(g_ins, 12, (sqlite3_int64)time(NULL));   /* ADD_TIME = now (new rows only; UPDATE keeps the original) */
    bind_meta(g_ins, 13, disc, track, album_artist);
    if(sqlite3_step(g_ins)==SQLITE_DONE){ pthread_mutex_lock(&g_mu); g_done++; pthread_mutex_unlock(&g_mu); }
    else g_scan_err=1;   /* insert failure (disk full, DB corruption) must block the commit */
}

/* recursive walk; inserts every audio file found under dir. lstat (not stat) so symlinks
 * are never followed, + a depth cap, so a hostile/looping tree can't run away. */
static void walk(const char *dir, int depth){
    if(depth > 24){ g_scan_err=1; return; }   /* absurdly deep -> treat as incomplete, don't commit */
    DIR *d=opendir(dir);
    if(!d){ g_scan_err=1; return; }           /* the SD is exFAT/vfat (no perms): a failed opendir means
                                               * I/O error or the card was pulled -> incomplete scan */
    struct dirent *e;
    char path[MAXPATH];
    for(errno=0; (e=readdir(d)); errno=0){    /* errno reset before each readdir so we can detect a read error */
        if(g_scan_err) break;                 /* an earlier write failed (may have auto-rolled-back the txn): stop issuing writes */
        if(atomic_load(&g_abort)){ g_scan_err = 1; break; }  /* SD access revoked mid-walk: stop reading the card at once
                                                * (g_scan_err also blocks the commit, keeping the library) */
        if(e->d_name[0]=='.'){
            if(e->d_name[1]==0 || (e->d_name[1]=='.'&&e->d_name[2]==0)) continue;   /* . / .. */
            /* Our OWN app-owned art-cache dir (SCAN_ROOT/.diskos) holds no indexed audio, so it must NOT
             * block the prune. Without this skip EVERY scan sets g_prune_blocked and neither vanished songs
             * NOR vanished/renamed books are ever pruned. Restrict to depth 0 (the real cache dir at the SD
             * root): a user's ".diskos" deeper in the tree may hold indexed songs and must still block. */
            if(depth == 0 && strcmp(e->d_name, ".diskos") == 0) continue;
            /* other dotfile/dotdir: don't index it, but a skipped dot-DIRECTORY may hold songs already
             * in the DB that this walk won't re-see, so block the vanished-row prune to avoid deleting them. */
            int wl = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
            struct stat ds;
            /* block the prune if this skipped dot-entry is a directory (may hold indexed songs) OR we
             * could not stat it (unknown type -> stay safe, never risk deleting its subtree's rows). */
            if(wl<=0 || wl>=(int)sizeof path || lstat(path,&ds)!=0 || S_ISDIR(ds.st_mode)) g_prune_blocked = 1;
            continue;
        }
        int w = snprintf(path,sizeof path,"%s/%s",dir,e->d_name);
        if(w<0 || w>=(int)sizeof path){ g_skipped++; g_prune_blocked=1; continue; }   /* overlong path unknowable -> can't preserve its row -> block the prune */
        struct stat st;
        /* Failing the whole scan on ANY per-file lstat error was the killer bug: every real exFAT SD
         * has entries that readdir lists but lstat can't resolve (ENOENT) - orphaned entries and, on
         * this device, real files whose special-character names don't round-trip through the mount's
         * encoding. Those always set g_scan_err=1, so the commit gate rolled back a fully-successful
         * index -> empty library (why the DB had to be hand-built). Skip ONLY the benign ENOENT case
         * (the file can't be opened to index anyway); any OTHER errno (EIO/EACCES = the card going bad)
         * stays fatal so we never commit a silently-incomplete library over a good one. */
        if(lstat(path,&st)!=0){
            if(errno == ENOENT){
                if(is_audio(e->d_name)) mark_seen(path);   /* audio file: preserve its existing row (special-char names) */
                else g_prune_blocked = 1;                  /* could be an unresolvable DIRECTORY whose songs we never
                                                            * walked -> block the prune so its subtree rows aren't deleted */
                g_skipped++; continue;                     /* unresolvable entry -> skip, best-effort */
            }
            g_scan_err = 1; continue;                        /* real I/O/access error -> don't commit a partial index */
        }
        if(S_ISDIR(st.st_mode)) walk(path, depth+1);
        else if(S_ISREG(st.st_mode) && is_audio(e->d_name)) upsert_song(path, e->d_name);
        else if(S_ISREG(st.st_mode) && is_unsupported_audio(e->d_name)){  /* AAC/M4A/OGG/... not indexed yet */
            pthread_mutex_lock(&g_mu); g_unsupported++; pthread_mutex_unlock(&g_mu);
        }
    }
    if(errno) g_scan_err=1;                   /* readdir error -> this directory listing was incomplete */
    closedir(d);
}

/* True only when SCAN_ROOT is a REAL, readable mountpoint (its st_dev differs from its
 * parent's). A missing/late SD, or a bare placeholder /tmp/sdcard dir, returns 0 - so we
 * never wipe the live library rebuilding from an unmounted card. */
static int scan_root_ready(void){
    struct stat sroot, sparent;
    if(stat(SCAN_ROOT, &sroot)!=0 || !S_ISDIR(sroot.st_mode)) return 0;
    char parent[MAXPATH];
    if(snprintf(parent,sizeof parent,"%s/..",SCAN_ROOT) >= (int)sizeof parent) return 0;
    if(stat(parent, &sparent)!=0) return 0;
    if(sroot.st_dev == sparent.st_dev) return 0;   /* not a separate mount -> SD not mounted */
    DIR *d=opendir(SCAN_ROOT); if(!d) return 0; closedir(d);
    return 1;
}

static void *scan_thread(void *arg){
    (void)arg;
    int ok=0, found=0;
    /* Guard: if the SD isn't actually mounted at SCAN_ROOT, do NOT open a transaction or
     * DELETE anything - keep the existing library intact. This is the #1 data-loss guard:
     * an absent/late mount must never commit an empty SONG table. */
    if(!scan_root_ready()){
        pthread_mutex_lock(&g_mu);
        g_no_sd=1; g_total=0;      /* 0 => library kept; g_no_sd lets the UI say "insert SD" */
        g_active=0; g_finished_seq++;
        pthread_mutex_unlock(&g_mu);
        sd_io_end();
        return NULL;
    }
    if(sqlite3_open_v2(DB_PATH,&g_db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK){
        sqlite3_busy_timeout(g_db,8000);
        sqlite3_exec(g_db, SCHEMA_SONG, 0,0,0);
        /* Bring an EXISTING (older diskOS or stock) SONG table up to the V2.28 superset BEFORE the
         * scan transaction, so the stock player's playback SQL (which SELECTs IS_M3U/M3U_PATH) always
         * finds the columns instead of depending on the player winning a startup migration race.
         * ALTERs are idempotent here - a duplicate column just errors harmlessly. */
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN IS_M3U INT DEFAULT 0;", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN M3U_PATH TEXT DEFAULT '';", 0,0,0);
        sqlite3_exec(g_db, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0,0,0);
        /* PROVE the required columns exist (a duplicate-column ALTER error is fine, but a real
         * failure - BUSY/FULL/IOERR/corruption - is not). If any is missing, flag a scan error so
         * the commit gate below rolls back and KEEPS the existing library rather than rebuilding one
         * the stock V2.28 player can't query (its playback SQL SELECTs IS_M3U/M3U_PATH). */
        if(!song_has_col(g_db,"IS_M3U") || !song_has_col(g_db,"M3U_PATH") || !song_has_col(g_db,"ACCENT")){
            g_scan_err = 1;
            fprintf(stderr, "scanner: SONG schema migration incomplete -> keeping existing library\n");
        }
        g_ins=NULL; g_upd=NULL; g_seen=NULL; g_cuechk=NULL; g_bookok=NULL;
        if(sqlite3_prepare_v2(g_db, INS_SONG, -1, &g_ins,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, UPD_SONG, -1, &g_upd,  NULL)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, CUE_CHECK, -1, &g_cuechk, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, SEEN_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, SEEN_INS, -1, &g_seen, NULL)==SQLITE_OK
           && sqlite3_exec(g_db, BOOKOK_DDL, 0,0,0)==SQLITE_OK
           && sqlite3_prepare_v2(g_db, BOOKOK_INS, -1, &g_bookok, NULL)==SQLITE_OK){
            /* ONE atomic transaction: the whole MERGE (UPDATE existing / INSERT new / DELETE vanished)
             * commits together, or ROLLBACK on any failure - a failed/partial scan can never corrupt or
             * wipe the library. MERGE (not delete+reinsert) so existing rows KEEP their ID + ACCENT,
             * preserving resume (MEMORY_PLAY.MUSIC_ID) + favourites (MY_LOVE.ID) + art-accent across a
             * rescan. Only mp3/flac/wav rows are ever removed; m4a/ape/dsf/... are never touched. */
            if(sqlite3_exec(g_db,"BEGIN IMMEDIATE;",0,0,0)==SQLITE_OK){
                g_skipped=0; g_prune_blocked=0;
                /* Audiobooks live in their OWN table, never SONG (the player queues all of SONG for
                 * music, so a book in SONG would leak into Play-All/shuffle). */
                sqlite3_exec(g_db,"CREATE TABLE IF NOT EXISTS BOOKS (ID INTEGER PRIMARY KEY autoincrement,"
                    "PATH TEXT UNIQUE, NAME TEXT, TITLE TEXT, ARTIST TEXT, ALBUM TEXT, GENRE TEXT,"
                    "DURATION BIGINT, ADD_TIME INT8);",0,0,0);
                sqlite3_exec(g_db,"DELETE FROM seen;",0,0,0);   /* start from an empty seen-set */
                sqlite3_exec(g_db,"DELETE FROM book_ok;",0,0,0);   /* and an empty real-tags-book set */
                walk(SCAN_ROOT, 0);
                pthread_mutex_lock(&g_mu); found = g_done; pthread_mutex_unlock(&g_mu);
                /* observability: one concise line per scan */
                fprintf(stderr,"scanner: merged %d songs, skipped %d unreadable file(s), scan_err=%d\n",
                        found, g_skipped, g_scan_err);
                /* Commit only if the walk found audio AND the SD is STILL mounted afterwards (guards a
                 * card pulled / gone-I/O mid-scan; zero-found rolls back too). Then delete scanned-format
                 * rows whose PATH vanished from the SD - inside the txn, so it rolls back on any failure. */
                if(found > 0 && !g_scan_err && scan_root_ready()){
                    /* Prune vanished songs - but ONLY when pruning wasn't blocked (!g_prune_blocked: no
                     * overlong/unknown-type skip, no mark_seen failure) and scoped to THIS mount (PATH
                     * under SCAN_ROOT). ENOENT/unreadable audio files ARE preserved in `seen`, so they
                     * don't block; only unknowable skips do. Rows from another mount/source that this
                     * scanner never inspected must not be touched. If blocked, we still commit the merge
                     * (UPDATEs/INSERTs) but do NOT delete. */
                    static const char *DEL_ABSENT =
                        "DELETE FROM SONG WHERE (lower(PATH) LIKE '%.mp3' OR lower(PATH) LIKE '%.flac'"
                        " OR lower(PATH) LIKE '%.wav' OR lower(PATH) LIKE '%.m4a' OR lower(PATH) LIKE '%.m4b')"
                        " AND PATH LIKE '" SCAN_ROOT "/%' "
                        "AND PATH NOT IN (SELECT PATH FROM seen);";
                    int del_ok = (g_prune_blocked) ? 1
                                                   : (sqlite3_exec(g_db,DEL_ABSENT,0,0,0)==SQLITE_OK);
                    /* Relocate the .m4b rows the walk indexed into BOOKS, then remove them from SONG, all
                     * inside this txn so the player never sees a book in the music queue. Only SEEN (present)
                     * books move (never lose a book under a prune-blocked skipped dir); vanished books are
                     * pruned from BOOKS only when pruning is unblocked. */
                    if(del_ok && DISKOS_AUDIOBOOKS){
                        int r = sqlite3_exec(g_db,
                            /* Insert new books; for an EXISTING book, refresh NAME/TITLE/ARTIST/ALBUM/GENRE
                             * ONLY when this scan actually read its tags (PATH in book_ok) - so a good re-read
                             * fixes stale/fallback metadata, while a read FAILURE (filename fallback) never
                             * clobbers previously-good tags. DURATION + ADD_TIME are left out of the SET, so
                             * they are preserved on conflict (a re-index into SONG carries DURATION=0). */
                            "INSERT INTO BOOKS(PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME) "
                            "SELECT PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME FROM SONG "
                            "WHERE lower(PATH) LIKE '%.m4b' AND PATH IN (SELECT PATH FROM seen) "
                            "ON CONFLICT(PATH) DO UPDATE SET "
                            "NAME=excluded.NAME,TITLE=excluded.TITLE,ARTIST=excluded.ARTIST,"
                            "ALBUM=excluded.ALBUM,GENRE=excluded.GENRE "
                            "WHERE BOOKS.PATH IN (SELECT PATH FROM book_ok);",0,0,0)==SQLITE_OK
                          && sqlite3_exec(g_db,
                            "DELETE FROM SONG WHERE lower(PATH) LIKE '%.m4b' AND PATH IN (SELECT PATH FROM seen);",0,0,0)==SQLITE_OK;
                        if(r && !g_prune_blocked)
                            r = sqlite3_exec(g_db,
                                "DELETE FROM BOOKS WHERE PATH LIKE '" SCAN_ROOT "/%' AND PATH NOT IN (SELECT PATH FROM seen);",0,0,0)==SQLITE_OK;
                        if(!r) del_ok = 0;   /* relocate failed -> roll back the whole scan */
                    }
                    /* A cancellation that lands AFTER the walk (or during the pruning SQL) must still keep the
                     * library as it was: check again immediately before COMMIT, and roll back instead. */
                    if(del_ok && !atomic_load(&g_abort)) ok = (sqlite3_exec(g_db,"COMMIT;",0,0,0)==SQLITE_OK);
                    else ok = 0;
                }
                if(!ok) sqlite3_exec(g_db,"ROLLBACK;",0,0,0);
            }
        }
        sqlite3_finalize(g_ins);  g_ins=NULL;
        sqlite3_finalize(g_upd);  g_upd=NULL;
        sqlite3_finalize(g_cuechk); g_cuechk=NULL;
        sqlite3_finalize(g_seen); g_seen=NULL;
        sqlite3_finalize(g_bookok); g_bookok=NULL;
    }
    sqlite3_close(g_db); g_db=NULL;   /* close even on a failed open: sqlite3_open_v2 may still return a handle */
    pthread_mutex_lock(&g_mu);
    g_skipped_result = g_skipped;
    g_total = ok ? g_done : 0;    /* committed count; 0 signals a failed rebuild (library kept) */
    g_active=0; g_finished_seq++;
    pthread_mutex_unlock(&g_mu);
    sd_io_end();
        return NULL;
}

/* Stop a running walk. The scan holds a single lease for its whole run, so closing admission alone
 * cannot stop it - without this, revoking SD access left the scanner traversing the card. */
void scanner_abort(void){ atomic_store(&g_abort, 1); }
int scanner_start(void){
    if(!sd_io_begin()) return -1;
    pthread_mutex_lock(&g_mu);
    /* Refuse an overlapping start BEFORE touching the cancellation flag: clearing it first erased a
     * cancellation aimed at the scan that is already running. */
    if(g_active){ pthread_mutex_unlock(&g_mu); sd_io_end(); return -1; }   /* already scanning */
    atomic_store(&g_abort, 0);
    g_active=1; g_done=0; g_total=0; g_no_sd=0; g_skipped=0; g_skipped_result=0; g_scan_err=0; g_unsupported=0;
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if(pthread_create(&th,NULL,scan_thread,NULL)!=0){
        pthread_mutex_lock(&g_mu); g_active=0; pthread_mutex_unlock(&g_mu);
        sd_io_end(); return -1;
    }
    pthread_detach(th);
    return 0;
}

/* ===================== [isolated test harness] =========================== */
#ifdef SCANNER_TEST
#include <unistd.h>
int main(void){
    fprintf(stderr,"scan test -> %s (root %s)\n", DB_PATH, SCAN_ROOT);
    if(scanner_start()!=0){ fprintf(stderr,"start failed\n"); return 1; }
    int done,total;
    while(scanner_active()){ scanner_progress(&done,NULL); fprintf(stderr,"  %d...\r",done); usleep(200000); }
    scanner_progress(&done,&total);
    fprintf(stderr,"\nDONE: %d songs\n", total);
    return 0;
}
#endif

int scan_read_chapters(const char *path, chapter_t *out, int max){
    if(!sd_io_begin()) return 0;
    int result = scan_read_chapters_leased(path, out, max);
    sd_io_end();
    return result;
}

int scan_read_narrator(const char *path, char *out, int cap){
    if(!sd_io_begin()) return 0;
    int result = scan_read_narrator_leased(path, out, cap);
    sd_io_end();
    return result;
}
