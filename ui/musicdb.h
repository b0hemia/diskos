/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef MUSICDB_H
#define MUSICDB_H

#define MDB_MAX_SONGS   1500
#define MDB_MAX_GROUPS  600
#define MDB_STR         160

/* Audiobook feature gate. The audiobook mode (Books view, chapters, resume, sleep timer,
 * chapter-bound ring) is OUT OF SCOPE for the FW2.40 parity release and is not yet device-
 * qualified (see analysis/BOOK_GAPHUNT_2026-09-17.md). Held at 0 for that release so .m4b
 * files are not indexed and all shared book behaviour reverts to plain music paths. Flip to
 * 1 to restore the full feature once it has had a device-qualification pass. */
#define DISKOS_AUDIOBOOKS 1

/* Reserved custom-playlist LIST_ID used as the isolated audiobook playback context. It MUST be the
 * lowest LIST_ID so the player's seq-0 resolution (ORDER BY LIST_ID LIMIT 1 OFFSET 0) targets it.
 * NEGATIVE so it sorts below every stock/user playlist (1+) and can never collide with a real one
 * (an existing playlist at id 0 would otherwise be wiped). Device-verified: -1000 resolves as seq 0. */
#define DISKOS_RSV_LISTID -1000
#define XSTR_(x) #x
#define XSTR(x) XSTR_(x)

typedef struct {
    int  id;
    char title[MDB_STR];
    char artist[MDB_STR];   /* raw ARTIST field (may be "A, B") */
    char artist_group[MDB_STR]; /* COALESCE(ALBUM_ARTIST,ARTIST): the player's artist key under "Album Artist" */
    char album[MDB_STR];    /* ALBUM, trimmed and cut at MDB_STR-1 bytes (display/drill name) */
    unsigned album_h;       /* FNV-1a of the FULL stored ALBUM value (untrimmed, uncut): the Albums list groups on it, like stock's GROUP BY ALBUM */
    char genre[MDB_STR];    /* GENRE field - used here as user mood/tag */
    int  dur_ms;
    int  track;             /* SONG.TRACK (0 = none) - shown in album lists with Track Numbers on */
    int  love_id;           /* MY_LOVE.ID - set only by mdb_favorites (the V2.57 type-6 start index), else unset */
    /* The stock sort codes stored on the row (mdb_load only; 0 when the schema has none): the Artists/Albums/Genres lists
     * order by these exactly as the player's own queries do, so non-Latin names land where stock puts them. */
    int  code_artist, code_album, code_genre, code_group;   /* ARTIST_CODE / ALBUM_CODE / GENRE_CODE / album-artist-or-artist code */
} mdb_song_t;

/* Load the whole library once (one sqlite3 call). Safe to call repeatedly;
 * reloads. Returns song count. */
int mdb_load(void);
int mdb_load_failed(void);   /* 1 if the last mdb_load hit a DB error (BUSY/IOERR/OOM), not just empty */

int               mdb_song_count(void);
const mdb_song_t *mdb_song(int i);
/* On-demand absolute file path for a song ID (queries the DB). 1 on success. */
int               mdb_song_path(int id, char *out, int cap);
int               mdb_song_id_by_path(const char *path);   /* SONG.ID for an absolute path; 0 if not indexed */
/* On-demand ALBUM for a song ID. 1 if a non-empty album was found. */
int               mdb_song_album(int id, char *out, int cap);
int               mdb_song_meta_by_path(const char *path, char *album, int acap, char *artist, int arcap);
int               mdb_tag_title_by_path(const char *path, char *out, int cap);   /* tag TITLE of a plain-file row; 1 = found. Safe off the UI thread */
/* 1-based position of a song within the player's rebuilt list for a given
 * list_type (0=all,2=artist,3=album,10=genre) + name, matching mq_player's
 * exact ORDER BY so a tap lands on the exact track. Returns >=1. */
int               mdb_play_pos(int id, int list_type, const char *name);
/* Current/last playing track from MEMORY_PLAY (for startup state-sync).
 * Fills *out (title/artist/album/dur_ms/id), *pos_ms, *is_playing. 1 if found. */
int               mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing);

/* Distinct albums (with a representative artist + track count). */
int  mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap);
/* ---- Album identity + play plans (Artist -> Albums, stage 1) ----------------------------------------------------
 * An album is (raw COALESCE(ALBUM_ARTIST,ARTIST), raw ALBUM): exact values, no trim/case-folding, so two artists'
 * same-named albums never merge and the key matches the player's own SQL. */
typedef struct {
    char owner[MDB_STR];   /* raw COALESCE(ALBUM_ARTIST,ARTIST) */
    int  owner_null;       /* 1 when both ALBUM_ARTIST and ARTIST are NULL */
    char album[MDB_STR];   /* raw ALBUM (never NULL/empty for a keyed album) */
} mdb_album_key_t;
typedef struct {
    int  list_type;        /* 3 = stock album queue, 7 = stock artist+album queue, 5 = exact reserved-slot queue */
    char name[2*MDB_STR + 40];   /* the 0100 payload for types 3/7 */
    int *ids; int count;   /* the queue, in the exact order the player will play it (malloc'd: mdb_plan_free) */
} mdb_plan_t;
int  mdb_album_keys(mdb_album_key_t *keys, int *counts, int *rep_ids, int cap);   /* all keyed albums, by name */
int  mdb_album_key_song_ids(const mdb_album_key_t *k, int *ids, int cap);         /* the album's songs, player album order */
int  mdb_album_plan(const mdb_album_key_t *k, mdb_plan_t *plan);                  /* 1 = plan built (caller frees) */
int  mdb_artist_plan(const char *key, mdb_plan_t *plan);                           /* Album-Artist view key; 1 = built */
/* Scoped albums (Artist -> Albums -> Songs, Genre -> Albums -> Songs): kind = MDB_SCOPE_*, key = an Artists-view key
 * (grouped per mdb_artist_mode) or a genre name. */
#define MDB_SCOPE_ARTIST 1
#define MDB_SCOPE_GENRE  2
int  mdb_scope_albums(int kind, const char *key, char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap);   /* distinct non-empty albums in scope, by name; returns the FULL count (rows written up to cap; artists/counts optional) */
int  mdb_scope_album_songs(int kind, const char *key, const char *album, const mdb_song_t **out, int cap);  /* that album's songs in scope, player album order; returns the rows written (<= cap; out NULL = full count) */
int  mdb_scope_album_plan(int kind, const char *key, const char *album, mdb_plan_t *plan);   /* type 7 (artist) / 8 (genre) when exact, else 5; 1 = built (caller frees) */
int  mdb_plan_materialize(mdb_plan_t *plan);   /* type 5: write the reserved slot, adopt the player's real order */
int  mdb_plan_pos(const mdb_plan_t *plan, int song_id);                           /* 1-based position, 0 = not in it */
void mdb_plan_free(mdb_plan_t *plan);
int  mdb_subtrack_count(const char *path, int iso_only);   /* CUE/ISO rows under one PATH (iso_only: IS_ISO rows only) */
int  mdb_subtrack_plan(const char *const *paths, int npaths, mdb_plan_t *plan);   /* type-5 plan of their tracks, TRACK order; 1 = built (caller frees) */

/* Up Next: the player's live queue from the current song on (read-only; the stock player owns it).
 * Sequential/repeat modes play LIST_SONG_0 in ID order; shuffle plays LIST_SONG_3 in ID order, whose POS_ID is
 * the LIST_SONG_0.ID it plays. The current song is found by the a2 pos_id (a LIST_SONG_0.ID). Every row carries
 * its 1-based position in LIST_SONG_0 ID order - what a type-0 jump takes, in shuffle too (device-verified). */
typedef struct {
    int  base_id;            /* LIST_SONG_0.ID */
    int  ord;                /* 1-based position in LIST_SONG_0 ID order (the type-0 jump index + 1) */
    int  dur_ms;
    char title[MDB_STR];     /* TITLE, else the file NAME */
    char artist[MDB_STR];
    char path[256];
} mdb_qrow_t;
#define MDB_UPNEXT_EMPTY    0      /* the queue is empty */
#define MDB_UPNEXT_ERROR    (-1)   /* the queue can't be read (DB error) */
#define MDB_UPNEXT_NOTFOUND (-2)   /* the current song isn't in the queue, or the shuffle list isn't an exact permutation
                                    * of it: the player is rebuilding the queue (transient) */
#define MDB_UPNEXT_BUSY     (-3)   /* the player holds the DB lock right now (transient) */
/* Fill out[0] = the current song, out[1..] = what plays after it, at most cap rows; *more = rows not shown.
 * cur_pos_id 0 (unknown, e.g. after a UI restart) falls back to cur_path, only if exactly one queue row has it.
 * Returns the number of rows (>0) or one of the MDB_UPNEXT_* codes above. */
int mdb_upnext(int shuffle, int cur_pos_id, const char *cur_path, mdb_qrow_t *out, int cap, int *more);
int mdb_queue_row_valid(int base_id, int ord, const char *path);   /* re-check a jump target right before sending: 1/0/-1 */
int mdb_sysconfig_play_mode(void);                                 /* the player's saved PLAY_MODE 0..4, -1 unreadable */
int  ui_play_plan(mdb_plan_t *plan, int song_id);   /* main.c: play song_id through the plan (1 = sent) */
int  mdb_artist_class_type(void);                          /* player Artist grouping: 0 ARTIST, 1 album artist, -1 unknown */
void mdb_set_artist_mode(int album_artist);                 /* Artists view: 0 = by ARTIST (collabs split), 1 = by album artist */
int  mdb_artist_mode(void);
int  mdb_album_rep_ids(int *out, int cap);                    /* per album (mdb_albums order): its first song's ID */
int  mdb_album_count(void);                                  /* distinct album count (for dynamic buffer sizing) */
/* Distinct artists, with comma-separated artists split so a collab shows under
 * each individual name. */
int  mdb_artists(char names[][MDB_STR], int cap);

/* Audiobooks (v1: single-file .m4b). Books are kept out of the music Songs/Albums/Artists views. */
typedef struct {
    int  id;                    /* SONG.ID of the book file */
    char title[MDB_STR];
    char author[MDB_STR];
    char path[512];             /* the .m4b file (also the book key) */
    long position_ms;           /* saved resume position (0 if none) */
    int  completed;
} book_t;
int  mdb_total_song_count(void);                            /* full SONG count incl. audiobooks (buffer sizing / empty check) */
int  mdb_is_book_path(const char *path);                    /* 1 if this path is an audiobook (.m4b) */
int  mdb_song_genre(const char *path, int track, char *out, int cap);   /* GENRE of the song row for `path` (sub-track `track` > 0, else the first row); 1 = found. Safe off the UI thread */
int  mdb_books(book_t *out, int max);                       /* list audiobooks, continue-listening first */
int  mdb_book_progress(const char *key, char *member_out, int member_cap, long *position_ms, int *completed_out, long *updated_out); /* 1 if bookmark */
int  mdb_book_save(const char *key, const char *member_path, long position_ms, int completed);  /* 1=written, 0=failed (caller should retry, not advance its throttle) */
int  mdb_reserved_slot_set(const char *path);  /* set the reserved list_type-5 slot (seq 0) to this single path for isolated audiobook playback; 1=ok */
int  mdb_reserved_slot_set_playlist(long pid);  /* set the reserved slot (seq 0) to a user playlist's members for isolated playlist playback; returns member count, 0=fail/empty */
int  mdb_reserved_slot_player_pos(long pid, int row);   /* player's slot position of display row `row` (1-based); 0 = unknown */
int  mdb_migrate_books(void);                   /* move any .m4b out of music stores into BOOKS; 1=clean/done, 0=failed (retry) */
int  mdb_artist_count(void);   /* distinct tokenized artist count (for sizing the caller's buffer) */
/* Distinct genres/tags (with track count). */
int  mdb_genres(char names[][MDB_STR], int *counts, int cap);

/* Fill out[] with songs in an album / by an artist token / in a genre. Returns count. */
int  mdb_album_songs(const char *album, const mdb_song_t **out, int cap);
int  mdb_album_track_ids(const char *album, int *ids, int cap);   /* unsorted IDs, no DB sort (cover lookup) */
int  mdb_artist_songs(const char *artist, const mdb_song_t **out, int cap);
int  mdb_genre_songs(const char *genre, const mdb_song_t **out, int cap);

/* Case-insensitive substring match on title/artist/album. Returns count. */
int  mdb_search(const char *q, const mdb_song_t **out, int cap);

/* Favourites (MY_LOVE) and playlists (PLAY_LIST). Both may be empty. */
int  mdb_favorites(mdb_song_t *out, int cap, int stock_order);   /* stock_order: the player's type-6 queue order */
int  mdb_favorite_count(void);                                   /* MY_LOVE music rows (may exceed SONG); -1 = error */
int  mdb_love_path(int love_id, char *out, int cap);              /* MY_LOVE.PATH for a MY_LOVE.ID; 1 = found */
void mdb_record_play(const char *path);            /* bump PLAY_STATS on a new track */
int  mdb_mostplayed(mdb_song_t *out, int cap);     /* songs by play count desc */
int  mdb_recent(mdb_song_t *out, int cap);         /* songs by last-played desc */
int  mdb_unfavorite(int love_id);                   /* remove by MY_LOVE.ID */
int  mdb_playlist_sync_registry(void);   /* reconcile PLAYLIST_INFO with stock's CUSTOM_PLAYLIST_INDEX; 2 = synced, 1 = no index, 0 = failed */
int  mdb_playlist_num(void);   /* count of playlists (to size a dynamic buffer, no fixed cap) */
int  mdb_playlists(char names[][MDB_STR], long *ids, int cap);
/* custom playlists: create returns id (>0) or 0; add copies a SONG row by path */
long mdb_playlist_create(const char *name);
int  mdb_playlist_add_song(long pid, const char *path);
int  mdb_playlist_add_song_ex(long pid, const char *path, int *hard_err);   /* *hard_err=1 only on a real DB failure */
int  mdb_playlist_remove_at(long pid, int position);   /* remove the song at 1-based display ordinal; 1 if deleted */
int  mdb_song_subtrack(const char *path, int pos_id, int *track, int *is_cue, int *is_iso);   /* 1 = the queue row pos_id is a CUE/ISO sub-track of path (display use) */
int  mdb_song_subtrack_verified(const char *path, int pos_id, const char *title, int *track, int *is_cue, int *is_iso);   /* same, and the row must also match the current title (for adds) */   /* 1 = the queue row pos_id is a CUE/ISO sub-track of path */
int  mdb_playlist_add_subtrack(long pid, const char *path, int track, int *hard_err);   /* add exactly that CUE/ISO track; 1 if added */
int  mdb_playlist_add_ids(long pid, const int *ids, int n);   /* add exactly these SONG.IDs, skipping held ones; count added, -1 on a failed write */
int  mdb_song_multi(const char *path);   /* 1 = more than one SONG row has this path (CUE/ISO) */
int  mdb_playlist_has_subtrack(long pid, const char *path, int track);
int  mdb_playlist_add_group(long pid, const char *col, const char *val);   /* bulk-add an ALBUM/ARTIST/GENRE; returns count added */
int  mdb_playlist_has_song(long pid, const char *path);
int  mdb_playlist_rename(long pid, const char *name);
int  mdb_playlist_delete(long pid);
long mdb_book_scope_set(const char *path);   /* point the reserved single-book playlist at path; returns its id (0 fail) */
int  mdb_playlist_export(long pid, const char *name, char *outname, int cap);   /* write /tmp/sdcard/<name>.m3u; 1=ok */
int  mdb_playlist_songs(long pid, mdb_song_t *out, int cap);
int  mdb_playlist_count(long pid);
/* scan a dir for *.m3u / *.m3u8 and import each new one as a playlist; returns # imported */
int  mdb_import_m3u_dir(const char *dir);
/* import from every folder under the SD root (bounded depth/count, no hidden dirs or links); returns # imported */
int  mdb_import_m3u_sd(const char *root);

/* Split a raw ARTIST string into individual names (comma / ; / "feat."). */
int  mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap);

/* persistent per-song accent cache (0xRRGGBB; 0 = not computed). Survives reboot. */
int  mdb_song_accent(const char *path);
void mdb_set_song_accent(const char *path, int rgb, const char *cover_key);   /* worker threads only */
int  mdb_song_accent_fresh(const char *path, const char *cover_key);          /* worker threads only */
/* prewarm iterator: next SONG row with ID > after_id. Returns 1 + fills id/path. */
int  mdb_prewarm_next(int after_id, int *id, char *path, int cap);

/* write a custom EQ curve to the player's PEQ table (STYLE_PRESET slot), then select it
 * with 0689 to apply. params_json = stock format (10 bands, gain/qValue as strings). */
int  mdb_set_peq(int style_preset, double master_gain, const char *params_json);
/* read a PEQ slot's stored curve: *master_out (dB) + up to 10 band gains (int dB, band order).
 * Returns 1 if the slot row exists, 0 if not. gains_out must hold >=10 ints. */
int  mdb_get_peq(int style_preset, int *master_out, int *gains_out);   /* 1=found, 0=no slot (flat is real), -1=read FAILED */
int  mdb_peq_is_graphic(int style_preset);   /* 1=graphic-editable (or empty), 0=parametric (don't overwrite), -1=read failed */

int  mdb_sysconfig_folder_jump(void);
int  mdb_sysconfig_language(void);          /* SYSCONFIG.LANGUAGE (0..), -1 unreadable */        /* SYSCONFIG.FOLDER_JUMP (0687): 0/1, -1 unreadable */
int  mdb_sysconfig_auto_time(void);          /* SYSCONFIG.AUTO_TIME: 0/1, -1 unreadable (player reads it at start) */
int  mdb_sysconfig_set_auto_time(int on);    /* 0 = written + read back, -1 = not */
#endif
