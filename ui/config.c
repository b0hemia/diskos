/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* Paths are overridable at compile time (-D...) so the off-device host test can point them at a
 * temp file; the device build uses these defaults. */
#ifndef CFG_PATH
#define CFG_PATH     "/usr/data/diskos.conf"
#endif
#ifndef CFG_DIR
#define CFG_DIR      "/usr/data"
#endif
#ifndef LEGACY_CONF
#define LEGACY_CONF  "/usr/data/ipodos.conf"    /* pre-rename config; migrated on first load */
#endif
#ifndef LEGACY_SWIPE
#define LEGACY_SWIPE "/usr/data/swipe_thresh"   /* pre-config single-value file */
#endif
/* Headroom for every key the app can persist: 10 EQ slots x 11 keys (eqN_b0..b9 + eqN_master)
 * = 110, plus the legacy flat eq_* keys (migrated once), plus ~30 general settings. 64 was far
 * too small - once g_cfg filled, put() silently dropped new keys AND rewrite() flushed only the
 * first 64 entries back to diskos.conf, permanently deleting the rest. */
#define CFG_MAX      256
#define KLEN         28
#define VLEN         48

typedef struct { char k[KLEN]; char v[VLEN]; } cfg_entry_t;
static cfg_entry_t g_cfg[CFG_MAX];
static int g_n = 0;
static int g_loaded = 0;

static int g_save_err = 0;   /* sticky until cfg_take_save_error() reads it */
static int g_dirty = 0;      /* the in-memory set differs from the on-disk file: a save is pending
                              * (a deferred batch) or FAILED. Guards the unchanged-value shortcut so a
                              * failed persist is retried instead of masked by "value already equals v". */

/* Atomic multi-key batch (e.g. Last.fm credentials): cfg_begin() snapshots the whole store, deferred
 * sets mutate it, cfg_commit() rewrites ONLY if every set succeeded, else it rolls the store back to
 * the snapshot and reports failure - so a partial credentials write is impossible even if the store
 * fills or a value is rejected mid-batch. */
static cfg_entry_t g_snap[CFG_MAX];
static int g_snap_n = 0, g_snap_dirty = 0, g_txn = 0, g_txn_err = 0;
static int g_load_failed = 0;   /* cfg_load hit a transient open/read error (NOT genuine first-run):
                                 * the in-memory set is empty/partial, so rewrite() must REFUSE to run -
                                 * else a single boot-time read glitch would overwrite a good diskos.conf
                                 * (all EQ/wifi/lastfm/UI settings) with an empty one. */

static cfg_entry_t *find(const char *key){
    for(int i=0;i<g_n;i++) if(!strcmp(g_cfg[i].k,key)) return &g_cfg[i];
    return NULL;
}

/* Atomic, durable rewrite: write a temp file, fsync it, rename over the real file
 * (atomic on POSIX), then fsync the directory. A power cut can no longer leave a
 * truncated/half-written diskos.conf - you keep either the old file or the new one.
 * Returns 0 on success, -1 on any failure (and sets the sticky save-error flag). */
static int rewrite(void){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* never persist an unloaded/partial config over a good file */
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", CFG_PATH);
    FILE *f = fopen(tmp, "w");
    if(!f){ g_save_err = 1; return -1; }
    int ok = 1;
    for(int i=0;i<g_n;i++)
        if(fprintf(f, "%s=%s\n", g_cfg[i].k, g_cfg[i].v) < 0){ ok = 0; break; }
    if(ok && (fflush(f) != 0 || fsync(fileno(f)) != 0)) ok = 0;
    if(fclose(f) != 0) ok = 0;
    if(!ok || rename(tmp, CFG_PATH) != 0){ unlink(tmp); g_save_err = 1; return -1; }
    /* persist the rename itself; a dir open/fsync/close failure means the new file may not survive a
     * power cut, so surface it (rewrite()'s contract is durable success), though the content is written. */
    int dfd = open(CFG_DIR, O_RDONLY | O_DIRECTORY);
    if(dfd < 0){ g_save_err = 1; return -1; }
    int dr = fsync(dfd);
    if(close(dfd) != 0 || dr != 0){ g_save_err = 1; return -1; }
    g_dirty = 0;   /* the on-disk file now equals the in-memory set */
    return 0;
}

int cfg_take_save_error(void){ int e = g_save_err; g_save_err = 0; return e; }

/* Returns 0 on success, -1 if the key/value is too long or the store is full. Rejects WITHOUT
 * mutating, so a bad set never half-writes an entry (was: silently dropped/truncated). Marks g_dirty
 * only when it actually changes a stored value. */
static int put(const char *key, const char *val){
    if(strlen(key) >= KLEN || strlen(val) >= VLEN){ if(g_txn) g_txn_err = 1; return -1; }  /* too long -> reject, no mutation */
    cfg_entry_t *e = find(key);
    int isnew = 0;
    if(!e){
        if(g_n >= CFG_MAX){ if(g_txn) g_txn_err = 1; return -1; }   /* store full -> reject */
        e = &g_cfg[g_n++];
        e->k[0] = 0; e->v[0] = 0;
        snprintf(e->k, KLEN, "%s", key);
        isnew = 1;
    }
    /* a NEW key is always a change - even value "" - so it must mark dirty (else adding an empty key
     * would stay "clean" and never persist). */
    if(isnew || strcmp(e->v, val) != 0){ snprintf(e->v, VLEN, "%s", val); g_dirty = 1; }
    return 0;
}

/* Parse "key=value\n" lines from f into the store. Returns 0 clean, -1 if the load is INCOMPLETE (an I/O
 * error, or the store filled up) so the caller can block rewrites and not persist a partial config over a
 * good file. An OVERLONG physical line (longer than the buffer - never produced by our KLEN/VLEN-bounded
 * writer) is DRAINED and skipped, so its tail can't be misparsed as a separate (corrupt) setting. */
static int cfg_parse_stream(FILE *f){
    char line[KLEN+VLEN+4];
    int failed = 0;
    while(fgets(line, sizeof(line), f)){
        size_t ll = strlen(line);
        int has_nl = (ll && line[ll-1]=='\n');
        if(!has_nl && ll == sizeof(line)-1){          /* buffer full, no newline -> overlong line: drain + skip */
            int c; while((c=fgetc(f))!=EOF && c!='\n'){}
            continue;
        }
        if(has_nl) line[ll-1] = 0;
        char *eq = strchr(line, '='); if(!eq) continue;
        *eq = 0;
        if(line[0] && put(line, eq+1) < 0) failed = 1; /* store capacity full -> incomplete load */
    }
    if(ferror(f)) failed = 1;                          /* mid-read I/O error -> incomplete load */
    return failed ? -1 : 0;
}

void cfg_load(void){
    if(g_loaded) return;
    g_loaded = 1;
    FILE *f = fopen(CFG_PATH, "r");
    if(f){
        if(cfg_parse_stream(f) < 0) g_load_failed = 1; /* incomplete (I/O error or store full) -> block rewrites */
        fclose(f);
        g_dirty = 0;   /* the in-memory set now equals the on-disk file (loading isn't a pending save) */
        return;
    }
    if(errno != ENOENT){   /* open failed for a TRANSIENT reason (EIO/EACCES/EMFILE), not "file absent":
                            * treat as unavailable, not first-run - leave the on-disk config untouched. */
        g_load_failed = 1;
        return;
    }
    /* ENOENT for diskos.conf: migrate settings from the pre-rename ipodos.conf if it
     * exists, so nobody loses their config across the rename. We read it, then rewrite()
     * persists to the new diskos.conf (the old file is left in place, harmless). */
    FILE *lc = fopen(LEGACY_CONF, "r");
    if(lc){
        int bad = (cfg_parse_stream(lc) < 0);
        fclose(lc);
        if(!bad) rewrite();   /* write the migrated set to diskos.conf */
        else g_load_failed = 1;
        return;
    }
    if(errno != ENOENT){   /* legacy open failed for a TRANSIENT reason (EIO/EMFILE/EACCES), not "absent":
                            * don't treat as first-run + create a fresh defaults-only config that would
                            * permanently bypass migration. Mark failed so a later boot retries. */
        g_load_failed = 1;
        return;
    }
    /* genuine first run (ENOENT): import legacy single-value swipe_thresh file if present */
    FILE *lf = fopen(LEGACY_SWIPE, "r");
    if(lf){
        int v=0;
        if(fscanf(lf,"%d",&v)==1 && v>=20 && v<=200){
            char b[16]; snprintf(b,sizeof(b),"%d",v); put("swipe_thresh", b);
        }
        fclose(lf);
        rewrite();
    }
}

int cfg_get_int(const char *key, int def){
    cfg_entry_t *e = find(key);
    return e ? atoi(e->v) : def;
}

int cfg_set_int(const char *key, int v){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* load failed -> fully read-only, no memory mutation */
    cfg_entry_t *e = find(key);
    if(e && atoi(e->v) == v && !g_dirty) return 0;   /* unchanged AND fully persisted -> nothing to do.
                                                      * Must NOT skip when g_dirty: a prior save failed, or
                                                      * a deferred batch is pending, so re-setting the same
                                                      * value still has to flush it to disk (retries the save). */
    char b[16]; snprintf(b,sizeof(b),"%d",v);
    if(put(key, b) < 0){ g_save_err = 1; return -1; }   /* too long / store full -> report, don't persist a bad set */
    return rewrite();
}
/* Batched set: update the in-memory value only, NO flash write. Follow a run of these
 * with a single cfg_flush() so bulk updates (e.g. EQ "Flat" = 11 bands) do ONE atomic
 * rewrite/fsync instead of one per key. */
int cfg_set_int_deferred(const char *key, int v){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* load failed -> read-only */
    cfg_entry_t *e = find(key);
    if(e && atoi(e->v) == v) return 0;
    char b[16]; snprintf(b,sizeof(b),"%d",v);
    if(put(key, b) < 0){ g_save_err = 1; return -1; }
    return 0;
}
int cfg_flush(void){ return rewrite(); }

void cfg_begin(void){
    if(g_load_failed){ g_save_err = 1; return; }
    if(g_n > 0) memcpy(g_snap, g_cfg, (size_t)g_n * sizeof g_cfg[0]);
    g_snap_n = g_n; g_snap_dirty = g_dirty; g_txn = 1; g_txn_err = 0;
}
int cfg_commit(void){
    if(!g_txn) return cfg_flush();          /* no open batch -> behave like a plain flush */
    g_txn = 0;
    if(g_txn_err){                          /* a set failed mid-batch -> roll the WHOLE store back to the snapshot */
        if(g_snap_n > 0) memcpy(g_cfg, g_snap, (size_t)g_snap_n * sizeof g_cfg[0]);
        g_n = g_snap_n; g_dirty = g_snap_dirty; g_save_err = 1;
        return -1;
    }
    return rewrite();
}

const char *cfg_get_str(const char *key, const char *def){
    cfg_entry_t *e = find(key);
    return e ? e->v : def;
}

int cfg_set_str(const char *key, const char *v){
    if(g_load_failed){ g_save_err = 1; return -1; }   /* load failed -> read-only */
    if(put(key, v) < 0){ g_save_err = 1; return -1; }
    return rewrite();
}
/* in-memory only; follow a run with cfg_flush() so multi-key writes (e.g. Last.fm api_key + secret
 * + session key) persist in ONE atomic rewrite - a partial credentials write on a mid-run failure
 * is then impossible. */
int cfg_set_str_deferred(const char *key, const char *v){
    if(g_load_failed){ g_save_err = 1; return -1; }
    if(put(key, v) < 0){ g_save_err = 1; return -1; }
    return 0;
}
