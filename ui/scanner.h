/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef SCANNER_H
#define SCANNER_H
/* diskOS music scanner - walks the SD, reads ID3 tags, rebuilds song.db's SONG table.
 * All calls are safe from the LVGL/main thread; the scan itself runs on a worker thread. */
int  scanner_start(void);                     /* begin a rescan; 0 = started, -1 = busy/failed */
int  scanner_active(void);                    /* 1 while a scan is running */
void scanner_progress(int *done, int *total); /* files done so far; total is 0 until finished */
int  scanner_take_finished(void);             /* returns 1 exactly once after a scan finishes */
int  scanner_no_sd(void);                     /* 1 if the last scan aborted: no SD mounted (library kept) */
int  scanner_unsupported(void);               /* count of audio files present but not indexable (AAC/M4A/OGG/...) */

/* Audiobook chapters (Nero 'chpl' in an M4B/M4A). */
#define CHAP_TITLE 128
typedef struct { long start_ms; char title[CHAP_TITLE]; } chapter_t;
int  scan_read_chapters(const char *path, chapter_t *out, int max);   /* count read (0 if none); chpl only */
int  scan_read_narrator(const char *path, char *out, int cap);        /* 1 if the m4b names a narrator (composer/©wrt) */
#endif
