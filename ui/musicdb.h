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
    char album[MDB_STR];
    char genre[MDB_STR];    /* GENRE field - used here as user mood/tag */
    int  dur_ms;
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
/* 1-based position of a song within the player's rebuilt list for a given
 * list_type (0=all,2=artist,3=album,10=genre) + name, matching mq_player's
 * exact ORDER BY so a tap lands on the exact track. Returns >=1. */
int               mdb_play_pos(int id, int list_type, const char *name);
/* Current/last playing track from MEMORY_PLAY (for startup state-sync).
 * Fills *out (title/artist/album/dur_ms/id), *pos_ms, *is_playing. 1 if found. */
int               mdb_current_play(mdb_song_t *out, int *pos_ms, int *is_playing);

/* Distinct albums (with a representative artist + track count). */
int  mdb_albums(char names[][MDB_STR], char artists[][MDB_STR], int *counts, int cap);
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
int  mdb_books(book_t *out, int max);                       /* list audiobooks, continue-listening first */
int  mdb_book_progress(const char *key, char *member_out, int member_cap, long *position_ms, int *completed_out, long *updated_out); /* 1 if bookmark */
int  mdb_book_save(const char *key, const char *member_path, long position_ms, int completed);  /* 1=written, 0=failed (caller should retry, not advance its throttle) */
int  mdb_reserved_slot_set(const char *path);  /* set the reserved list_type-5 slot (seq 0) to this single path for isolated audiobook playback; 1=ok */
int  mdb_reserved_slot_set_playlist(long pid);  /* set the reserved slot (seq 0) to a user playlist's members for isolated playlist playback; returns member count, 0=fail/empty */
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
int  mdb_favorites(mdb_song_t *out, int cap);
void mdb_record_play(const char *path);            /* bump PLAY_STATS on a new track */
int  mdb_mostplayed(mdb_song_t *out, int cap);     /* songs by play count desc */
int  mdb_recent(mdb_song_t *out, int cap);         /* songs by last-played desc */
int  mdb_unfavorite(int id);
int  mdb_playlist_num(void);   /* count of playlists (to size a dynamic buffer, no fixed cap) */
int  mdb_playlists(char names[][MDB_STR], long *ids, int cap);
/* custom playlists: create returns id (>0) or 0; add copies a SONG row by path */
long mdb_playlist_create(const char *name);
int  mdb_playlist_add_song(long pid, const char *path);
int  mdb_playlist_add_song_ex(long pid, const char *path, int *hard_err);   /* *hard_err=1 only on a real DB failure */
int  mdb_playlist_remove_at(long pid, int position);   /* remove the song at 1-based display ordinal; 1 if deleted */
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
/* import from the SD root + any case-insensitive Music/Playlist(s) subdir; returns # imported */
int  mdb_import_m3u_sd(const char *root);

/* Split a raw ARTIST string into individual names (comma / ; / "feat."). */
int  mdb_split_artists(const char *raw, char toks[][MDB_STR], int cap);

/* persistent per-song accent cache (0xRRGGBB; 0 = not computed). Survives reboot. */
int  mdb_song_accent(const char *path);
void mdb_set_song_accent(const char *path, int rgb);
/* prewarm iterator: next SONG row with ID > after_id. Returns 1 + fills id/path. */
int  mdb_prewarm_next(int after_id, int *id, char *path, int cap);

/* write a custom EQ curve to the player's PEQ table (STYLE_PRESET slot), then select it
 * with 0689 to apply. params_json = stock format (10 bands, gain/qValue as strings). */
int  mdb_set_peq(int style_preset, double master_gain, const char *params_json);
/* read a PEQ slot's stored curve: *master_out (dB) + up to 10 band gains (int dB, band order).
 * Returns 1 if the slot row exists, 0 if not. gains_out must hold >=10 ints. */
int  mdb_get_peq(int style_preset, int *master_out, int *gains_out);   /* 1=found, 0=no slot (flat is real), -1=read FAILED */
int  mdb_peq_is_graphic(int style_preset);   /* 1=graphic-editable (or empty), 0=parametric (don't overwrite), -1=read failed */

#endif
