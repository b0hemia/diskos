/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef IPC_H
#define IPC_H
typedef struct {
    char title[160];
    char artist[160];
    char album[160];
    char playing_num[24];
    char work_mode[24];
    char path[256];
    long duration_ms;
    long position_ms;
    int  state;       /* RAW A2 metadata when returned by ipc_get_state() - UNRELIABLE (reports 0 while
                       * playing); never test it for play-state. The main loop overwrites ITS LOCAL copy
                       * with the normalized 2=playing / 1=paused inference (and publishes g_playing). */
    int  have_track;
    int  sample_rate; /* Hz, from song_sample_rate */
    int  is_dsd;      /* DSD stream */
    int  is_favorite; /* outer "love" flag */
    int  volume;      /* 0..VOL_MAX, from a714 frames */
    unsigned volume_seq; /* bumps on each volume update (drives the on-screen bar) */
    unsigned seq;
    unsigned path_seq;   /* value of seq when path last CHANGED (a track-identity epoch) */
    unsigned pos_seq;    /* value of seq when position was last reported (an a1 frame). pos_seq > path_seq
                          * means the current position was reported AFTER the current track loaded, i.e. it
                          * belongs to this track and not a stale one - used to gate audiobook checkpoints. */
} track_state_t;

#define VOL_MAX 120   /* device MAX_VOL (sysconfig); a714 VV is on this same 0..120 scale */

int  ipc_start(void);                  /* open queues + start thread; 0=ok */
void ipc_get_state(track_state_t *out);
void ipc_seed_state(const track_state_t *s);  /* startup resume-state seed */
int  ipc_send_cmd(const char *frame);  /* e.g. "0201000C0000" play/pause; 0=queued, -1=failed */
int  ipc_send_probe(const char *frame);/* background send (state-sync/health): silent, no toast */
int  ipc_is_ready(void);               /* 1 once the /ui receive queue is open */
int  ipc_take_send_error(void);        /* 1 (and clears) if a recent send failed */
void ipc_health_check(void);           /* ~30s: detect player-restart queue recreation (fstat identity) */
int  ipc_take_reconnected(void);       /* 1 (and clears) after a recovery reattach -> caller resyncs */
unsigned ipc_rx_frames(void);          /* count of /ui frames received from the player (>0 => player up) */
int  ipc_player_mode(void);            /* last a607 player mode this gen: -1=none, 8=LOCALPLAYER (v2.40 oracle) */
unsigned ipc_generation(void);         /* bumps on each /ui reattach (player restart) - v2.40 one-shot re-arm */
#endif
