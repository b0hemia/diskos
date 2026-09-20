/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef CONFIG_H
#define CONFIG_H

/* Tiny key=value config persisted to /usr/data/diskos.conf.
 * Load once at startup; setters rewrite the whole file (it's small). */
void        cfg_load(void);
int         cfg_get_int(const char *key, int def);
int         cfg_set_int(const char *key, int v);   /* 0 ok, -1 save failed */
int         cfg_set_int_deferred(const char *key, int v);  /* in-memory only; follow with cfg_flush() */
int         cfg_flush(void);                        /* one atomic rewrite/fsync of pending sets */
void        cfg_begin(void);                        /* start an atomic multi-key batch (snapshots the store) */
int         cfg_commit(void);                       /* persist the batch, or roll it ALL back on any set failure; 0 ok, -1 failed */
const char *cfg_get_str(const char *key, const char *def);
int         cfg_set_str(const char *key, const char *v);   /* 0 ok, -1 save failed */
int         cfg_set_str_deferred(const char *key, const char *v);  /* in-memory only; follow with cfg_flush() */
/* returns 1 (and clears) if any cfg write failed since the last call - so the UI
 * can surface a "couldn't save" toast instead of losing the change silently. */
int         cfg_take_save_error(void);

#endif
