/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef ARTCACHE_H
#define ARTCACHE_H
/* Persistent decoded-art cache on the SD card, keyed by a file fingerprint
 * (path + size + mtime). Lets covers survive reboots and re-decode (ffmpeg) only
 * once per file, ever - so song-switching is instant once a song has been seen.
 * Keyed by file identity (not album/title), so it works regardless of metadata. */

/* Satisfy a request from cache: copies the cached cover/thumb/backdrop BMPs to the
 * given output paths. Returns 0 on a full hit (all three written), -1 on miss. */
int  artcache_get(const char *track, const char *cover_out, const char *thumb_out, const char *bg_out);

/* Store freshly-decoded BMPs into the cache for this track (atomic, best-effort).
 * No-op if the SD is nearly full. */
void artcache_put(const char *track, const char *cover, const char *thumb, const char *bg);

/* 1 if this track already has a complete cache entry (cover+thumb+backdrop). */
int  artcache_has(const char *track);

/* Copy JUST the cached 42px thumb to thumb_out (fast, no ffmpeg). Used by the Books
 * list, which only needs the small cover. Returns 0 on hit, -1 if the thumb isn't cached. */
int  artcache_get_thumb(const char *track, const char *thumb_out);
int  artcache_get_cover(const char *track, const char *cover_out);   /* just the 148px cover (Album Wall) */
int  artcache_cover_path(const char *track, char *out, int cap);     /* native path of the cached cover (no copy) */

/* Monotonic counter bumped each time a NEW cover is written to the cache (any decoder). The cover
 * flow polls this to re-bake placeholder albums the moment their art becomes available. */
unsigned artcache_gen(void);

/* SD-write ownership guard (implemented in main.c). artcache_put brackets its SD writes with
 * begin()/end() so they never race the player's Storage-mode export of /dev/mmcblk0. begin()
 * returns 0 (skip) when the card is host-owned or an export is in progress. */
int  sd_write_begin(void);
void sd_write_end(void);

#endif
