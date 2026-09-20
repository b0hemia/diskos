/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "art.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

/* int64 so tv_sec*1000 can't overflow a 32-bit long after ~24.9 days of monotonic uptime (which would
 * corrupt every deadline comparison below and kill fresh decodes on sight). */
static int64_t art_now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec*1000LL + t.tv_nsec/1000000LL; }

/* ffmpeg reads the embedded cover art straight from the track file and, in a
 * single split filtergraph, writes the 148px cover, 42px Home thumb, and the
 * 360px blurred backdrop. No manual JPEG extraction, no big RAM buffer, and it
 * finds art anywhere in the file (not just the first ~1.3MB). tjpgd garbles
 * large covers, so we never use it. */

/* Escape a path for single-quoted shell inclusion: ' -> '\'' */
static void shesc(const char *in, char *out, size_t cap){
    size_t o = 0;
    for(size_t i=0; in[i] && o+4 < cap; i++){
        if(in[i]=='\''){ out[o++]='\''; out[o++]='\\'; out[o++]='\''; out[o++]='\''; }
        else out[o++] = in[i];
    }
    out[o] = 0;
}

/* The LIVE (user-driven) decode registers its child here so art_cancel() can kill
 * it when the user skips ahead. Prewarm / fallback decodes pass cancellable=0 and
 * are never registered, so cancelling a skip only targets the on-screen track. */
static pthread_mutex_t g_pid_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile pid_t  g_live_pid = 0;
/* Bumped by art_cancel(). A cancellable decode snapshots it on entry and re-checks it (under
 * g_pid_mu) before starting EACH ffmpeg child, so a skip is honored not just by killing the
 * running child but by refusing to launch a follow-up cover-fallback decode for the old track. */
static volatile unsigned g_cancel_gen = 0;

/* Kill the in-flight LIVE decode (process group), if any, and supersede any between-attempt work.
 * Safe to call anytime. */
void art_cancel(void){
    pthread_mutex_lock(&g_pid_mu);
    g_cancel_gen++;
    pid_t p = g_live_pid;
    if(p > 0) kill(-p, SIGKILL);
    pthread_mutex_unlock(&g_pid_mu);
}

/* Run the ffmpeg pipeline on ONE input as a killable child (sh -c, own process group).
 * cancellable=1 registers the child for art_cancel(). Returns 0 on success, -1 on error/no-art,
 * -2 if a newer request superseded this one (gen changed) before the child was launched. */
static int art_run_pipeline(const char *input, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0,
                    int64_t deadline_ms){
    char esc[600]; shesc(input, esc, sizeof esc);
    char cmd[1500];
    /* Backdrop = the actual cover art, scaled to fill the screen and gaussian-blurred
     * (iOS/Apple-Music "frosted cover" look). sigma chosen to soften detail while the
     * cover is still recognisable as itself. */
    snprintf(cmd, sizeof cmd,
        "ffmpeg -y -loglevel quiet -i '%s' -an -filter_complex "
        "'[0:v]split=3[a][b][c];[a]scale=148:148:flags=area[cv];[b]scale=42:42:flags=area[th];"
        "[c]scale=360:360:flags=bilinear,gblur=sigma=19[bg]' "
        "-map '[cv]' -frames:v 1 -pix_fmt bgr24 -f image2 '%s' "
        "-map '[th]' -frames:v 1 -pix_fmt bgr24 -f image2 '%s' "
        "-map '[bg]' -frames:v 1 -pix_fmt bgr24 -f image2 '%s'",
        esc, cover_bmp, thumb_bmp, backdrop_bmp);

    /* Hold g_pid_mu across fork + setpgid + register so art_cancel() can't run in
     * the window between fork and registration (it would otherwise miss this child).
     * Both parent and child call setpgid (idempotent race) so the process group is
     * guaranteed to exist before we unlock -> kill(-pid) can't ESRCH on a missing pgid. */
    if(cancellable) pthread_mutex_lock(&g_pid_mu);
    if(cancellable && g_cancel_gen != gen0){ pthread_mutex_unlock(&g_pid_mu); return -2; }  /* superseded - don't start */
    pid_t pid = fork();
    if(pid < 0){ if(cancellable) pthread_mutex_unlock(&g_pid_mu); return -1; }
    if(pid == 0){
        setpgid(0, 0);                       /* own group so a single kill takes the whole pipeline */
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);                          /* exec failed */
    }
    if(cancellable){
        setpgid(pid, pid);                   /* parent side of the race; ignore EACCES/ESRCH */
        g_live_pid = pid;
        pthread_mutex_unlock(&g_pid_mu);
    }
    /* BOUNDED wait against a SHARED absolute monotonic deadline (art_now_ms), so the whole decode request
     * (embedded + every sibling-cover attempt) can't exceed ~15s of held g_decode_mu regardless of how many
     * attempts run, scheduling jitter, or EINTR storms. We check REAL elapsed time each iteration (not the
     * count of requested sleeps), and never round a near-expired deadline back up. On timeout, SIGKILL the
     * group and reap it WITHOUT blocking (a D-state child reparents to init on our exit). */
    int status = 0, killed = 0; pid_t w = 0;
    for(;;){
        if(art_now_ms() >= deadline_ms){                           /* over the shared deadline -> kill */
            kill(-pid, SIGKILL); killed = 1;
            int64_t rdl = art_now_ms() + 1000;                     /* bounded reap ~1s (real time) */
            while(art_now_ms() < rdl){ if(waitpid(pid,&status,WNOHANG)==pid) break; usleep(10*1000); }
            break;
        }
        w = waitpid(pid, &status, WNOHANG);
        if(w == pid) break;                                        /* child exited */
        if(w < 0 && errno == EINTR) continue;
        if(w < 0){ killed = 1; break; }                            /* waitpid error */
        usleep(20*1000);
    }
    if(cancellable){
        pthread_mutex_lock(&g_pid_mu); g_live_pid = 0; pthread_mutex_unlock(&g_pid_mu);
    }
    if(killed) return -1;                                          /* waitpid failed / killed on timeout */
    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;  /* killed or ffmpeg error */

    /* all three outputs must exist + be non-trivial - catches a broken filtergraph
     * (e.g. a missing filter) immediately instead of shipping a half-written backdrop. */
    const char *outs[3] = { cover_bmp, thumb_bmp, backdrop_bmp };
    for(int i=0;i<3;i++){
        FILE *b = fopen(outs[i], "rb"); if(!b) return -1;
        fseek(b, 0, SEEK_END); long sz = ftell(b); fclose(b);
        if(sz <= 100) return -1;
    }
    return 0;
}

/* Sibling cover filenames tried when a track has no embedded art. JPEG/BMP only: the V2.40 ffmpeg
 * codec set (and diskOS's decoders) can't do PNG. exfat is case-insensitive so a couple of casings
 * cover the common ones. */
static const char *ART_COVER_NAMES[] = {
    "cover.jpg","folder.jpg","cover.jpeg","folder.jpeg","cover.bmp","folder.bmp",
    "Cover.jpg","Folder.jpg","AlbumArt.jpg","albumart.jpg",
};

/* Render cover/thumb/backdrop for a track. Prefer the track's EMBEDDED art (stock precedence); if
 * there is none, fall back to a sibling cover file in the track's folder so albums that keep their
 * art as cover.jpg/folder.jpg still show artwork instead of the no-art placeholder. A cancellable
 * decode snapshots the cancel generation and stops (returns -1, no wasted work) as soon as a skip
 * supersedes it, whether during the embedded run or between it and the cover fallback. */
/* Read the current cancel generation (for a caller that wants to capture it ATOMICALLY with its own
 * request identity, then pass it to art_make_all_ex_gen). art_cancel() is the only bumper and it's called
 * by the live-art setter while holding that setter's request lock, so a caller holding the same lock across
 * this read gets a token consistent with its request snapshot. */
unsigned art_cancel_gen(void){
    unsigned g; pthread_mutex_lock(&g_pid_mu); g = g_cancel_gen; pthread_mutex_unlock(&g_pid_mu); return g;
}

/* gen0 = the cancel token the CALLER captured with its request. A superseding skip bumps g_cancel_gen (via
 * art_cancel), so the per-child gen check trips and no obsolete decode runs - even if the skip lands after
 * the caller validated its request but before we get here. */
int art_make_all_ex_gen(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0){
    /* ONE 15s deadline across the whole request (embedded + every sibling-cover attempt), so a slow/hung
     * album can't hold g_decode_mu for attempts x 15s. Absolute monotonic so every attempt shares it. */
    int64_t deadline = art_now_ms() + 15000;
    int r = art_run_pipeline(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0, deadline);
    if(r == 0)  return 0;    /* embedded art */
    if(r == -2) return -1;   /* superseded - stop */

    const char *slash = strrchr(track, '/');       /* else try a sibling cover file */
    if(!slash) return -1;
    size_t dlen = (size_t)(slash - track);
    if(dlen == 0 || dlen > 900) return -1;
    for(unsigned i=0; i<sizeof ART_COVER_NAMES/sizeof ART_COVER_NAMES[0]; i++){
        if(art_now_ms() >= deadline) break;        /* request deadline spent -> stop trying */
        char cov[1024];
        int n = snprintf(cov, sizeof cov, "%.*s/%s", (int)dlen, track, ART_COVER_NAMES[i]);
        if(n <= 0 || n >= (int)sizeof cov) continue;
        if(access(cov, R_OK) != 0) continue;       /* not present */
        r = art_run_pipeline(cov, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0, deadline);
        if(r == 0)  return 0;                       /* rendered from the external cover */
        if(r == -2) return -1;                      /* superseded between attempts - stop */
    }
    return -1;
}

/* Snapshotting wrapper: captures the cancel generation internally (fine for callers that don't need to
 * couple it to an external request identity - non-cancellable ones ignore gen0 entirely). */
int art_make_all_ex(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable){
    unsigned gen0;
    pthread_mutex_lock(&g_pid_mu); gen0 = g_cancel_gen; pthread_mutex_unlock(&g_pid_mu);
    return art_make_all_ex_gen(track, cover_bmp, thumb_bmp, backdrop_bmp, cancellable, gen0);
}

/* Non-cancellable convenience wrapper (prewarm + synchronous fallback). */
int art_make_all(const char *track, const char *cover_bmp,
                 const char *thumb_bmp, const char *backdrop_bmp){
    return art_make_all_ex(track, cover_bmp, thumb_bmp, backdrop_bmp, 0);   /* NOLINT */
}
