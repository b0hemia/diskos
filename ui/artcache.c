/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "artcache.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <stdatomic.h>

/* cache lives on the (large) SD card so it persists across reboots */
#define CACHE_ROOT "/tmp/sdcard/.diskos/artcache"
#define MIN_FREE_BYTES (1024ULL*1024*1024)   /* stop caching if < 1GB free */
/* Bump when the art RECIPE changes (cover/thumb/backdrop generation in art.c) so old
 * cached files auto-invalidate: it's mixed into the fingerprint -> all keys change ->
 * miss -> re-decode. v2 = axis-wash backdrop (was gblur). v3 = 2x2 smooth colour gradient.
 * v4 = blurred cover art (gblur sigma 28). v5 = sharper blur (sigma 14). v6 = sigma 19. */
#define ART_RECIPE_VER 6

/* fingerprint = FNV-1a(path + size + mtime) -> 16 hex chars. Changes if the file
 * is replaced/edited, so a stale cache entry simply misses (and is orphaned). */
static int fingerprint(const char *track, char *out, int cap){
    struct stat stt;
    if(!track || stat(track, &stt) != 0) return -1;
    uint64_t h = 1469598103934665603ULL;
    h ^= (unsigned)ART_RECIPE_VER; h *= 1099511628211ULL;   /* recipe salt -> bump invalidates old cache */
    for(const char *p=track; *p; p++){ h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    uint64_t sz=(uint64_t)stt.st_size, mt=(uint64_t)stt.st_mtime;
    for(int i=0;i<8;i++){ h ^= (sz>>(i*8))&0xff; h *= 1099511628211ULL; }
    for(int i=0;i<8;i++){ h ^= (mt>>(i*8))&0xff; h *= 1099511628211ULL; }
    snprintf(out, cap, "%016llx", (unsigned long long)h);
    return 0;
}
static int file_ok(const char *p){ struct stat s; return stat(p,&s)==0 && s.st_size>100; }
static void cache_paths(const char *fp, char *cv, char *th, char *bg, int cap){
    snprintf(cv,cap,"%s/%s/c.bmp",CACHE_ROOT,fp);
    snprintf(th,cap,"%s/%s/t.bmp",CACHE_ROOT,fp);
    snprintf(bg,cap,"%s/%s/b.bmp",CACHE_ROOT,fp);
}
static int copy_file(const char *src, const char *dst){
    FILE *in=fopen(src,"rb"); if(!in) return -1;
    /* unique tmp per call: a live decode + a prewarm decode can copy the SAME dst
     * concurrently; a shared "<dst>.tmp" would let them interleave and expose a
     * partial file. Unique tmp + the atomic rename below means dst is only ever
     * replaced by a complete file. */
    static _Atomic unsigned g_tmp_ctr = 0;
    unsigned seq = atomic_fetch_add(&g_tmp_ctr, 1u);
    char tmp[600]; snprintf(tmp,sizeof tmp,"%s.tmp%u",dst,seq);
    FILE *out=fopen(tmp,"wb"); if(!out){ fclose(in); return -1; }
    char buf[16384]; size_t n; int ok=1;
    while((n=fread(buf,1,sizeof buf,in))>0){ if(fwrite(buf,1,n,out)!=n){ ok=0; break; } }
    if(ferror(in)) ok=0;                       /* read error mid-copy -> don't publish a truncated file */
    fclose(in);
    if(fflush(out)!=0) ok=0;
    fclose(out);
    if(!ok){ unlink(tmp); return -1; }
    if(rename(tmp,dst)!=0){ unlink(tmp); return -1; }
    return 0;
}
static int enough_free(void){
    struct statvfs v;
    if(statvfs("/tmp/sdcard", &v) != 0) return 0;     /* unknown -> don't cache */
    return ((uint64_t)v.f_bavail * v.f_frsize) > MIN_FREE_BYTES;
}

int artcache_has(const char *track){
    char fp[24]; if(fingerprint(track,fp,sizeof fp)!=0) return 0;
    char cv[600],th[600],bg[600]; cache_paths(fp,cv,th,bg,600);
    return file_ok(cv) && file_ok(th) && file_ok(bg);
}

int artcache_get(const char *track, const char *cover_out, const char *thumb_out, const char *bg_out){
    char fp[24]; if(fingerprint(track,fp,sizeof fp)!=0) return -1;
    char cv[600],th[600],bg[600]; cache_paths(fp,cv,th,bg,600);
    if(!(file_ok(cv) && file_ok(th) && file_ok(bg))) return -1;
    /* copy cache -> the worker's /tmp output paths (fast vs ffmpeg) */
    if(copy_file(cv,cover_out)!=0) return -1;
    if(copy_file(th,thumb_out)!=0) return -1;
    if(copy_file(bg,bg_out)!=0)    return -1;
    return 0;
}

int artcache_get_thumb(const char *track, const char *thumb_out){
    char fp[24]; if(fingerprint(track,fp,sizeof fp)!=0) return -1;
    char cv[600],th[600],bg[600]; cache_paths(fp,cv,th,bg,600);
    if(!file_ok(th)) return -1;          /* thumb not cached (or truncated) */
    if(copy_file(th,thumb_out)!=0) return -1;
    return 0;
}

/* Return the native path of the cached 148px cover for a track (no copy), so a caller can read/decode it
 * directly (e.g. an off-thread cover loader). Returns 0 + fills out on a cache hit, -1 if not cached. */
int artcache_cover_path(const char *track, char *out, int cap){
    char fp[24]; if(fingerprint(track,fp,sizeof fp)!=0) return -1;
    char cv[600],th[600],bg[600]; cache_paths(fp,cv,th,bg,600);
    if(!file_ok(cv)) return -1;
    snprintf(out, cap, "%s", cv);
    return 0;
}

/* Copy JUST the cached 148px cover (no thumb/backdrop) - used by the Album Wall, which only needs the
 * cover, so it avoids copying the ~360px backdrop every step. Returns 0 on hit, -1 if not cached. */
int artcache_get_cover(const char *track, const char *cover_out){
    char fp[24]; if(fingerprint(track,fp,sizeof fp)!=0) return -1;
    char cv[600],th[600],bg[600]; cache_paths(fp,cv,th,bg,600);
    if(!file_ok(cv)) return -1;
    if(copy_file(cv,cover_out)!=0) return -1;
    return 0;
}

/* Monotonic counter bumped whenever a new cover lands in the cache (any decoder: live NP art,
 * per-track prewarm, or the album prewarm). The cover flow watches it to re-bake placeholders. */
static _Atomic unsigned g_ac_gen;
unsigned artcache_gen(void){ return atomic_load(&g_ac_gen); }

void artcache_put(const char *track, const char *cover, const char *thumb, const char *bg){
    char fp[24]; if(fingerprint(track,fp,sizeof fp)!=0) return;
    if(!enough_free()) return;
    if(!sd_write_begin()) return;   /* M18: card is host-owned or an export is in progress -> skip the SD write */
    char dir[600];
    mkdir("/tmp/sdcard/.diskos", 0755);   /* ignore EEXIST */
    mkdir(CACHE_ROOT, 0755);
    snprintf(dir,sizeof dir,"%s/%s",CACHE_ROOT,fp);
    mkdir(dir, 0755);
    char cv[600],th[600],bg2[600]; cache_paths(fp,cv,th,bg2,600);
    int cover_ok = (copy_file(cover,cv)==0);
    copy_file(thumb,th); copy_file(bg,bg2);
    sd_write_end();
    if(cover_ok) atomic_fetch_add(&g_ac_gen, 1);   /* signal the cover flow that a new cover is available */
}
