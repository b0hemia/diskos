/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "sdio.h"
#include "musicdb.h"
#include "sqlite3.h"
#define JSMN_HEADER      /* declarations only; the implementation lives in ipc.c */
#include "jsmn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <strings.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define DB_PATH "/usr/data/fiio/db/song.db"

/* The whole library is held in a dynamically-sized array (no fixed cap): sized to
 * the actual SONG count at load. Loaded ONCE at startup, so the realloc never moves
 * under live pointers. */
static mdb_song_t *g_songs = NULL;
static int g_n = 0;
static int g_cap = 0;
static int g_load_err = 0;   /* mdb_load hit a DB error (BUSY/IOERR/OOM) vs a genuinely empty library */

/* ---- cached group lists (Artists/Albums/Genres) --------------------------
 * These are derived from g_songs by parse+sort+dedup - expensive to recompute
 * on every list open.  Build once, cache, and return a fast memcpy thereafter
 * (so opening Artists/Albums is as instant as Songs).  mdb_load() invalidates. */
static char (*g_cart)[MDB_STR];                      static int  g_cart_n = -1;   /* artists */
static char (*g_calb)[MDB_STR], (*g_calb_ar)[MDB_STR]; static int *g_calb_ct; static int g_calb_n = -1; /* albums */
static char (*g_cgen)[MDB_STR];                      static int *g_cgen_ct;  static int g_cgen_n = -1;   /* genres */
static void groups_free(void){
    free(g_cart);    g_cart=NULL;    g_cart_n=-1;
    free(g_calb);    free(g_calb_ar); free(g_calb_ct);
    g_calb=NULL; g_calb_ar=NULL; g_calb_ct=NULL; g_calb_n=-1;
    free(g_cgen);    free(g_cgen_ct); g_cgen=NULL; g_cgen_ct=NULL; g_cgen_n=-1;
}

static int mdb_ensure_cap(int need){
    if(need <= g_cap) return 1;
    /* The library loads ONCE with the known SONG count, so size to that exact count plus a small pad
     * (was: round up to the next power of two, which wasted ~515KB at 3282 songs). A later rescan with a
     * larger count just reallocs again. */
    if((size_t)need + 16 > ((size_t)-1) / sizeof *g_songs) return 0;   /* overflow guard */
    int nc = need + 16;
    mdb_song_t *p = realloc(g_songs, (size_t)nc * sizeof *g_songs);
    if(!p) return 0;
    g_songs = p; g_cap = nc; return 1;
}

static void trim(char *s){
    char *p = s; while(*p==' '||*p=='\t') p++;
    if(p!=s) memmove(s, p, strlen(p)+1);
    int n = (int)strlen(s);
    while(n>0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n]=0;
}

/* One cached read/write connection, opened lazily. Holds no lock while idle, so
 * it coexists with mq_player (rollback-journal DB) via the busy timeout. All
 * mdb_* calls run on the UI thread. Prepared statements + bound params replace
 * the old popen("sqlite3 ...") + hand-built SQL (no escaping/injection/parse
 * bugs, no fork/exec per query). */
static sqlite3 *g_db;
/* g_db is shared across the UI thread, the art worker, and the prewarm thread.
 * The lazy open MUST be serialized (a plain `if(!g_db) open` is a data race: two
 * threads can both open, publish competing handles, and corrupt state -> SIGSEGV).
 * The mutex also gives a memory barrier so a published g_db is fully initialised
 * before another thread sees it. FULLMUTEX makes concurrent USE of the open handle
 * safe (sqlite serializes every API call on it). */
static pthread_mutex_t g_db_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db(void){
    pthread_mutex_lock(&g_db_mu);
    if(!g_db){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 4000);
            /* persistent per-song accent cache (computed once from album art, survives reboot).
             * ALTER is idempotent here: harmless error if the column already exists. */
            sqlite3_exec(tmp, "ALTER TABLE SONG ADD COLUMN ACCENT INTEGER DEFAULT 0;", 0, 0, 0);
            /* diskOS-owned play history (separate table -> no SONG schema change, survives rescans,
             * ignored by the stock player). Drives Most-Played / Recently-Played. */
            sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS PLAY_STATS(PATH TEXT PRIMARY KEY, "
                              "PLAYS INTEGER DEFAULT 0, LAST_PLAYED INTEGER DEFAULT 0);", 0, 0, 0);
            /* diskOS-owned audiobook progress (separate table, path-keyed like PLAY_STATS, survives
             * rescans + the stock DB rebuild). One bookmark per book; MEMBER_PATH is the file that was
             * playing (a multipart book has several), POSITION_MS the spot within it. */
            sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS BOOK_PROGRESS(BOOK_KEY TEXT PRIMARY KEY, "
                              "MEMBER_PATH TEXT NOT NULL, POSITION_MS INTEGER NOT NULL DEFAULT 0, "
                              "UPDATED_AT INTEGER NOT NULL DEFAULT 0, COMPLETED INTEGER NOT NULL DEFAULT 0);", 0, 0, 0);
            /* diskOS-owned monotone counter + per-playlist export identity. A playlist's export filename
             * is keyed by a UID from this counter, NOT its pid or creation time: the create-allocator can
             * reuse a pid after a delete (it UPDATEs a new row's id upward without advancing
             * sqlite_sequence), and two creations can share a whole-second ADD_TIME, so neither is a
             * reuse-proof identity. The counter only ever increases and is persisted, so a UID is never
             * reused across playlist lifetimes -> two different playlists never target the same .m3u. */
            sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS DISKOS_META(K TEXT PRIMARY KEY, V INTEGER NOT NULL);", 0, 0, 0);
            sqlite3_exec(tmp, "CREATE TABLE IF NOT EXISTS DISKOS_PL_EXPORT(PID INTEGER PRIMARY KEY, UID INTEGER NOT NULL);", 0, 0, 0);
            g_db = tmp;   /* publish only after full init */
        } else if(tmp){ sqlite3_close(tmp); }
    }
    sqlite3 *ret = g_db;
    pthread_mutex_unlock(&g_db_mu);
    return ret;
}
/* Dedicated connection for the worker-thread accent WRITE (mdb_set_song_accent) ONLY.
 * Keeping that UPDATE off g_db means a worker's row-change can never be misread by a
 * UI-thread sqlite3_changes() (which follows the UI thread's own step()), and a worker
 * write can never join/rollback a g_db transaction. INVARIANT: nothing else may use
 * g_db_w, and no code here may call sqlite3_changes() on it (would reintroduce a race
 * between the two worker threads). Shorter busy_timeout than g_db: a stuck UI txn must
 * not wedge art decode for seconds - the accent write is best-effort, retried next decode. */
static sqlite3 *g_db_w;
static pthread_mutex_t g_db_w_mu = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db_w(void){
    db();   /* ensure the main connection (and its ACCENT-column ALTER) is initialised first,
             * so g_db_w never races an ALTER and the SONG.ACCENT column always exists. */
    pthread_mutex_lock(&g_db_w_mu);
    if(!g_db_w){
        sqlite3 *tmp = NULL;
        if(sqlite3_open_v2(DB_PATH, &tmp, SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX, NULL) == SQLITE_OK){
            sqlite3_busy_timeout(tmp, 500);
            g_db_w = tmp;
        } else if(tmp){ sqlite3_close(tmp); }
    }
    sqlite3 *ret = g_db_w;
    pthread_mutex_unlock(&g_db_w_mu);
    return ret;
}
/* per-song accent (0xRRGGBB packed, 0 = not computed yet). Persisted in SONG.ACCENT. */
int mdb_song_accent(const char *path){
    sqlite3 *d = db(); if(!d || !path) return 0;
    sqlite3_stmt *st; int rgb = 0;
    if(sqlite3_prepare_v2(d, "SELECT ACCENT FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) rgb = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return rgb;
}
void mdb_set_song_accent(const char *path, int rgb){
    sqlite3 *d = db_w(); if(!d || !path) return;   /* worker-only write on its private connection */
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE SONG SET ACCENT=? WHERE PATH=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, rgb); sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
        sqlite3_step(st); sqlite3_finalize(st);
    }
}
/* prewarm iterator: next SONG with ID > after_id, ordered by ID (covers ALL rows,
 * not just the in-memory cap). Fills *id and path. Returns 1 if a row was found.
 * Own prepared statement each call -> safe to call from the prewarm thread under
 * SQLITE_THREADSAFE=1 (serialized) alongside the UI thread's DB use. */
int mdb_prewarm_next(int after_id, int *id, char *path, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int found = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID, PATH FROM SONG WHERE ID > ? ORDER BY ID LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int(st, 1, after_id);
        if(sqlite3_step(st) == SQLITE_ROW){
            if(id) *id = sqlite3_column_int(st, 0);
            const char *p = (const char*)sqlite3_column_text(st, 1);
            if(path && cap>0) snprintf(path, cap, "%s", p ? p : "");
            found = 1;
        }
        sqlite3_finalize(st);
    }
    return found;
}
/* text column, never NULL (so snprintf "%s" is safe) */
static const char *colt(sqlite3_stmt *st, int i){
    const char *t = (const char*)sqlite3_column_text(st, i);
    return t ? t : "";
}

int mdb_load(void){
    g_n = 0; g_load_err = 0;
    groups_free();                 /* library reloaded -> drop cached Artists/Albums/Genres */
    sqlite3 *d = db(); if(!d){ g_load_err = 1; return 0; }
    /* size the array to the real count first (no 1500 cap) */
    sqlite3_stmt *cst;
    int count = 0, count_ok = 0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM SONG;", -1, &cst, NULL) == SQLITE_OK){
        if(sqlite3_step(cst) == SQLITE_ROW){ count = sqlite3_column_int(cst, 0); count_ok = 1; }
        sqlite3_finalize(cst);
    }
    /* A FAILED count query (BUSY/IOERR/corruption) must NOT look like an empty library - the
     * startup auto-scan keys off mdb_song_count()==0, and a spurious rescan on a transient error
     * is wrong. Only count_ok + count==0 is a genuine empty. */
    if(!count_ok){ g_load_err = 1; return 0; }
    if(count > 0 && !mdb_ensure_cap(count)) g_load_err = 1;   /* OOM can't grow to `count`: the load below
                                                               * fills the old g_cap and stops at rc==ROW,
                                                               * which otherwise reads as a benign race -
                                                               * flag it so a partial library isn't silent */
    if(g_cap == 0){ if(count > 0) g_load_err = 1; return 0; }   /* count>0 but no cap => OOM (error); count==0 => empty */
    sqlite3_stmt *st;
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(ALBUM,''),"
        "IFNULL(DURATION,0),ID,IFNULL(GENRE,'') FROM SONG "
        "WHERE lower(PATH) NOT LIKE '%.m4b' "   /* audiobooks live in the Books view, not the music lists */
        "ORDER BY 1 COLLATE NOCASE;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ g_load_err = 1; return 0; }
    int rc = SQLITE_DONE;
    while(g_n < g_cap && (rc = sqlite3_step(st)) == SQLITE_ROW){
        mdb_song_t *s = &g_songs[g_n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        snprintf(s->genre,  MDB_STR, "%s", colt(st,5));
        trim(s->title); trim(s->artist); trim(s->album); trim(s->genre);
    }
    /* rc==ROW means we stopped only because the buffer filled (more rows than COUNT -> a benign
     * add-between-queries race, not an error). Anything other than ROW/DONE is a mid-query
     * BUSY/IOERR -> a partial load that must NOT read as "empty" to the startup auto-scan. */
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) g_load_err = 1;
    sqlite3_finalize(st);
    return g_n;
}

int mdb_song_count(void){ return g_n; }
/* Total SONG rows INCLUDING audiobooks (which mdb_load excludes from g_songs). Used for "is the DB
 * empty?" and to size buffers that Favourites/Most-Played/Recent (unfiltered queries) also fill.
 * Returns -1 (NOT 0) if the query fails, so callers never mistake a query error for an empty DB. */
int mdb_total_song_count(void){
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st; int total = -1;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM SONG;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    /* Add BOOKS if that table exists: books live in their own table now, but an audiobook-only library
     * still isn't "empty" and must not trigger a rescan every boot. A missing BOOKS table (older DB /
     * feature off) simply fails the prepare and leaves the SONG-only count. */
    if(total >= 0 && sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM BOOKS;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) total += sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return total;   /* >=0 on success, -1 on query failure */
}
int mdb_load_failed(void){ return g_load_err; }   /* 1 if the last mdb_load hit a DB error (not just empty) */
const mdb_song_t *mdb_song(int i){ return (i>=0 && i<g_n) ? &g_songs[i] : NULL; }

/* On-demand PATH lookup by song ID (not cached in mdb_song_t to save RAM;
 * taps are rare so a per-tap query is fine). Returns 1 on success. */
int mdb_song_path(int id, char *out, int cap){
    if(!out || cap<=0) return 0;
    out[0]=0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE ID=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", colt(st,0));
    sqlite3_finalize(st);
    return out[0] ? 1 : 0;
}

/* On-demand SONG.ID for an absolute file path (folder browser -> library play). 0 if not indexed. */
int mdb_song_id_by_path(const char *path){
    if(!path || !*path) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int id = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return id;
}

/* On-demand ALBUM for a song ID. Returns 1 if a non-empty album was found. */
int mdb_song_album(int id, char *out, int cap){
    if(!out || cap<=0) return 0;
    out[0]=0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(ALBUM,'') FROM SONG WHERE ID=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    if(sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", colt(st,0));
    sqlite3_finalize(st);
    return out[0] ? 1 : 0;
}

/* Canonical ALBUM + ARTIST for a song by its PATH. The player's a2 metadata
 * strings can differ from the DB's stored values (whitespace/encoding/suffix),
 * so a Go-to-Album/Artist drill must match by the DB record, not the metadata
 * string, or it finds nothing. Returns 1 if the row was found. */
int mdb_song_meta_by_path(const char *path, char *album, int acap, char *artist, int arcap){
    if(album && acap>0) album[0]=0;
    if(artist && arcap>0) artist[0]=0;
    if(!path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(ALBUM,''),IFNULL(ARTIST,'') FROM SONG WHERE PATH=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    int found = 0;
    if(sqlite3_step(st) == SQLITE_ROW){
        if(album  && acap>0)  snprintf(album,  acap,  "%s", colt(st,0));
        if(artist && arcap>0) snprintf(artist, arcap, "%s", colt(st,1));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* The current/last "memory play" track (MEMORY_PLAY in song.db) + resume info,
 * so the UI can show what's playing on startup before any a2 frame arrives.
 * MEMORY_PLAY.MUSIC_ID maps to SONG.ID.  Returns 1 if a track was found. */
int mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing){
    if(out) memset(out, 0, sizeof *out);
    if(pos_ms) *pos_ms = 0;
    if(is_playing) *is_playing = 0;
    sqlite3 *d = db(); if(!d) return 0;
    int mid = 0, pos = 0, play = 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MUSIC_ID,IFNULL(POSITION,0),IFNULL(IS_PLAYING,0) "
                             "FROM MEMORY_PLAY ORDER BY ID DESC LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW){
            mid  = sqlite3_column_int(st,0);
            pos  = sqlite3_column_int(st,1);
            play = sqlite3_column_int(st,2);
        }
        sqlite3_finalize(st);
    }
    if(mid <= 0) return 0;
    if(sqlite3_prepare_v2(d, "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),"
                             "IFNULL(ALBUM,''),IFNULL(DURATION,0),ID FROM SONG WHERE ID=? LIMIT 1;",
                             -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, mid);
    int got = 0;
    if(sqlite3_step(st) == SQLITE_ROW){
        if(out){
            snprintf(out->title,  MDB_STR, "%s", colt(st,0));
            snprintf(out->artist, MDB_STR, "%s", colt(st,1));
            snprintf(out->album,  MDB_STR, "%s", colt(st,2));
            out->dur_ms = sqlite3_column_int(st,3);
            out->id     = sqlite3_column_int(st,4);
        }
        if(pos_ms) *pos_ms = pos;
        if(is_playing) *is_playing = play;
        got = 1;
    }
    sqlite3_finalize(st);
    return got;
}

/* The player rebuilds LIST_SONG_0 with these exact ORDER BY clauses before it
 * starts (reverse-engineered from mq_player's INSERT...SELECT SQL).  To make a
 * song tap land on the EXACT track, we compute the song's 1-based rank within
 * the same filtered+ordered set.  list_type: 1=all,2=artist,3=album,10=genre
 * (anything else = unfiltered all-songs order).
 * Returns the 1-based position (>=1), or 1 if it can't be resolved. */
int mdb_play_pos(int id, int list_type, const char *name){
    /* The title-code ordering the player uses for non-album lists. */
    static const char *ORDER_TAIL =
        "CASE WHEN IS_CUE=0 AND IS_ISO=0 THEN 1 WHEN IS_CUE=1 OR IS_ISO=1 THEN 2 END,"
        "CASE WHEN (IS_CUE=0 AND IS_ISO=0) THEN CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN NAME_CODE END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN ID END,"
        "CASE WHEN (IS_CUE=1 OR IS_ISO=1) THEN TRACK END";
    static const char *ORDER_ALBUM =
        "CASE WHEN DISC=0 THEN 1 ELSE 0 END,DISC,"
        "CASE WHEN TRACK=0 THEN 1 ELSE 0 END,TRACK,"
        "CASE WHEN TITLE IS NOT NULL THEN TITLE_CODE ELSE NAME_CODE END";

    sqlite3 *d = db(); if(!d) return 0;
    /* order + filter column are CONSTANTS chosen by list_type (never user text);
     * the playlist NAME is bound, not interpolated. */
    const char *order = (list_type==3) ? ORDER_ALBUM : ORDER_TAIL;
    const char *col   = (list_type==3) ? "ALBUM" : (list_type==2) ? "ARTIST" : (list_type==10) ? "GENRE" : NULL;
    char sql[1400];
    /* Exclude .m4b from the row-numbering so the position is BOOK-FREE, matching the music queue once books
     * are migrated out of SONG (the play path gates on that). Without this, a book still sitting in SONG
     * (migration pending) would shift the position and play the wrong track after the play-time migration. */
    if(col)
        snprintf(sql, sizeof sql,
            "SELECT pos FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY %s) pos FROM SONG WHERE %s=? AND lower(PATH) NOT LIKE '%%.m4b') WHERE ID=?;",
            order, col);
    else
        snprintf(sql, sizeof sql,
            "SELECT pos FROM (SELECT ID,ROW_NUMBER() OVER (ORDER BY %s) pos FROM SONG WHERE lower(PATH) NOT LIKE '%%.m4b') WHERE ID=?;", order);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if(col){ sqlite3_bind_text(st,1,name?name:"",-1,SQLITE_STATIC); sqlite3_bind_int(st,2,id); }
    else   { sqlite3_bind_int(st,1,id); }
    int pos = 0;   /* 0 = song not found in this list (caller must check) */
    if(sqlite3_step(st) == SQLITE_ROW){ int v=sqlite3_column_int(st,0); if(v>=1) pos=v; }
    sqlite3_finalize(st);
    return pos;
}

int mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap){
    int n = 0;
    char buf[MDB_STR]; snprintf(buf, MDB_STR, "%s", raw);
    /* split on ',' and ';' */
    char *p = buf, *start = buf;
    while(n < cap){
        if(*p==',' || *p==';' || *p==0){
            char c = *p; *p = 0;
            char tok[MDB_STR]; snprintf(tok, MDB_STR, "%s", start); trim(tok);
            if(tok[0]) snprintf(toks[n++], MDB_STR, "%s", tok);
            if(c==0) break;
            start = p+1;
        }
        p++;
    }
    return n;
}


typedef struct { const char *al; const char *ar; } mdb_ai_t;   /* (album, its artist) for sorting */
static int mdb_ai_cmp(const void *a, const void *b){
    return strcasecmp(((const mdb_ai_t*)a)->al, ((const mdb_ai_t*)b)->al);
}
int mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap){
    if(g_calb_n < 0){                                        /* build once - the FULL set - then cache */
        mdb_ai_t *tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof *tmp);
        int      *cnt = malloc((size_t)(g_n>0?g_n:1) * sizeof *cnt);
        if(!tmp || !cnt){ free(tmp); free(cnt); return 0; } /* transient OOM: don't cache, retry later */
        int m = 0;
        for(int i=0;i<g_n;i++) if(g_songs[i].album[0]){ tmp[m].al=g_songs[i].album; tmp[m].ar=g_songs[i].artist; m++; }
        qsort(tmp, (size_t)m, sizeof *tmp, mdb_ai_cmp);      /* sort by album, then group adjacent */
        /* dedup IN PLACE with NO cap, so a capped caller (e.g. the cover flow) can't truncate the shared
         * album cache and starve the full List view. */
        int n = 0;
        for(int i=0;i<m;i++){
            if(n>0 && !strcasecmp(tmp[i].al, tmp[n-1].al)) cnt[n-1]++;
            else { tmp[n]=tmp[i]; cnt[n]=1; n++; }
        }
        g_calb=malloc((size_t)(n>0?n:1)*MDB_STR); g_calb_ar=malloc((size_t)(n>0?n:1)*MDB_STR); g_calb_ct=malloc((size_t)(n>0?n:1)*sizeof(int));
        if(g_calb && g_calb_ar && g_calb_ct){
            for(int i=0;i<n;i++){ snprintf(g_calb[i],MDB_STR,"%s",tmp[i].al); snprintf(g_calb_ar[i],MDB_STR,"%s",tmp[i].ar); g_calb_ct[i]=cnt[i]; }
            g_calb_n=n;
        } else { free(g_calb); free(g_calb_ar); free(g_calb_ct); g_calb=NULL; g_calb_ar=NULL; g_calb_ct=NULL; free(tmp); free(cnt); return 0; }
        free(tmp); free(cnt);
        /* fall through to the capped copy-out */
    }
    int n = g_calb_n < cap ? g_calb_n : cap;                 /* return a cap-limited copy of the FULL cache */
    if(n>0){ memcpy(names,g_calb,(size_t)n*MDB_STR); memcpy(artists,g_calb_ar,(size_t)n*MDB_STR); memcpy(counts,g_calb_ct,(size_t)n*sizeof(int)); }
    return n;
}

/* Number of distinct albums in the library. Builds+caches the FULL set on first use (same cache as
 * mdb_albums), so a caller can size its buffers to the real count and never truncate a large library. */
int mdb_album_count(void){
    if(g_calb_n < 0) mdb_albums(NULL, NULL, NULL, 0);        /* cap 0 -> builds the full cache, copies nothing */
    return g_calb_n;                                          /* -1 if the build failed (OOM); >=0 = real count */
}

/* qsort comparator over the flat names[][MDB_STR] array (case-insensitive) */
static int mdb_name_ci_cmp(const void *a, const void *b){ return strcasecmp((const char*)a, (const char*)b); }

int mdb_artists(char names[][MDB_STR], int cap){
    if(g_cart_n < 0){                                        /* build once, then cache */
        /* Build into a temp sized to the EXACT token count (not the caller's cap), so a
         * collab-heavy library can't truncate before dedup. Count every credit (separators
         * on ',' / ';', +1) with NO per-song cap, and split each song's artists straight into
         * the buffer - a song crediting more than a handful of artists keeps all of them. */
        long maxtok = 0;
        for(int i=0;i<g_n;i++){
            int c=1; for(const char *p=g_songs[i].artist; *p; p++) if(*p==','||*p==';') c++;
            maxtok += c;
        }
        char (*buf)[MDB_STR] = malloc((size_t)(maxtok>0?maxtok:1) * MDB_STR);
        if(!buf) return 0;                                   /* transient OOM: leave -1, retry later */
        int n = 0;
        for(int i=0;i<g_n && n<maxtok;i++)
            n += mdb_split_artists(g_songs[i].artist, buf + n, (int)(maxtok - n));
        qsort(buf, (size_t)n, MDB_STR, mdb_name_ci_cmp);
        int w = 0;
        for(int i=0;i<n;i++)
            if(w==0 || strcasecmp(buf[w-1], buf[i]) != 0){
                if(w != i) memcpy(buf[w], buf[i], MDB_STR);
                w++;
            }
        g_cart = malloc((size_t)(w>0?w:1) * MDB_STR);
        if(g_cart){ if(w) memcpy(g_cart, buf, (size_t)w * MDB_STR); g_cart_n = w; }
        free(buf);
        if(g_cart_n < 0) return 0;                           /* cache alloc failed: retry later */
    }
    int n = g_cart_n < cap ? g_cart_n : cap;                 /* cache hit -> instant copy */
    if(n>0) memcpy(names, g_cart, (size_t)n * MDB_STR);
    return n;
}

/* Number of distinct (tokenized) artists. Builds the cache on first use. Lets the caller size its
 * buffer so mdb_artists never has to clip. */
int mdb_artist_count(void){
    if(g_cart_n < 0){ char dummy[MDB_STR]; mdb_artists((char (*)[MDB_STR])dummy, 0); }   /* build cache, copy nothing */
    return g_cart_n < 0 ? 0 : g_cart_n;
}

static int mdb_pstr_cmp(const void *a, const void *b){ return strcasecmp(*(const char*const*)a, *(const char*const*)b); }
int mdb_genres(char names[][MDB_STR], int *counts, int cap){
    if(g_cgen_n < 0){                                        /* build once, then cache */
        const char **tmp = malloc((size_t)(g_n>0?g_n:1) * sizeof(char*));
        if(!tmp) return 0;
        int m=0;
        for(int i=0;i<g_n;i++) if(g_songs[i].genre[0]) tmp[m++]=g_songs[i].genre;
        qsort(tmp, (size_t)m, sizeof(char*), mdb_pstr_cmp);
        int n=0;
        for(int i=0;i<m;i++){
            if(n>0 && !strcasecmp(tmp[i], names[n-1])) counts[n-1]++;
            else { if(n>=cap) break; snprintf(names[n],MDB_STR,"%s",tmp[i]); counts[n]=1; n++; }
        }
        free(tmp);
        g_cgen=malloc((size_t)(n>0?n:1)*MDB_STR); g_cgen_ct=malloc((size_t)(n>0?n:1)*sizeof(int));
        if(g_cgen && g_cgen_ct){ if(n){ memcpy(g_cgen,names,(size_t)n*MDB_STR); memcpy(g_cgen_ct,counts,(size_t)n*sizeof(int)); } g_cgen_n=n; }
        else { free(g_cgen); free(g_cgen_ct); g_cgen=NULL; g_cgen_ct=NULL; }
        return n;
    }
    int n = g_cgen_n < cap ? g_cgen_n : cap;                 /* cache hit -> instant copy */
    if(n>0){ memcpy(names,g_cgen,(size_t)n*MDB_STR); memcpy(counts,g_cgen_ct,(size_t)n*sizeof(int)); }
    return n;
}

int mdb_genre_songs(const char *genre, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(!strcasecmp(g_songs[i].genre, genre)) out[n++] = &g_songs[i];
    return n;
}

/* Sort key for album ordering: the player lists album tracks by disc, then track, then title.
 * DISC=0 / TRACK=0 mean "unknown" and sort LAST (matches mq_player's ORDER_ALBUM CASE). `ord`
 * is the song's original (title-order) index, used as a stable tiebreak within a disc+track. */
typedef struct { int disc, track, ord; const mdb_song_t *s; } alb_key_t;
static int alb_cmp(const void *a, const void *b){
    const alb_key_t *x = a, *y = b;
    if(x->disc  != y->disc ) return (x->disc  < y->disc ) ? -1 : 1;
    if(x->track != y->track) return (x->track < y->track) ? -1 : 1;
    return x->ord - y->ord;
}

/* Up to `cap` SONG.IDs in an album, UNSORTED (linear scan only, no per-song DISC/TRACK query). For cover
 * lookup, where playback order is irrelevant - avoids mdb_album_songs' disc/track sort DB queries. */
int mdb_album_track_ids(const char *album, int *ids, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++) if(!strcasecmp(g_songs[i].album, album)) ids[n++] = g_songs[i].id;
    return n;
}

int mdb_album_songs(const char *album, const mdb_song_t **out, int cap){
    if(cap <= 0) return 0;
    /* Select the album set exactly as before (case-insensitive match on the trimmed name) so
     * WHICH songs appear is unchanged. Then reorder that set by the player's disc/track order so
     * the displayed rows match playback; g_songs' title order is the stable tiebreak. If the DB is
     * unavailable or the query won't prepare, the whole list keeps its title order (the old
     * behavior); a single per-song disc/track read miss just sorts that one song last. Taps play
     * by song ID via mdb_play_pos regardless, so a tap always lands on the right track. */
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(!strcasecmp(g_songs[i].album, album)) out[n++] = &g_songs[i];
    if(n <= 1) return n;

    sqlite3 *d = db();
    if(!d) return n;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT DISC,TRACK FROM SONG WHERE ID=?;", -1, &st, NULL) != SQLITE_OK)
        return n;
    alb_key_t *k = malloc((size_t)n * sizeof *k);
    if(!k){ sqlite3_finalize(st); return n; }
    for(int i=0;i<n;i++){
        int disc = 0, track = 0;
        sqlite3_reset(st);
        sqlite3_bind_int(st, 1, out[i]->id);
        if(sqlite3_step(st) == SQLITE_ROW){ disc = sqlite3_column_int(st,0); track = sqlite3_column_int(st,1); }
        k[i].disc  = (disc  > 0) ? disc  : INT_MAX;   /* unknown -> last */
        k[i].track = (track > 0) ? track : INT_MAX;
        k[i].ord   = i;                               /* stable: keep title order within ties */
        k[i].s     = out[i];
    }
    sqlite3_finalize(st);
    qsort(k, (size_t)n, sizeof *k, alb_cmp);
    for(int i=0;i<n;i++) out[i] = k[i].s;
    free(k);
    return n;
}

/* True if `artist` is one of the ',' / ';'-separated credits in `raw` (case-insensitive, trimmed).
 * No token-count cap, so a song crediting many artists matches under EACH of them - the artist
 * list and the artist's song list agree. */
static int artist_credited(const char *raw, const char *artist){
    size_t alen = strlen(artist);
    for(const char *s = raw; *s; ){
        while(*s==' '||*s=='\t') s++;                       /* skip leading space */
        const char *start = s;
        while(*s && *s!=',' && *s!=';') s++;                /* token = [start, end) */
        const char *end = s;
        while(end>start && (end[-1]==' '||end[-1]=='\t'||end[-1]=='\r'||end[-1]=='\n')) end--;   /* trim trailing, matching trim() */
        if((size_t)(end-start)==alen && alen && strncasecmp(start, artist, alen)==0) return 1;
        if(*s) s++;                                         /* step past the separator */
    }
    return 0;
}

int mdb_artist_songs(const char *artist, const mdb_song_t **out, int cap){
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++)
        if(artist_credited(g_songs[i].artist, artist)) out[n++] = &g_songs[i];
    return n;
}

int mdb_favorites(mdb_song_t *out, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    /* MY_LOVE.ID is its OWN autoincrement, NOT a SONG.ID - but a tapped favourite plays via
     * SONG.ID, so resolve the real SONG.ID by PATH here (NULL -> 0 if the song left the library). */
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(ALBUM,''),"
        "IFNULL(DURATION,0),(SELECT ID FROM SONG WHERE SONG.PATH=MY_LOVE.PATH LIMIT 1) "
        "FROM MY_LOVE WHERE lower(PATH) NOT LIKE '%.m4b' ORDER BY 1 COLLATE NOCASE;";   /* books have their own view */
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        mdb_song_t *s = &out[n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        s->genre[0] = 0;
        trim(s->title); trim(s->artist); trim(s->album);
    }
    sqlite3_finalize(st); return n;
}

/* ---- Play history (diskOS PLAY_STATS): Most-Played / Recently-Played ------- */
/* Record one play: bump PLAYS + stamp LAST_PLAYED for this path. Best-effort. */
void mdb_record_play(const char *path){
    if(!path || !path[0]) return;
    sqlite3 *d = db(); if(!d) return;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d,
        "INSERT INTO PLAY_STATS(PATH,PLAYS,LAST_PLAYED) VALUES(?1,1,?2) "
        "ON CONFLICT(PATH) DO UPDATE SET PLAYS=PLAYS+1, LAST_PLAYED=?2;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text (st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    sqlite3_step(st); sqlite3_finalize(st);
}
/* Fill `out` with songs from PLAY_STATS joined to SONG, ordered by plays (by_recent=0) or by
 * last-played time (by_recent=1). Playback is by SONG.ID, same as favourites. Returns count. */
static int mdb_stats_list(mdb_song_t *out, int cap, int by_recent){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql = by_recent
      ? "SELECT IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')),IFNULL(S.ARTIST,''),IFNULL(S.ALBUM,''),"
        "IFNULL(S.DURATION,0),S.ID FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH "
        "WHERE lower(S.PATH) NOT LIKE '%.m4b' ORDER BY P.LAST_PLAYED DESC LIMIT ?1;"   /* books have their own view */
      : "SELECT IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')),IFNULL(S.ARTIST,''),IFNULL(S.ALBUM,''),"
        "IFNULL(S.DURATION,0),S.ID FROM PLAY_STATS P JOIN SONG S ON S.PATH=P.PATH "
        "WHERE P.PLAYS>0 AND lower(S.PATH) NOT LIKE '%.m4b' ORDER BY P.PLAYS DESC, P.LAST_PLAYED DESC LIMIT ?1;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, cap);
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        mdb_song_t *s = &out[n++];
        snprintf(s->title,  MDB_STR, "%s", colt(st,0));
        snprintf(s->artist, MDB_STR, "%s", colt(st,1));
        snprintf(s->album,  MDB_STR, "%s", colt(st,2));
        s->dur_ms = sqlite3_column_int(st,3);
        s->id     = sqlite3_column_int(st,4);
        s->genre[0] = 0;
        trim(s->title); trim(s->artist); trim(s->album);
    }
    sqlite3_finalize(st); return n;
}
int mdb_mostplayed(mdb_song_t *out, int cap){ return mdb_stats_list(out, cap, 0); }
int mdb_recent(mdb_song_t *out, int cap){ return mdb_stats_list(out, cap, 1); }

/* ---- Audiobooks (v1: single-file .m4b books) ------------------------------ */
int mdb_is_book_path(const char *path){
    if(!DISKOS_AUDIOBOOKS) return 0;   /* compile-time gate (currently 1): when built out, a .m4b is treated as ordinary audio, not a book */
    if(!path) return 0;
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".m4b");
}

/* List audiobooks (one book per .m4b), each joined to its saved progress. Ordered so a book with the
 * most recent bookmark leads ("continue listening"), then by add time. */
/* Returns the number of books (>=0), or -1 on a DB/read error (db unavailable, prepare failed, or a step
 * error). A caller must distinguish -1 (show a retry state) from 0 (genuinely no audiobooks), or a
 * transient DB failure looks like an empty library. */
int mdb_books(book_t *out, int max){
    if(max <= 0) return 0;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    const char *sql =
        "SELECT S.ID, IFNULL(S.TITLE,IFNULL(S.NAME,'Untitled')), IFNULL(S.ARTIST,''), S.PATH, "
        "IFNULL(B.POSITION_MS,0), IFNULL(B.COMPLETED,0) "
        "FROM BOOKS S LEFT JOIN BOOK_PROGRESS B ON B.BOOK_KEY = S.PATH "
        "ORDER BY IFNULL(B.UPDATED_AT,0) DESC, IFNULL(S.ADD_TIME,0) DESC, S.ID DESC;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int n = 0, rc;
    while(n < max && (rc = sqlite3_step(st)) == SQLITE_ROW){
        book_t *b = &out[n++];
        b->id = sqlite3_column_int(st, 0);
        snprintf(b->title,  MDB_STR, "%s", colt(st, 1));
        snprintf(b->author, MDB_STR, "%s", colt(st, 2));
        snprintf(b->path, sizeof b->path, "%s", colt(st, 3));
        b->position_ms = (long)sqlite3_column_int64(st, 4);
        b->completed   = sqlite3_column_int(st, 5);
        trim(b->title); trim(b->author);
    }
    int err = (n == 0 && rc != SQLITE_DONE && rc != SQLITE_ROW);   /* first step errored (not "no rows") -> report as error */
    sqlite3_finalize(st);
    return err ? -1 : n;
}

/* Saved resume position for a book. Returns 1 (fills member_out/position_ms) if a bookmark exists. */
/* Returns 1 = bookmark found, 0 = no bookmark, -1 = read error (DB unavailable / prepare failed). A -1
 * must NOT be treated as "start from zero": a caller that then checkpoints would erase a real bookmark
 * that was only temporarily unreadable. */
int mdb_book_progress(const char *key, char *member_out, int member_cap, long *position_ms, int *completed_out, long *updated_out){
    if(completed_out) *completed_out = 0;
    if(updated_out) *updated_out = 0;
    if(!key) return -1;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st; int found = 0;
    /* read POSITION, COMPLETED and UPDATED_AT together so a caller can decide resume-vs-restart from the
     * LATEST saved state (the book may have finished since the list was built) and how long it's been idle. */
    if(sqlite3_prepare_v2(d, "SELECT MEMBER_PATH,POSITION_MS,COMPLETED,UPDATED_AT FROM BOOK_PROGRESS WHERE BOOK_KEY=? LIMIT 1;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        if(member_out && member_cap > 0) snprintf(member_out, (size_t)member_cap, "%s", colt(st, 0));
        if(position_ms) *position_ms = (long)sqlite3_column_int64(st, 1);
        if(completed_out) *completed_out = sqlite3_column_int(st, 2);
        if(updated_out) *updated_out = (long)sqlite3_column_int64(st, 3);
        found = 1;
    } else if(rc != SQLITE_DONE){
        found = -1;   /* BUSY / IOERR etc. -> a read error, not "no bookmark" */
    }
    sqlite3_finalize(st);
    return found;
}

/* Checkpoint a book's position (upsert). Called periodically during playback and on pause/exit.
 * Returns 1 if the row was actually written, 0 on any failure (so the caller can retry rather than
 * advance its save-throttle timestamp and silently drop the bookmark). */
int mdb_book_save(const char *key, const char *member_path, long position_ms, int completed){
    if(!key || !member_path) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "INSERT INTO BOOK_PROGRESS(BOOK_KEY,MEMBER_PATH,POSITION_MS,UPDATED_AT,COMPLETED) VALUES(?1,?2,?3,?4,?5) "
        "ON CONFLICT(BOOK_KEY) DO UPDATE SET MEMBER_PATH=?2,POSITION_MS=?3,UPDATED_AT=?4,COMPLETED=?5;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, member_path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, position_ms < 0 ? 0 : position_ms);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(st, 5, completed ? 1 : 0);
    int ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return ok;
}

/* Audiobook isolation: the stock player builds the music queue as an unfiltered SELECT FROM SONG, so a
 * book must NOT be in SONG (or it leaks into Play-All/shuffle). Instead a book plays from a RESERVED
 * custom-playlist slot: the player resolves a list_type-5 play by seq via
 *   SELECT LIST_ID FROM CUSTOM_PLAYLIST_INDEX ORDER BY LIST_ID LIMIT 1 OFFSET <seq>
 * and diskOS's play frame always sends seq 0, so the reserved slot must sit at the LOWEST LIST_ID.
 * Device-verified: the player re-reads BOTH the registry and the membership FRESH on each play (no
 * mq_player restart needed), and a member with no SONG row still builds + decodes. This idempotently
 * ensures the reserved registry row exists and sets its single member to `path`, copying the book's
 * metadata from SONG when present, else a bare-path row. Returns 1 on success. */
int mdb_reserved_slot_set(const char *path){
    sqlite3 *d = db(); if(!d || !path || !*path) return 0;
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 1;
    if(sqlite3_exec(d, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME,M3U_PATH) VALUES ("
                       XSTR(DISKOS_RSV_LISTID) ",'diskos-book','');", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok){
        sqlite3_stmt *st;
        /* DURATION must be NON-ZERO: a 0-length slot makes the player treat the book as already finished
         * (instant EOF in Single mode). Use the real duration when known, else a large placeholder - the
         * player overwrites it with the true length once it decodes the moov (diskOS shows the player's
         * live duration, not this value). */
        const char *sql =
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DURATION,IS_CUE,IS_ISO,IS_DSD,OFFSET) "
            "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,"
            "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),0,0,0,0 FROM BOOKS WHERE PATH=?1;";
        if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
            if(sqlite3_step(st) != SQLITE_DONE) ok = 0;
            sqlite3_finalize(st);
        } else ok = 0;
        if(ok && sqlite3_changes(d) == 0){   /* book not in SONG (books live outside SONG) -> bare-path row (self-contained; the player only needs PATH to decode) */
            sqlite3_stmt *s2;
            if(sqlite3_prepare_v2(d, "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,DURATION) VALUES ("
                                     XSTR(DISKOS_RSV_LISTID) ",?1,?1,?1,86400000);", -1, &s2, NULL) == SQLITE_OK){
                sqlite3_bind_text(s2, 1, path, -1, SQLITE_STATIC);
                if(sqlite3_step(s2) != SQLITE_DONE) ok = 0;
                sqlite3_finalize(s2);
            } else ok = 0;
        }
    }
    if(ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return 1;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);   /* a failed COMMIT (e.g. reader BUSY) must not leave the txn open with the membership half-applied */
    return 0;
}

/* Reserved-slot playback for a user PLAYLIST. diskOS's type-5 play always resolves to seq 0, so before
 * the reservation a playlist play hit the wrong list, and after it hit the reserved BOOK slot. This
 * copies the playlist's members INTO the reserved slot so seq 0 plays the playlist isolated (and also
 * finally makes diskOS playlist playback work). Non-zero DURATION per row (0 -> placeholder) so the
 * player doesn't instant-skip a 0-length CUSTOM_PLAYLIST member. Returns the member count, 0 on fail/empty. */
int mdb_reserved_slot_set_playlist(long pid){
    sqlite3 *d = db(); if(!d) return 0;
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 1, n = 0;
    if(sqlite3_exec(d, "INSERT OR IGNORE INTO CUSTOM_PLAYLIST_INDEX (LIST_ID,LIST_NAME,M3U_PATH) VALUES ("
                       XSTR(DISKOS_RSV_LISTID) ",'diskos-book','');", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=" XSTR(DISKOS_RSV_LISTID) ";", 0, 0, 0) != SQLITE_OK) ok = 0;
    if(ok){
        sqlite3_stmt *st;
        const char *sql =
            "INSERT INTO CUSTOM_PLAYLIST (PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST) "
            "SELECT " XSTR(DISKOS_RSV_LISTID) ",PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,"
            "(CASE WHEN DURATION>0 THEN DURATION ELSE 86400000 END),ALBUM_ARTIST FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1;";
        if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) == SQLITE_OK){
            sqlite3_bind_int64(st, 1, (sqlite3_int64)pid);
            if(sqlite3_step(st) != SQLITE_DONE) ok = 0; else n = sqlite3_changes(d);
            sqlite3_finalize(st);
        } else ok = 0;
    }
    if(ok && n > 0 && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return n;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}

/* One-time migration: move any .m4b rows left in SONG (a pre-BOOKS build, or a device whose non-empty
 * DB skipped the startup rescan) into BOOKS, so books leave the music queue even without a rescan.
 * Idempotent + guarded so it's a no-op once BOOKS holds them. Safe to call at every startup. */
/* Returns 1 when books are (now) out of every music-facing store - either nothing needed moving, or the
 * relocation committed. Returns 0 ONLY when the state could not be established or the move failed (DB null,
 * detection query errored, BEGIN/COMMIT lost a lock race) - so the caller can RETRY rather than leave a
 * .m4b sitting in the stock player's unfiltered music queue. A failed detection is NOT treated as
 * "already clean" (that was the old bug: it silently returned success and left books in SONG). */
int mdb_migrate_books(void){
    /* Runs regardless of DISKOS_AUDIOBOOKS: a .m4b left in ANY music-facing store (SONG, favourites
     * MY_LOVE, or a non-reserved playlist) leaks into the stock player's music/favourites/playlist
     * queues. Sanitising is safe with the feature off too (books just sit hidden in BOOKS). */
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS BOOKS (ID INTEGER PRIMARY KEY autoincrement,"
        "PATH TEXT UNIQUE, NAME TEXT, TITLE TEXT, ARTIST TEXT, ALBUM TEXT, GENRE TEXT,"
        "DURATION BIGINT, ADD_TIME INT8);", 0, 0, 0);
    sqlite3_stmt *c; int have = 0, detect_ok = 0;
    if(sqlite3_prepare_v2(d,
        "SELECT 1 FROM SONG WHERE lower(PATH) LIKE '%.m4b' "
        "UNION ALL SELECT 1 FROM MY_LOVE WHERE lower(PATH) LIKE '%.m4b' "
        "UNION ALL SELECT 1 FROM CUSTOM_PLAYLIST WHERE lower(PATH) LIKE '%.m4b' AND PLAYLIST_ID<>" XSTR(DISKOS_RSV_LISTID)
        " LIMIT 1;", -1, &c, NULL) == SQLITE_OK){
        int rc = sqlite3_step(c);
        if(rc == SQLITE_ROW){ have = 1; detect_ok = 1; }
        else if(rc == SQLITE_DONE){ have = 0; detect_ok = 1; }   /* proven empty */
        sqlite3_finalize(c);                                     /* else a step error -> detect_ok stays 0 */
    }
    if(!detect_ok) return 0;   /* couldn't establish the state -> report failure so the caller retries */
    if(!have) return 1;        /* no book left in a music-facing store -> already sanitised */
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK) return 0;
    /* OR IGNORE, not OR REPLACE: a book already in BOOKS keeps its metadata (ADD_TIME, any real
     * DURATION) - a re-index of the same path in SONG carries only DURATION=0 + a fresh ADD_TIME. */
    int ok = sqlite3_exec(d,
        "INSERT OR IGNORE INTO BOOKS(PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME) "
        "SELECT PATH,NAME,TITLE,ARTIST,ALBUM,GENRE,DURATION,ADD_TIME FROM SONG WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM SONG WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM MY_LOVE WHERE lower(PATH) LIKE '%.m4b';",0,0,0)==SQLITE_OK
      && sqlite3_exec(d, "DELETE FROM CUSTOM_PLAYLIST WHERE lower(PATH) LIKE '%.m4b' AND PLAYLIST_ID<>" XSTR(DISKOS_RSV_LISTID) ";",0,0,0)==SQLITE_OK;
    if(ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return 1;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);   /* a failed COMMIT (reader BUSY) must not leave the txn open on the persistent connection */
    return 0;
}

/* Remove a song from MY_LOVE (favourites) by its ID. Direct DB delete - the
 * player re-reads MY_LOVE on demand, so refreshing the list reflects it. */
/* `id` is a SONG.ID (mdb_favorites now returns SONG.IDs, not MY_LOVE.IDs), so remove the
 * favourite by matching PATH - MY_LOVE.ID is a different id space. */
int mdb_unfavorite(int id){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "DELETE FROM MY_LOVE WHERE PATH=(SELECT PATH FROM SONG WHERE ID=?1);", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, id);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(d);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed > 0;   /* true only if a MY_LOVE row was removed */
}

/* ---- custom playlists (PLAYLIST_INFO + CUSTOM_PLAYLIST) ------------------ */
/* The player builds a playlist by copying SONG rows into CUSTOM_PLAYLIST keyed
 * by a PLAYLIST_ID; PLAYLIST_INFO holds the names. We manage both directly. */
#define PL_COLS "PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE"
#define PL_SRC  "PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,NAME_CODE,TITLE_CODE,ALBUM_CODE,ARTIST_CODE,GENRE_CODE,ADD_TIME,SAMPLE_RATE,BIT_PER_SAMPLE,CHANNELS,BIT_RATE,SONG_MIMETYPE,SONG_PRODUCTION_YEAR,IS_SELECT,ALBUM_ARTIST,ALBUM_ARTIST_CODE"

/* Create a new playlist; returns its id (>0) or 0 on failure. */
/* run a one-row, one-column integer query. Sets *ok=1 and returns the value on success; *ok=0 and
 * returns 0 on any failure (prepare error, no row) - callers treat that as a hard error. */
static sqlite3_int64 mdb_scalar_i64(sqlite3 *d, const char *sql, int *ok){
    sqlite3_stmt *st; sqlite3_int64 v = 0; *ok = 0;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if(sqlite3_step(st) == SQLITE_ROW){ v = sqlite3_column_int64(st, 0); *ok = 1; }
    sqlite3_finalize(st);
    return v;
}

/* Allocate the next monotone export UID (>=1) and advance the persisted counter. The caller MUST already
 * be inside a transaction (SAVEPOINT or BEGIN), so the read+bump is atomic. Returns 0 on any failure or
 * overflow. A UID handed out here is never reused for the life of the DB.
 *
 * THREADING: the read and the bump are two calls on the shared g_db connection. That is safe only because
 * playlist creation/import is UI-thread-only (mdb_playlist_create from npmenus, mdb_import_m3u_* from
 * settings - both LVGL handlers on the one UI thread; the scanner thread never creates playlists). This is
 * not a new constraint: mdb_playlist_create already opens a SAVEPOINT on g_db, and SQLite cannot run two
 * transactions on one connection, so concurrent creation was never supported. Any future off-thread
 * creator must serialise the WHOLE create/allocate transaction, not rely on FULLMUTEX (which serialises
 * single calls, not transactions). */
static long long diskos_next_export_uid(sqlite3 *d){
    /* Ensure the export tables exist even if db()'s startup DDL lost a busy-timeout race (it ignores its
     * CREATE results). Every UID assignment funnels through here first, so this guarantees a create never
     * fails permanently just because the one-time init DDL was starved. IF NOT EXISTS is a cheap no-op once
     * they exist; it runs inside the caller's write transaction. */
    if(sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_META(K TEXT PRIMARY KEY, V INTEGER NOT NULL);", 0, 0, 0) != SQLITE_OK) return 0;
    if(sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS DISKOS_PL_EXPORT(PID INTEGER PRIMARY KEY, UID INTEGER NOT NULL);", 0, 0, 0) != SQLITE_OK) return 0;
    int ok = 0;
    sqlite3_int64 next = mdb_scalar_i64(d,
        "SELECT COALESCE((SELECT V FROM DISKOS_META WHERE K='pl_export_seq'),1);", &ok);
    if(!ok || next < 1 || next >= 0x7fffffffffffffffLL) return 0;
    char sql[96];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO DISKOS_META(K,V) VALUES('pl_export_seq',%lld);", (long long)next + 1);
    if(sqlite3_exec(d, sql, 0, 0, 0) != SQLITE_OK) return 0;
    return (long long)next;
}
/* Bind pid -> an already-allocated UID (overwriting any stale row a reused pid inherited). Caller in a
 * txn. Returns 1 on success. */
static int diskos_assign_export_uid_val(sqlite3 *d, sqlite3_int64 pid, long long uid){
    if(uid <= 0) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "INSERT OR REPLACE INTO DISKOS_PL_EXPORT(PID,UID) VALUES(?,?);", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}
/* Allocate a fresh UID and bind it to pid. Caller in a txn. Returns 1 on success. */
static int diskos_assign_export_uid(sqlite3 *d, sqlite3_int64 pid){
    return diskos_assign_export_uid_val(d, pid, diskos_next_export_uid(d));
}

long mdb_playlist_create(const char *name){
    sqlite3 *d = db(); if(!d) return 0;
    /* Store the CANONICAL (trimmed) name - the same form mdb_playlists shows and export writes. Leading/
     * trailing whitespace in the raw name would otherwise let a display "Rock" (from stored " Rock") miss
     * playlist_id_by_name("Rock") and create a duplicate on re-import. Trim at the source so stored,
     * displayed, exported and idempotency-checked names all agree. */
    char nm[MDB_STR]; snprintf(nm, sizeof nm, "%s", name ? name : ""); trim(nm); name = nm;
    sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS PLAYLIST_INFO "
                    "(ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT, ADD_TIME INT8);", 0, 0, 0);
    /* CUSTOM_PLAYLIST (the membership table) is SHARED with the stock player, keyed by PLAYLIST_ID.
     * PLAYLIST_INFO's AUTOINCREMENT hands back low ids (1,2,3...) that can collide with a STOCK
     * playlist's id. The old code DELETED those "orphan" rows - which ERASED the stock playlist's
     * membership (data loss; reproduced by the 2026-09-05 firmware audit: new diskOS id=1 wiped stock
     * id 1's members). Instead we place the new playlist ABOVE every id known to any playlist table
     * (shared membership, the stock index if present, and our own rows), so it can never touch another
     * playlist's rows and always starts empty. Done atomically in a SAVEPOINT; fail closed (roll back
     * and return 0) on ANY sql error so we never report a colliding/misplaced playlist as created. */
    /* Did THIS call open the outermost transaction? (autocommit on = yes; off = we are nested inside a
     * caller's BEGIN, e.g. import_m3u_file). Determines cleanup: if we own it, a failed RELEASE must be
     * force-closed with a plain ROLLBACK, or the shared connection is left stuck IN a transaction and every
     * later BEGIN/SAVEPOINT (book playback, import) fails. If we are nested, we only unwind our savepoint
     * and leave the outer transaction for its owner. */
    int owns_txn = sqlite3_get_autocommit(d);
    if(sqlite3_exec(d, "SAVEPOINT plcreate;", 0, 0, 0) != SQLITE_OK) return 0;
    sqlite3_stmt *st;
    sqlite3_int64 id = 0;
    int ok = 1;

    if(sqlite3_prepare_v2(d, "INSERT INTO PLAYLIST_INFO (NAME,ADD_TIME) VALUES (?, strftime('%s','now'));",
                          -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_bind_text(st, 1, name?name:"", -1, SQLITE_STATIC) != SQLITE_OK) ok = 0;
        else if(sqlite3_step(st) != SQLITE_DONE) ok = 0;
        sqlite3_finalize(st);
    } else ok = 0;

    if(ok){
        id = sqlite3_last_insert_rowid(d);
        int q;
        sqlite3_int64 hi = mdb_scalar_i64(d, "SELECT COALESCE(MAX(PLAYLIST_ID),0) FROM CUSTOM_PLAYLIST;", &q);
        if(!q) ok = 0;
        if(ok){
            /* other diskOS playlists (EXCLUDE the row we just inserted, else id<=hi is always true and we
             * renumber every time). AUTOINCREMENT already keeps our own ids distinct, but we UPDATE ids
             * upward without advancing the sequence, so a later low id must still clear existing rows. */
            char qs[96]; snprintf(qs, sizeof qs, "SELECT COALESCE(MAX(ID),0) FROM PLAYLIST_INFO WHERE ID<>%lld;", (long long)id);
            sqlite3_int64 v = mdb_scalar_i64(d, qs, &q);
            if(!q) ok = 0; else if(v > hi) hi = v;
        }
        if(ok){
            /* The stock playlist index. Empty stock playlists have NO membership rows, so only this
             * table reveals their ids. If we cannot READ an index that exists, we cannot establish
             * collision safety -> fail closed. A genuinely-absent table is safe to skip. */
            int has;
            sqlite3_int64 t = mdb_scalar_i64(d,
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='CUSTOM_PLAYLIST_INDEX' COLLATE NOCASE;", &has);
            if(!has) ok = 0;
            else if(t > 0){
                int q2; sqlite3_int64 v = mdb_scalar_i64(d, "SELECT COALESCE(MAX(LIST_ID),0) FROM CUSTOM_PLAYLIST_INDEX;", &q2);
                if(!q2) ok = 0; else if(v > hi) hi = v;
            }
        }
        if(ok && id <= hi){
            if(hi >= 0x7fffffffffffffffLL) ok = 0;   /* overflow guard */
            else {
                sqlite3_int64 safe = hi + 1;
                if(sqlite3_prepare_v2(d, "UPDATE PLAYLIST_INFO SET ID=? WHERE ID=?;", -1, &st, NULL) == SQLITE_OK){
                    sqlite3_bind_int64(st, 1, safe); sqlite3_bind_int64(st, 2, id);
                    if(sqlite3_step(st) != SQLITE_DONE || sqlite3_changes(d) != 1) ok = 0;
                    sqlite3_finalize(st);
                } else ok = 0;
                if(ok) id = safe;
            }
        }
        if(ok && id > 0x7fffffffLL) ok = 0;   /* the API returns long (32-bit here); refuse to narrow wrongly */
        /* bind this (possibly pid-reused) playlist to a FRESH monotone export UID, overwriting any stale
         * DISKOS_PL_EXPORT row a recycled pid inherited. Inside the savepoint, so it is atomic with the
         * create; fail closed so every diskOS playlist has a reuse-proof export identity. */
        if(ok && !diskos_assign_export_uid(d, id)) ok = 0;
    }

    /* commit only if RELEASE actually succeeds (a busy DB can fail it, leaving the txn open) */
    if(ok && sqlite3_exec(d, "RELEASE plcreate;", 0, 0, 0) == SQLITE_OK) return (long)id;
    sqlite3_exec(d, "ROLLBACK TO plcreate;", 0, 0, 0);
    sqlite3_exec(d, "RELEASE plcreate;", 0, 0, 0);
    /* ROLLBACK TO does not end the transaction, and the RELEASE above can also fail under a reader lock. If
     * we opened the outer transaction, force it closed so the shared connection isn't left mid-transaction
     * (which would break every later BEGIN/SAVEPOINT). If we're nested, leave the outer txn to its owner. */
    if(owns_txn && !sqlite3_get_autocommit(d)) sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* Copy a song (by path, from the SONG table) into a playlist. 1 on success. */
/* Add one SONG-resident path to a custom playlist. Returns 1 if a row was actually inserted,
 * 0 otherwise. `hard_err` (optional) distinguishes a real DB failure (prepare/step error) from a
 * benign 0-row result (path not in SONG, or already a member): *hard_err is set to 1 ONLY on a real
 * failure, so an importer can tell "nothing to add" from "the DB write failed mid-import". */
int mdb_playlist_add_song_ex(long pid, const char *path, int *hard_err){
    if(hard_err) *hard_err = 0;
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d){ if(hard_err) *hard_err = 1; return 0; }
    sqlite3_stmt *st;
    const char *sql = "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") "
                      "SELECT ?," PL_SRC " FROM SONG WHERE PATH=?;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK){ if(hard_err) *hard_err = 1; return 0; }
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE){ if(hard_err) *hard_err = 1; return 0; }
    /* OR IGNORE + SELECT-from-SONG can succeed (DONE) yet insert 0 rows (path not in
     * SONG, or already present) - report a real add only when a row changed. */
    return (sqlite3_changes(d) > 0) ? 1 : 0;
}
int mdb_playlist_add_song(long pid, const char *path){
    return mdb_playlist_add_song_ex(pid, path, NULL);
}
/* ---- single-book playback scope --------------------------------------------------------------------
 * A reserved, hidden custom playlist that holds ONLY the currently-playing audiobook. Playing it
 * (list_type 5) isolates the book: the player's queue is the one file, so next/prev/end can't wander
 * into music. The name carries a leading 0x01 so it can't collide with a user playlist and is filtered
 * out of the Playlists view. */
#define BOOK_SCOPE_NAME "\x01""diskos-book"
static long book_scope_pid(void){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; long pid = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM PLAYLIST_INFO WHERE NAME=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, BOOK_SCOPE_NAME, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) pid = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if(pid <= 0) pid = mdb_playlist_create(BOOK_SCOPE_NAME);
    return pid;
}
/* Point the reserved book-scope playlist at exactly `path`, returning its id (0 on failure). */
long mdb_book_scope_set(const char *path){
    if(!path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    long pid = book_scope_pid();
    if(pid <= 0) return 0;
    sqlite3_stmt *st;   /* clear the previous member, then add this book (copied from its SONG row) */
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid); sqlite3_step(st); sqlite3_finalize(st);
    }
    if(!mdb_playlist_add_song(pid, path)) return 0;
    return pid;
}

/* Bulk-add every SONG in a group (all songs of an ALBUM/ARTIST/GENRE) to a custom playlist.
 * `col` MUST be one of the whitelisted column names (it is interpolated into SQL, so it can never be
 * user text). Skips duplicates (INSERT OR IGNORE against UNIQUE(PLAYLIST_ID,PATH,TRACK)). Returns the
 * number of rows actually added. */
int mdb_playlist_add_group(long pid, const char *col, const char *val){
    if(pid<=0 || !col || !val || !val[0]) return 0;
    if(strcmp(col,"ALBUM") && strcmp(col,"ARTIST") && strcmp(col,"GENRE")) return 0;   /* whitelist */
    sqlite3 *d = db(); if(!d) return 0;
    char sql[800];
    snprintf(sql, sizeof sql,
             "INSERT OR IGNORE INTO CUSTOM_PLAYLIST (" PL_COLS ") SELECT ?," PL_SRC " FROM SONG WHERE %s=?;", col);
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, val, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? sqlite3_changes(d) : 0;
}
/* 1 if the playlist already contains this song path. */
int mdb_playlist_has_song(long pid, const char *path){
    if(pid<=0 || !path || !path[0]) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT 1 FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? AND PATH=? LIMIT 1;",
                          -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int yes = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return yes;
}
/* Remove the song at 1-based ORDINAL position from a custom playlist. The ordinal matches
 * mdb_playlist_songs()'s display order (ORDER BY PATH, TRACK), so the UI can remove by row index.
 * Deletes by the row's (PATH,TRACK) identity - UNIQUE(PLAYLIST_ID,PATH,TRACK) makes it exact and
 * avoids any ROWID assumption. Returns 1 if a row was deleted. */
int mdb_playlist_remove_at(long pid, int position){
    if(pid<=0 || position<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "DELETE FROM CUSTOM_PLAYLIST WHERE ID = ("
        "SELECT ID FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?1 ORDER BY PATH, TRACK LIMIT 1 OFFSET ?2);";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    sqlite3_bind_int(st, 2, position-1);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return (rc == SQLITE_DONE && sqlite3_changes(d) > 0) ? 1 : 0;
}
int sd_write_begin(void); void sd_write_end(void);   /* SD-write ownership guard (main.c, M18) */
/* Sanitise a playlist name into a safe filename base (keep alnum/space/-/_; others -> _). */
static void pl_sanitize(const char *name, char *out, int cap){
    int j=0;
    for(const char *s = (name && name[0]) ? name : "playlist"; *s && j < cap - 1; s++){
        char c = *s;
        out[j++] = ((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c==' '||c=='-'||c=='_') ? c : '_';
    }
    out[j] = 0;
    if(!out[0]){ out[0]='p'; out[1]=0; }
}
/* Export a custom playlist to /tmp/sdcard/<name>-<uid>.m3u (an M3U the stock player + others can read).
 * Uses the SD-write ownership guard so it never races a USB-Storage export. Returns 1 on success.
 * `outname` (<=cap) receives the file name actually written (for the toast).
 *
 * The filename identity is "<name>-<uid>", where uid is a monotone counter value bound to the playlist at
 * creation (DISKOS_PL_EXPORT). Names alone collide two ways a name-based file would not survive - distinct
 * names sanitise to the same base ("Rock/Pop" and "Rock:Pop" -> "Rock_Pop"), and distinct bases alias on a
 * case-insensitive FAT/exFAT card ("Rock" vs "rock"). The pid is not reuse-proof (the allocator recycles
 * it after a delete) and ADD_TIME is not either (two creations can share a whole second). The uid counter
 * only ever increases and is persisted, and a recycled pid is rebound to a fresh uid at create, so two
 * different playlists (any lifetime) never share a uid -> never target the same file. */
/* Is `path` an existing diskOS export of the playlist called `name`? (starts with #EXTM3U and carries our
 * #PLAYLIST:<name> directive). Only such a file is safe to overwrite on re-export; anything else - a user's
 * own .m3u, or a stale export of a different playlist - must be preserved. */
static int pl_export_is_ours(const char *path, const char *name){
    FILE *f = fopen(path, "r"); if(!f) return 0;   /* unreadable -> treat as not-ours, don't clobber */
    char line[256]; int ok = 0, i = 0;
    while(i < 8 && fgets(line, sizeof line, f)){
        if(i == 0 && strncmp(line, "#EXTM3U", 7) != 0) break;   /* not an extended-M3U -> not ours */
        if(strncmp(line, "#PLAYLIST:", 10) == 0){
            char *v = line + 10; size_t L = strlen(v);
            while(L && (v[L-1]=='\n' || v[L-1]=='\r')) v[--L] = 0;
            if(name && strcmp(v, name) == 0) ok = 1;            /* same playlist name -> our prior export */
            break;
        }
        i++;
    }
    fclose(f);
    return ok;
}

static int mdb_playlist_export_leased(long pid, const char *name, char *outname, int cap){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    char safe[96]; pl_sanitize(name, safe, sizeof safe);
    /* fetch this playlist's export uid; lazily assign one (in its own txn) for a playlist created before
     * this feature existed. If neither read nor assign yields a uid, REFUSE the export rather than fall
     * back to a non-unique name that could clobber another playlist's file. */
    long long uid = 0; int have_uid = 0;
    { sqlite3_stmt *st;
      if(sqlite3_prepare_v2(d, "SELECT UID FROM DISKOS_PL_EXPORT WHERE PID=?;", -1, &st, NULL) == SQLITE_OK){
          sqlite3_bind_int64(st, 1, pid);
          if(sqlite3_step(st) == SQLITE_ROW){ uid = sqlite3_column_int64(st, 0); have_uid = 1; }
          sqlite3_finalize(st);
      } }
    if(!have_uid && sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK){
        long long u = diskos_next_export_uid(d);
        int okk = (u > 0) && diskos_assign_export_uid_val(d, pid, u);
        if(okk && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK){ uid = u; have_uid = 1; }
        else sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    }
    if(!have_uid) return 0;
    char base[160];   /* safe(<=95) + "-" + uid (<=19 digits) [+ "_N" disambiguator], no truncation of the unique suffix */
    snprintf(base, sizeof base, "%s-%lld", safe, uid);
    char path[200]; snprintf(path, sizeof path, "/tmp/sdcard/%s.m3u", base);
    /* Never overwrite a file diskOS didn't write: if the destination exists and isn't our own export of
     * THIS playlist (a user's .m3u, or a stale export left after a DB rebuild reset the uid counter), pick a
     * free "<base>_N.m3u" instead of clobbering it. */
    if(access(path, F_OK) == 0 && !pl_export_is_ours(path, name)){
        int found = 0;
        for(int nsfx = 2; nsfx <= 999; nsfx++){
            char cand[200]; snprintf(cand, sizeof cand, "/tmp/sdcard/%s_%d.m3u", base, nsfx);
            if(access(cand, F_OK) != 0 || pl_export_is_ours(cand, name)){   /* free, OR our own prior suffixed export -> (re)use it */
                char nb[176]; snprintf(nb, sizeof nb, "%s_%d", base, nsfx);
                snprintf(base, sizeof base, "%s", nb);
                snprintf(path, sizeof path, "%s", cand);
                found = 1; break;
            }
        }
        if(!found) return 0;   /* every candidate belongs to someone else -> refuse rather than overwrite a user file */
    }
    char tmp[208];  snprintf(tmp, sizeof tmp, "%s.part", path);
    if(!sd_write_begin()) return 0;                 /* card host-owned or an export is pending -> skip */
    FILE *f = fopen(tmp, "w");                       /* write a temp; swap over the real file only when complete */
    if(!f){ sd_write_end(); return 0; }
    int ok = 1;
    if(fprintf(f, "#EXTM3U\n") < 0) ok = 0;
    /* Record the real playlist name in a standard extended-M3U directive (a comment, so any other player
     * ignores it). Import reads it back to restore the true name and stay idempotent, so re-importing an
     * exported "<name>-<uid>.m3u" updates/skips the same playlist instead of creating a "<name>-<uid>"
     * duplicate. Strip any newline from the name so the directive stays one line. */
    if(name && name[0]){
        char nm1[160]; int k=0;
        for(const char *s=name; *s && k<(int)sizeof nm1-1; s++){ char c=*s; if(c!='\r'&&c!='\n') nm1[k++]=c; }
        nm1[k]=0;
        if(nm1[0] && fprintf(f, "#PLAYLIST:%s\n", nm1) < 0) ok = 0;
    }
    sqlite3_stmt *st = NULL;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? ORDER BY PATH, TRACK;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_bind_int64(st, 1, pid) == SQLITE_OK){
            int rc;
            while((rc = sqlite3_step(st)) == SQLITE_ROW){ const char *p = colt(st,0); if(p && p[0] && fprintf(f, "%s\n", p) < 0) ok = 0; }
            if(rc != SQLITE_DONE) ok = 0;   /* a step error -> the export is incomplete, not a success */
        } else { ok = 0; }
        sqlite3_finalize(st);               /* always finalize a prepared stmt (bind-fail path too) */
    } else { ok = 0; }                      /* prepare failure -> not a valid export */
    if(ferror(f)) ok = 0;                 /* any earlier stream write error */
    if(fflush(f) != 0) ok = 0;
    /* fsync the data to the card before the rename makes it visible, so a yanked/remounted SD can't
     * surface a rename pointing at unflushed content - and so a deferred writeback error (EIO) is caught
     * HERE rather than being swallowed by fclose. A driver that simply doesn't implement fsync
     * (EINVAL/ENOSYS/EOPNOTSUPP) is tolerated; any other error fails the export. */
    if(ok){
        int fd = fileno(f);
        if(fd >= 0 && fsync(fd) != 0 && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) ok = 0;
    }
    if(fclose(f) != 0) ok = 0;
    /* swap into place only on full success; a failed/partial export leaves any previous good
     * <name>.m3u untouched (writing straight to the real path with "w" would truncate it up front). */
    if(ok && rename(tmp, path) != 0) ok = 0;
    if(ok){
        /* fsync the containing directory so the rename entry itself is durable on a non-journaled
         * card (file data was fsync'd above). Best-effort: an unsupported dir fsync just no-ops. */
        int dfd = open("/tmp/sdcard", O_RDONLY | O_DIRECTORY);
        if(dfd >= 0){ fsync(dfd); close(dfd); }
    }
    if(!ok) remove(tmp);
    sd_write_end();
    if(ok && outname && cap > 0) snprintf(outname, cap, "%s.m3u", base);
    return ok;
}
/* Rename a playlist. */
/* Write a custom EQ curve into the player's PEQ table (STYLE_PRESET slot). The player
 * reloads bands -> biquad coeffs when 0689 selects this preset. Format captured from the
 * stock UI (RE_CATALOGUE §3): PARAMS_JSON = 10 band objects, gain/qValue as STRINGS. */
int mdb_set_peq(int style_preset, double master_gain, const char *params_json){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE PEQ SET MASTER_GAIN=?, PARAMS_JSON=? WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_double(st, 1, master_gain);
    sqlite3_bind_text(st, 2, params_json?params_json:"", -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, style_preset);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if(rc != SQLITE_DONE) return 0;
    if(sqlite3_changes(d) == 0){
        /* slot doesn't exist yet -> create it. Name USER slots (STYLE_PRESET 11..20) "USER1".."USER10"
         * to match the stock player's labels; anything else stays "Custom". */
        char nm[16] = "Custom";
        if(style_preset >= 11 && style_preset <= 20) snprintf(nm, sizeof nm, "USER%d", style_preset - 10);
        if(sqlite3_prepare_v2(d, "INSERT INTO PEQ (STYLE_NAME, MASTER_GAIN, PARAMS_JSON, STYLE_PRESET) VALUES (?,?,?,?);", -1, &st, NULL) != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, nm, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 2, master_gain);
        sqlite3_bind_text(st, 3, params_json?params_json:"", -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, style_preset);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if(rc != SQLITE_DONE) return 0;
    }
    return 1;
}
/* Read a PEQ slot's stored curve (by STYLE_PRESET). Fills *master_out (dB) and up to 10 band
 * gains (integer dB, in band order) parsed from PARAMS_JSON. Returns 1 if the slot row exists,
 * 0 if not (caller treats a missing row as a flat/empty slot). gains_out must hold >=10 ints.
 * Lets the EQ editor show + preserve an un-edited USER slot's existing curve instead of
 * overwriting a stock preset with flat. Integer-only parse (the stored gains are "N.0"). */
/* Read a slot's stored PEQ curve. Returns 1 = found (out filled), 0 = no such slot (out is a real flat
 * curve), -1 = READ FAILED (DB unavailable / prepare error - the true curve is UNKNOWN, out is flat only
 * as a placeholder). The -1 vs 0 split matters: a caller must never persist the placeholder-flat of a
 * failed read over a real stock curve. */
int mdb_get_peq(int style_preset, int *master_out, int *gains_out){
    if(gains_out) for(int i=0;i<10;i++) gains_out[i]=0;
    if(master_out) *master_out=0;
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MASTER_GAIN, PARAMS_JSON FROM PEQ WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, style_preset);
    int found = 0, rc = sqlite3_step(st);
    if(rc == SQLITE_ROW){
        found = 1;
        if(master_out) *master_out = sqlite3_column_int(st, 0);   /* stored REAL "N.0" -> int */
        const char *js = (const char*)sqlite3_column_text(st, 1);
        if(gains_out && js){
            int n=0; const char *p=js;
            while(n<10 && (p=strstr(p, "\"gain\"")) != NULL){
                p += 6;                                  /* past the "gain" key */
                while(*p==':'||*p==' '||*p=='"') p++;     /* skip ':' and the opening quote/space */
                int neg=0; if(*p=='-'){ neg=1; p++; }
                int g=0; while(*p>='0'&&*p<='9' && g<100000){ g=g*10+(*p-'0'); p++; }   /* bound: no signed-overflow UB on a garbage-long number */
                gains_out[n++] = neg ? -g : g;           /* integer dB part (caller clamps to -12..12) */
            }
        }
    }
    sqlite3_finalize(st);
    if(!found && rc != SQLITE_DONE) return -1;   /* a step error (BUSY/corrupt) is a read failure, not an empty slot */
    return found;
}

/* Is a PEQ slot's stored curve representable by diskOS's 10-band graphic editor (fixed freqs, peaking
 * filter, Q=0.7, integer dB)? 1 = yes (or empty/absent - nothing to lose); 0 = it uses parametric params
 * (non-zero filterType, a non-0.7 qValue, or a fractional gain) that the graphic editor would FLATTEN, so
 * the caller must not overwrite it; -1 = read failed. Lets the editor preserve a stock parametric preset
 * instead of destroying it (full parametric editing is out of scope until the filterType enum is RE'd). */
/* Parse an INTEGER-VALUED JSON number at p: [-]?digits, optionally an all-zero ".0..." fraction, and NO
 * exponent. Rejects a non-zero fraction (32.5), an exponent (1e-1, 0.7e1), or a malformed number. On success
 * store the value + the end pointer, return 1. This is why the graphic editor (integer dB) can round-trip it. */
/* Parse a jsmn token's text as an INTEGER value ([-]digits, optional all-zero fraction, NO exponent), fully
 * consumed within the token. The token bounds (from a real JSON parse) already validate delimiters/quotes/
 * escapes, so "1 junk", 32.5, 1e-1 and trailing junk are all rejected. */
static int peq_tok_int(const char *js, const jsmntok_t *t, long *out){
    const char *p = js + t->start, *end = js + t->end;
    int neg = 0; if(p < end && *p == '-'){ neg = 1; p++; }
    if(p >= end || !(*p>='0'&&*p<='9')) return 0;
    if(*p == '0' && p+1 < end && p[1]>='0'&&p[1]<='9') return 0;   /* leading zero (032) -> invalid */
    long v = 0; while(p < end && *p>='0'&&*p<='9'){ if(v > 10000000) return 0; v = v*10 + (*p-'0'); p++; }
    if(p < end && *p == '.'){ p++; if(p >= end || !(*p>='0'&&*p<='9')) return 0;   /* "32." -> require a fraction digit */
                              while(p < end && *p>='0'&&*p<='9'){ if(*p != '0') return 0; p++; } }
    if(p != end) return 0;   /* exponent / extra dot / junk inside the token */
    *out = neg ? -v : v; return 1;
}
static int peq_key_is(const char *js, const jsmntok_t *t, const char *s){   /* object key == s (raw, no escapes) */
    int n = t->end - t->start;
    return n == (int)strlen(s) && strncmp(js + t->start, s, (size_t)n) == 0;
}

/* Is a PEQ slot representable by diskOS's 10-band graphic editor? Uses a REAL JSON parse (jsmn) so escaped
 * keys, nested objects, trailing junk, whitespace and quoting are all handled correctly. Graphic REQUIRES:
 * PARAMS_JSON is an array of EXACTLY 10 flat objects; band i has filterType==0 (number), frequency==the fixed
 * GFREQ[i] (number, position-preserving), qValue=="0.7" (string), gain=an integer string in [-12,12]; and
 * MASTER_GAIN is an integer in [-12,12]. Anything else (parametric params, out-of-range gains the editor
 * would clamp, or a structure it can't round-trip) is NOT graphic -> the caller preserves it. */
int mdb_peq_is_graphic(int style_preset){
    static const int GFREQ[10] = {32,64,125,250,500,1000,2000,4000,8000,16000};
    sqlite3 *d = db(); if(!d) return -1;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT MASTER_GAIN, PARAMS_JSON FROM PEQ WHERE STYLE_PRESET=?;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(st, 1, style_preset);
    int rc = sqlite3_step(st), graphic = 1;
    if(rc == SQLITE_ROW){
        /* master gain: an integer in [-12,12], fully consumed (a fraction/exponent/out-of-range would be
         * dropped or clamped by the editor on save). An empty/NULL master is 0 -> fine. */
        const char *ms = (const char*)sqlite3_column_text(st, 0);
        if(ms && ms[0]){
            const char *p = ms; int neg = 0, ok = 1; long mv = 0;
            if(*p == '-'){ neg = 1; p++; }
            if(!(*p>='0'&&*p<='9')) ok = 0;
            while(*p>='0'&&*p<='9'){ mv = (mv < 100000) ? mv*10 + (*p-'0') : 100000; p++; }  /* saturate: no signed overflow on an oversized value; out-of-range is rejected below */
            if(*p == '.'){ p++; while(*p>='0'&&*p<='9'){ if(*p != '0') ok = 0; p++; } }
            if(*p) ok = 0;                 /* trailing junk */
            if(neg) mv = -mv;
            if(!ok || mv < -12 || mv > 12) graphic = 0;
        }
        const char *js = (const char*)sqlite3_column_text(st, 1);
        int jlen = js ? sqlite3_column_bytes(st, 1) : 0;
        if(graphic && js){
            jsmn_parser jp; jsmntok_t tok[256]; jsmn_init(&jp);
            int nt = jsmn_parse(&jp, js, jlen, tok, (unsigned)(sizeof tok / sizeof tok[0]));
            if(nt < 1 || tok[0].type != JSMN_ARRAY || tok[0].size != 10) graphic = 0;
            else {
                /* jsmn is a lenient tokenizer: reject anything but whitespace after the array root, so a
                 * "valid curve + trailing junk / second value" can't slip through. */
                int re = tok[0].end;
                while(re < jlen && (js[re]==' '||js[re]=='\t'||js[re]=='\n'||js[re]=='\r')) re++;
                if(re != jlen) graphic = 0;
                int ti = 1;   /* walk the array's tokens in preorder */
                for(int band = 0; graphic && band < 10; band++){
                    if(ti >= nt || tok[ti].type != JSMN_OBJECT){ graphic = 0; break; }
                    int nfields = tok[ti].size; ti++;
                    int have_ft = 0, have_fr = 0, have_q = 0, have_g = 0;
                    for(int f = 0; graphic && f < nfields; f++){
                        if(ti + 1 >= nt || tok[ti].type != JSMN_STRING){ graphic = 0; break; }
                        const jsmntok_t *k = &tok[ti], *val = &tok[ti + 1];
                        if(val->type == JSMN_OBJECT || val->type == JSMN_ARRAY){ graphic = 0; break; }  /* no nested values */
                        long v;
                        if(peq_key_is(js, k, "filterType")){ if(have_ft || val->type != JSMN_PRIMITIVE || !peq_tok_int(js, val, &v) || v != 0) graphic = 0; else have_ft = 1; }
                        else if(peq_key_is(js, k, "frequency")){ if(have_fr || val->type != JSMN_PRIMITIVE || !peq_tok_int(js, val, &v) || v != GFREQ[band]) graphic = 0; else have_fr = 1; }
                        else if(peq_key_is(js, k, "qValue")){ if(have_q || val->type != JSMN_STRING || !(val->end - val->start == 3 && strncmp(js + val->start, "0.7", 3) == 0)) graphic = 0; else have_q = 1; }
                        else if(peq_key_is(js, k, "gain")){ if(have_g || val->type != JSMN_STRING || !peq_tok_int(js, val, &v) || v < -12 || v > 12) graphic = 0; else have_g = 1; }
                        /* "position" and any other flat field are ignored */
                        ti += 2;   /* key + flat value */
                    }
                    if(graphic && !(have_ft && have_fr && have_q && have_g)) graphic = 0;
                }
            }
        }
    }
    sqlite3_finalize(st);
    if(rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;   /* step error -> read failed */
    return graphic;
}

int mdb_playlist_rename(long pid, const char *name){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "UPDATE PLAYLIST_INFO SET NAME=? WHERE ID=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name?name:"", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, pid);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    return (rc == SQLITE_DONE && sqlite3_changes(d) > 0) ? 1 : 0;   /* real rename only */
}
/* Delete a playlist: removes only the playlist + its membership rows; the SONG
 * table (the actual songs/files) is never touched. */
int mdb_playlist_delete(long pid){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    /* atomic: drop membership + the playlist row together, or roll back (no orphan rows).
     * Every step's rc is checked: a failed BEGIN aborts; any DELETE failure or a failed
     * COMMIT (e.g. SQLITE_BUSY) rolls back and reports failure - so the transaction can
     * never be left open on the shared g_db connection. */
    if(sqlite3_exec(d, "BEGIN;", 0, 0, 0) != SQLITE_OK) return 0;
    sqlite3_stmt *st;
    int memb_ok = 0, info_ok = 0, changed = 0;
    if(sqlite3_prepare_v2(d, "DELETE FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid); memb_ok = (sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
    }
    if(memb_ok && sqlite3_prepare_v2(d, "DELETE FROM PLAYLIST_INFO WHERE ID=?;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_int64(st, 1, pid);
        if(sqlite3_step(st) == SQLITE_DONE){ info_ok = 1; changed = (sqlite3_changes(d) > 0); }
        sqlite3_finalize(st);
    }
    /* success only if both DELETEs stepped clean AND COMMIT actually succeeded */
    if(memb_ok && info_ok && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK) return changed;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* List a playlist's songs in the SAME order the stock player builds its play queue, so the display
 * position we send on a tap (ui_play_playlist -> ordinal) selects the song the user actually sees.
 * The stock CUSTOM_PLAYLIST has UNIQUE(PLAYLIST_ID, PATH, TRACK) (confirmed in the v2.40 player + stock
 * UI CREATE TABLE). The player's queue-build SELECT has NO ORDER BY; on the bundled SQLite the planner
 * satisfies its WHERE PLAYLIST_ID=? via that unique index, so rows come back in (PATH, TRACK) order, not
 * rowid (reproduced against the real index). We order the same way to match that observed v2.40 order (a
 * different player query plan could in principle differ; this matches current behaviour). The old code
 * ordered by TITLE_CODE (alphabetical), which disagreed with the player and could play the wrong song. */
int mdb_playlist_songs(long pid, mdb_song_t *out, int cap){
    if(pid<=0) return 0;
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    const char *sql =
        "SELECT IFNULL(TITLE,IFNULL(NAME,'Untitled')),IFNULL(ARTIST,''),IFNULL(DURATION,0) "
        "FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=? ORDER BY PATH, TRACK;";
    if(sqlite3_prepare_v2(d, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        snprintf(out[n].title,  MDB_STR, "%s", colt(st,0));
        snprintf(out[n].artist, MDB_STR, "%s", colt(st,1));
        out[n].album[0]=0; out[n].genre[0]=0;
        out[n].dur_ms = sqlite3_column_int(st,2); out[n].id = 0;
        trim(out[n].title); trim(out[n].artist);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* Track count in a playlist. */
int mdb_playlist_count(long pid){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM CUSTOM_PLAYLIST WHERE PLAYLIST_ID=?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, pid);
    int n=0; if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st,0);
    sqlite3_finalize(st);
    return n;
}

/* playlist id by exact name, or 0 if none (so re-import is idempotent). */
static long playlist_id_by_name(const char *name){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; long id = 0;
    if(sqlite3_prepare_v2(d, "SELECT ID FROM PLAYLIST_INFO WHERE NAME=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if(sqlite3_step(st) == SQLITE_ROW) id = (long)sqlite3_column_int64(st,0);
        sqlite3_finalize(st);
    }
    return id;
}
/* Resolve an m3u entry to a real SONG.PATH: exact match first, else by filename
 * (so m3u files with relative paths or a different root still resolve). 1=found. */
/* Escape LIKE metachars (% _ \) so a filename can't wildcard-match the wrong song. Returns 1 on
 * success, 0 if the escaped form would not fit (caller must then reject the match rather than use a
 * silently-truncated pattern that could match the wrong path). */
static int like_escape(const char *s, char *out, int cap){
    int j=0;
    for(; *s; s++){
        int need = (*s=='%'||*s=='_'||*s=='\\') ? 2 : 1;
        if(j + need > cap - 1) return 0;                 /* would truncate -> fail */
        if(need==2) out[j++]='\\';
        out[j++]=*s;
    }
    out[j]=0; return 1;
}
/* Fill out[] and return 1 iff EXACTLY ONE song PATH matches the LIKE pattern (unambiguous). LIMIT 2
 * so a second row means "ambiguous" -> reject. Uniqueness is only established when the step AFTER the
 * single row is SQLITE_DONE (an error there leaves it unproven). A path too long for out[cap] is also
 * rejected rather than silently truncated (it would fail playlist insertion). */
/* `herr` (optional) is set to 1 on a real DB error (prepare/bind/step failure) so a caller can tell it
 * apart from a genuine no-match/ambiguous/too-long reject (all of which leave *herr untouched). */
static int resolve_like(sqlite3 *d, const char *like, char *out, int cap, int *herr){
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH LIKE ? ESCAPE '\\' LIMIT 2;", -1, &st, NULL)!=SQLITE_OK){ if(herr)*herr=1; return 0; }
    if(sqlite3_bind_text(st, 1, like, -1, SQLITE_STATIC)!=SQLITE_OK){ sqlite3_finalize(st); if(herr)*herr=1; return 0; }
    int r1 = sqlite3_step(st);
    if(r1 != SQLITE_ROW){ sqlite3_finalize(st); if(r1 != SQLITE_DONE && herr)*herr=1; return 0; }   /* DONE=no match; else DB error */
    const char *p = (const char*)sqlite3_column_text(st, 0);   /* NULL here = OOM conversion error, not a real path */
    if(!p || !p[0]){ sqlite3_finalize(st); if(herr)*herr=1; return 0; }
    int plen = (int)strlen(p);
    int fits = (plen < cap);
    if(fits) memcpy(out, p, (size_t)plen + 1);          /* copy while the row pointer is still valid */
    int r2 = sqlite3_step(st);                          /* SQLITE_DONE proves uniqueness; ROW=ambiguous; else error */
    int fz = sqlite3_finalize(st);
    if(r2 != SQLITE_DONE || !fits){
        if((r2 != SQLITE_DONE && r2 != SQLITE_ROW) && herr)*herr=1;   /* a step error (not the benign ambiguous ROW) */
        if(cap>0) out[0]=0; return 0;
    }
    if(fz != SQLITE_OK){ if(herr)*herr=1; if(cap>0) out[0]=0; return 0; }   /* deferred error at finalize */
    return 1;
}
/* Resolve an m3u entry to a real SONG.PATH: exact match first, then the most SPECIFIC path suffix the
 * entry provides (the full relative path - disambiguates the same basename in different album dirs),
 * then a bare basename. A suffix/basename match is accepted ONLY if UNambiguous; otherwise we reject
 * rather than add the wrong track to a playlist (L36). 1=found. */
static int song_resolve(const char *entry, char *out, int cap, int *herr){
    sqlite3 *d = db(); if(!d){ if(herr)*herr=1; return 0; }
    sqlite3_stmt *st; int got = 0;
    if(sqlite3_prepare_v2(d, "SELECT PATH FROM SONG WHERE PATH=? LIMIT 1;", -1, &st, NULL) == SQLITE_OK){
        int rc;
        if(sqlite3_bind_text(st, 1, entry, -1, SQLITE_STATIC) != SQLITE_OK){ if(herr)*herr=1; rc = SQLITE_ERROR; }
        else rc = sqlite3_step(st);
        if(rc == SQLITE_ROW){
            const char *pth = (const char*)sqlite3_column_text(st, 0);   /* NULL = OOM conversion error, not a real empty path */
            if(pth && pth[0]){ snprintf(out, cap, "%s", pth); got = 1; }
            else if(herr)*herr=1;
        } else if(rc != SQLITE_DONE && herr)*herr=1;      /* a step error, not a clean no-match */
        if(sqlite3_finalize(st) != SQLITE_OK && herr)*herr=1;   /* a deferred error surfaces at finalize */
    } else if(herr)*herr=1;                               /* prepare failed -> DB error */
    if(got) return 1;
    if(herr && *herr) return 0;                           /* stop probing once the DB is erroring */
    /* strip a leading "/" or "./" so a relative entry ("Album/Track.flac") reads as a path suffix */
    const char *rel = entry; while(rel[0]=='/' || (rel[0]=='.' && rel[1]=='/')) rel += (rel[0]=='/')?1:2;
    if(!rel[0]) return 0;
    char esc[600], like[608];
    /* 1) most-specific: the full relative path (only if it carries directory components) */
    if(strchr(rel, '/') && like_escape(rel, esc, sizeof esc)){
        snprintf(like, sizeof like, "%%/%s", esc);
        if(resolve_like(d, like, out, cap, herr)) return 1;
        if(herr && *herr) return 0;
    }
    /* 2) bare basename - accepted only if unambiguous */
    const char *base = strrchr(rel, '/'); base = base ? base+1 : rel;
    if(!base[0]) return 0;
    if(like_escape(base, esc, sizeof esc)){
        snprintf(like, sizeof like, "%%/%s", esc);
        if(resolve_like(d, like, out, cap, herr)) return 1;
    }
    if(!(herr && *herr))
        fprintf(stderr,"m3u resolve: '%s' unresolved (no match, ambiguous, or too long)\n", entry);
    return 0;
}
/* Import one .m3u/.m3u8 file as a playlist. The name is the file's own "#PLAYLIST:" directive if present
 * (so a file diskOS exported round-trips under its true name), else the filename stem. Skips if a playlist
 * with that name already exists (idempotent re-scan). Returns tracks added, or 0 if skipped/empty. */
static int import_m3u_file(const char *m3u_path){
    FILE *fp = fopen(m3u_path, "r"); if(!fp) return 0;
    const char *b = strrchr(m3u_path, '/'); b = b ? b+1 : m3u_path;
    char name[160]; snprintf(name, sizeof name, "%s", b);
    char *dot = strrchr(name, '.'); if(dot) *dot = 0;
    /* Pre-scan the header for a "#PLAYLIST:" directive. If found, it is the canonical name - use it for both
     * the idempotency check and the created playlist, so re-importing an exported "<name>-<uid>.m3u"
     * resolves to the same "<name>" instead of a "<name>-<uid>" duplicate. Blank/whitespace-only lines and
     * other comments (#EXTM3U/#EXTINF) are skipped; the scan stops at the first real (non-comment) entry.
     * The directive value is trim()'d to the same canonical form the export writes (mdb_playlists trims the
     * displayed name), so the round-trip name is consistent. */
    /* Read the header a logical line at a time with fgetc so the byte count is exact (fgets + strlen
     * miscount an embedded NUL, and can't tell a full buffer from a real line). The directive lives in the
     * short comment header, so: an over-long line (past a small cap) is NOT a directive and ENDS the header
     * scan (it's a track or junk); a blank line is skipped; a real (non-#) entry ends the scan; a
     * #PLAYLIST: value is trim()'d to the display/export canonical form, and an empty one is ignored so a
     * later real directive still wins. */
    { char hl[192];
      for(;;){
          int n = 0, ch, overlong = 0, binary = 0;
          while((ch = fgetc(fp)) != EOF && ch != '\n'){
              if(ch == 0) binary = 1;                  /* a NUL byte -> this is not a text header line */
              if(n < (int)sizeof hl - 1) hl[n++] = (char)ch; else overlong = 1;   /* count EVERY byte (incl '\r') */
          }
          if(ch == EOF && n == 0) break;               /* end of file */
          if(binary || overlong) break;                /* binary or too-long line -> past the text header */
          if(n > 0 && hl[n-1] == '\r') n--;            /* strip ONE terminal CR (CRLF); keep any interior CR */
          hl[n] = 0;
          char *q = hl;
          if((unsigned char)q[0]==0xEF && (unsigned char)q[1]==0xBB && (unsigned char)q[2]==0xBF) q += 3;
          while(*q==' '||*q=='\t') q++;
          if(!*q) continue;                            /* blank / whitespace-only line -> keep scanning */
          if(!strncmp(q, "#PLAYLIST:", 10)){
              char v[192]; snprintf(v, sizeof v, "%s", q+10);
              trim(v);                                 /* trim the WHOLE value before the name-length limit */
              if(v[0]){ snprintf(name, sizeof name, "%s", v); break; }
              continue;                                /* empty directive -> keep looking for a real one */
          }
          if(*q != '#') break;                         /* first real entry line: no directive present */
          /* any other comment (#EXTM3U, #EXTINF, ...) -> keep scanning */
      }
      rewind(fp);
    }
    trim(name);   /* canonicalise the FINAL name (directive or filename stem, post-truncation) so the
                   * idempotency lookup matches what mdb_playlist_create stores (which also trims) */
    if(playlist_id_by_name(name) > 0){ fclose(fp); return 0; }   /* already imported */
    char dir[400]; snprintf(dir, sizeof dir, "%s", m3u_path);
    char *sl = strrchr(dir, '/'); if(sl) *sl = 0; else dir[0] = 0;
    /* Import the whole file ATOMICALLY: the playlist row + every member commit together, or nothing
     * does. A mid-import failure (a track fails to insert, or the final COMMIT loses a lock race) then
     * leaves NO partial playlist behind, so playlist_id_by_name can't block a clean retry on the next
     * scan. This replaces a best-effort delete that could itself fail under the same lock and strand the
     * partial forever. mdb_playlist_create's inner SAVEPOINT nests cleanly inside this transaction. */
    sqlite3 *d = db(); if(!d){ fclose(fp); return 0; }
    if(sqlite3_exec(d, "BEGIN IMMEDIATE;", 0, 0, 0) != SQLITE_OK){ fclose(fp); return 0; }   /* writer busy -> retry next scan */
    long pid = 0; int added = 0; int db_err = 0; char line[700];
    while(fgets(line, sizeof line, fp)){
        /* A logical line longer than the buffer would otherwise be split by fgets into
         * fragments, each resolved as a bogus separate track (e.g. a tail "other.flac").
         * When the buffer filled with no newline, peek the next byte: EOF or a line break
         * means the buffered content is a complete entry whose terminator just didn't fit -
         * keep it; any other byte means a genuinely over-long line - drain it and skip. */
        size_t ll = strlen(line);
        if(ll == sizeof line - 1 && line[ll-1] != '\n'){
            int ch = fgetc(fp);
            if(ch != EOF && ch != '\n' && ch != '\r'){
                while((ch = fgetc(fp)) != '\n' && ch != EOF){ }
                continue;                                    /* over-long: skip, don't fragment */
            }
        }
        char *nl = strpbrk(line, "\r\n"); if(nl) *nl = 0;
        char *p = line; while(*p==' '||*p=='\t') p++;
        if((unsigned char)p[0]==0xEF && (unsigned char)p[1]==0xBB && (unsigned char)p[2]==0xBF) p += 3; /* UTF-8 BOM */
        while(*p==' '||*p=='\t') p++;
        for(char *q=p; *q; q++) if(*q=='\\') *q='/';            /* Windows backslash -> '/' so paths resolve */
        if(!*p || *p=='#') continue;                            /* comment / #EXTINF / blank */
        char entry[700]; int en;
        if(*p=='/') en = snprintf(entry, sizeof entry, "%s", p);
        else        en = snprintf(entry, sizeof entry, "%s/%s", dir, p);
        int entry_ok = (en > 0 && en < (int)sizeof entry);   /* joined path fits (don't resolve a truncated one) */
        char songpath[700];
        int rherr = 0;
        int found = (entry_ok && song_resolve(entry, songpath, sizeof songpath, &rherr));
        if(!found && !rherr) found = song_resolve(p, songpath, sizeof songpath, &rherr);   /* still try the intact relative entry */
        if(rherr){ db_err = 1; break; }   /* a resolver DB error (not a clean miss) -> abandon, don't commit a partial */
        if(!found) continue;              /* genuine no-match / ambiguous -> skip this line */
        if(!pid){ pid = mdb_playlist_create(name); if(pid<=0){ db_err = 1; break; } }
        int aerr = 0;
        if(mdb_playlist_add_song_ex(pid, songpath, &aerr)) added++;
        if(aerr){ db_err = 1; break; }   /* a resolved track failed to insert (DB error) -> abandon the import */
    }
    int read_err = ferror(fp);
    fclose(fp);
    /* Commit ONLY a complete import (at least one track resolved+added, no read error, no DB error, and
     * the COMMIT itself succeeds). Anything else rolls the whole transaction back - no partial playlist
     * persists, so the next scan retries this file cleanly. */
    if(pid && added > 0 && !read_err && !db_err && sqlite3_exec(d, "COMMIT;", 0, 0, 0) == SQLITE_OK)
        return added;
    sqlite3_exec(d, "ROLLBACK;", 0, 0, 0);
    return 0;
}
/* Scan a directory (one level) for *.m3u / *.m3u8 and import each new one.
 * Returns the number of NEW playlists imported. */
static int mdb_import_m3u_dir_leased(const char *dir){
    DIR *d = opendir(dir); if(!d) return 0;
    int total = 0; struct dirent *e;
    while((e = readdir(d))){
        const char *n = e->d_name; int L = (int)strlen(n);
        int ism3u = (L>4 && !strcasecmp(n+L-4, ".m3u")) || (L>5 && !strcasecmp(n+L-5, ".m3u8"));
        if(!ism3u) continue;
        char path[600]; snprintf(path, sizeof path, "%s/%s", dir, n);
        if(import_m3u_file(path) > 0) total++;
    }
    closedir(d);
    return total;
}

/* Import .m3u/.m3u8 from the SD <root> AND any case-insensitively-named Music / Playlist(s)
 * subdirectory of it (exFAT on Linux is case-sensitive, so "music"/"MUSIC"/"Playlist" all
 * need matching). Returns total playlists imported. */
static int mdb_import_m3u_sd_leased(const char *root){
    int total = mdb_import_m3u_dir(root);          /* the root itself */
    DIR *d = opendir(root); if(!d) return total;
    struct dirent *e;
    while((e = readdir(d))){
        const char *nm = e->d_name;
        if(nm[0]=='.') continue;
        if(!strcasecmp(nm,"Music") || !strcasecmp(nm,"Playlist") || !strcasecmp(nm,"Playlists")){
            char path[600]; snprintf(path, sizeof path, "%s/%s", root, nm);
            total += mdb_import_m3u_dir(path);     /* opendir() fails harmlessly if it's a file */
        }
    }
    closedir(d);
    return total;
}

/* number of playlists - lets callers size a dynamic buffer to the real count (no fixed cap). */
int mdb_playlist_num(void){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st; int n=0;
    if(sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM PLAYLIST_INFO;", -1, &st, NULL) == SQLITE_OK){
        if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st,0);
        sqlite3_finalize(st);
    }
    return n;
}

int mdb_playlists(char names[][MDB_STR], long *ids, int cap){
    sqlite3 *d = db(); if(!d) return 0;
    sqlite3_stmt *st;
    if(sqlite3_prepare_v2(d, "SELECT NAME,ID FROM PLAYLIST_INFO WHERE NAME<>? ORDER BY ADD_TIME,ID;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, BOOK_SCOPE_NAME, -1, SQLITE_STATIC);   /* hide the reserved book-scope playlist */
    int n=0;
    while(n<cap && sqlite3_step(st) == SQLITE_ROW){
        snprintf(names[n], MDB_STR, "%s", colt(st,0)); trim(names[n]);
        ids[n] = (long)sqlite3_column_int64(st,1); n++;
    }
    sqlite3_finalize(st);
    return n;
}

int mdb_search(const char *q, const mdb_song_t **out, int cap){
    if(!q || !q[0]) return 0;
    int n = 0;
    for(int i=0;i<g_n && n<cap;i++){
        const mdb_song_t *s = &g_songs[i];
        if(strcasestr(s->title, q) || strcasestr(s->artist, q) || strcasestr(s->album, q))
            out[n++] = s;
    }
    return n;
}

int mdb_import_m3u_dir(const char *dir){
    if(!sd_io_begin()) return 0;
    int result = mdb_import_m3u_dir_leased(dir);
    sd_io_end();
    return result;
}

int mdb_import_m3u_sd(const char *root){
    if(!sd_io_begin()) return 0;
    int result = mdb_import_m3u_sd_leased(root);
    sd_io_end();
    return result;
}

int mdb_playlist_export(long pid, const char *name, char *outname, int cap){
    if(!sd_io_begin()) return 0;
    int result = mdb_playlist_export_leased(pid, name, outname, cap);
    sd_io_end();
    return result;
}
