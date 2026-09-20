/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef ART_H
#define ART_H
/* Decode the track's embedded cover and write three BMPs in one ffmpeg pass:
 * cover_bmp (148px), thumb_bmp (42px Home thumb), backdrop_bmp (360px blurred).
 * Returns 0 on success, non-zero if there's no usable cover. */
int art_make_all(const char *track, const char *cover_bmp,
                 const char *thumb_bmp, const char *backdrop_bmp);
/* Same, but cancellable=1 registers the child so art_cancel() can kill it. */
int art_make_all_ex(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable);
/* As above, but the caller supplies the cancel token (captured atomically with its request identity), so a
 * skip landing between request-validation and here still supersedes this decode. */
int art_make_all_ex_gen(const char *track, const char *cover_bmp,
                    const char *thumb_bmp, const char *backdrop_bmp, int cancellable, unsigned gen0);
/* Current cancel generation, for capturing a token to pass to art_make_all_ex_gen. */
unsigned art_cancel_gen(void);
/* Kill the in-flight cancellable (live) decode, if any. */
void art_cancel(void);
#endif
