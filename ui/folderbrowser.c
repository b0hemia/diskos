/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "sdio.h"
#include "folderbrowser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

/* File/folder browser (SCR_FOLDER).
 *
 * Parity item L24: a browsable directory tree of the SD card (mounted at
 * /tmp/sdcard), so a user can navigate folders and reach a track by its file
 * location rather than only by catalog axis (Songs/Albums/Artists/...).
 *
 * WHAT WORKS
 *   - Full folder navigation: folders (sorted first) then audio files (.mp3 /
 *     .flac / .wav, the set diskOS indexes - see scanner.c is_audio). Tap a
 *     folder to descend; the header back-chevron ascends one level and, at the
 *     SD root, leaves the screen.
 *   - Tap an audio file to PLAY that exact track. The file is matched to its
 *     library row by absolute path (the scanner stores PATH as the full
 *     /tmp/sdcard/... path), and playback runs in the all-songs scope via the
 *     same proven path a Songs-view / Search tap uses (ui_play_song_by_path).
 *
 * HONEST LIMITATION (no fake folder-queue)
 *   The stock player builds its play queue (LIST_SONG_0) itself, from the DB,
 *   filtered by a fixed list_type (0=all, 2=artist, 3=album, 5=playlist,
 *   6=favourites, 10=genre). There is NO "play these file paths" / "play this
 *   folder" command in the player's frame surface, and an arbitrary folder is
 *   not a DB list. So "play the whole folder as a queue" is not cleanly
 *   achievable through the existing play path, and we do not fake it (e.g. by
 *   silently minting a throwaway playlist). We browse + play a single tapped
 *   file. A file that is not in the library DB (e.g. an unscanned or
 *   unsupported container) reports "Not in library" instead of playing.
 */

#define FB_ROOT        "/tmp/sdcard"
#define FB_MAXPATH     1024
#define FB_NAMELEN     256
/* Memory-safe ceiling on entries kept per folder. Folders are always retained (scan pass 0);
 * files fill the rest, so a flat music folder of a few thousand tracks shows in full. True
 * unlimited would need a row-recycling virtual list, which this device's RAM does not favour. */
#define FB_MAX_ENTRIES 4000

typedef struct {
    char name[FB_NAMELEN];
    int  is_dir;
} fb_entry_t;

static char        g_dir[FB_MAXPATH];
static fb_entry_t *g_ent;                  /* grown on demand up to FB_MAX_ENTRIES */
static int         g_nent, g_ent_cap;
static lv_obj_t   *g_list;
static lv_obj_t   *g_title;
static lv_timer_t *g_fb_fill;              /* incremental row builder (avoids a long open stall) */
static int         g_fb_i;                 /* next entry index to render */

/* audio files diskOS can play - mirror scanner.c is_audio() exactly. */
static int fb_has_ext(const char *name, const char *ext){
    size_t nl = strlen(name), el = strlen(ext);
    return nl > el && !strcasecmp(name + nl - el, ext);
}
static int fb_is_audio(const char *name){
    /* mirror scanner.c is_audio()'s music set (SONG-resident, folder-playable). .m4b is intentionally
     * NOT here: audiobooks are isolated in their own BOOKS table + Books menu, out of the folder queue. */
    return fb_has_ext(name, ".mp3") || fb_has_ext(name, ".flac")
        || fb_has_ext(name, ".wav") || fb_has_ext(name, ".m4a");
}

/* folders first, then files; case-insensitive within each group. */
static int fb_cmp(const void *a, const void *b){
    const fb_entry_t *x = a, *y = b;
    if(x->is_dir != y->is_dir) return y->is_dir - x->is_dir;   /* dir(1) sorts before file(0) */
    return strcasecmp(x->name, y->name);
}

/* Scan g_dir into g_ent[]. Guards against unstat-able junk and over-long paths
 * the same way scanner.c walk() does: a failed lstat on one entry skips that
 * entry, it never aborts the listing. */
static void fb_fill_stop(void){ if(g_fb_fill){ lv_timer_del(g_fb_fill); g_fb_fill = NULL; } }

static void fb_scan_leased(void){
    fb_fill_stop();                        /* a rescan invalidates any in-flight incremental render */
    g_nent = 0;
    DIR *d = opendir(g_dir);
    if(!d) return;
    struct dirent *e;
    /* Two passes so a user's folders are NEVER dropped by the entry cap in a huge directory:
     * pass 0 admits every subdirectory, pass 1 fills the remaining budget with audio files.
     * A flat Music/ with thousands of files (the user's has 3277) otherwise pushed folders past
     * FB_MAX_ENTRIES in readdir order and hid them entirely, because the folders-first ordering
     * only happens in the post-scan sort. We classify from the dirent d_type when the FS provides
     * it (exfat does) and fall back to lstat only on DT_UNKNOWN, so the extra pass adds no stat()
     * cost on the normal path. */
    for(int pass = 0; pass < 2; pass++){
        for(errno = 0; (e = readdir(d)); errno = 0){
            const char *nm = e->d_name;
            if(nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;  /* skip . and .. */
            if(g_nent >= FB_MAX_ENTRIES) break;
            if(g_nent >= g_ent_cap){                          /* grow the entry array as needed */
                int nc = g_ent_cap ? g_ent_cap * 2 : 128;
                if(nc > FB_MAX_ENTRIES) nc = FB_MAX_ENTRIES;
                fb_entry_t *ne = realloc(g_ent, (size_t)nc * sizeof *g_ent);
                if(!ne) break;                                /* OOM: keep what we have, never crash */
                g_ent = ne; g_ent_cap = nc;
            }

            int is_dir;
            if(e->d_type == DT_DIR)      is_dir = 1;
            else if(e->d_type == DT_REG) is_dir = 0;
            else {                                            /* DT_UNKNOWN/other: stat to classify */
                char path[FB_MAXPATH];
                int pn = snprintf(path, sizeof path, "%s/%s", g_dir, nm);
                if(pn <= 0 || pn >= (int)sizeof path) continue;   /* path too long - skip */
                struct stat st;
                if(lstat(path, &st) != 0) continue;           /* orphaned / unreadable - skip (never fatal) */
                if(S_ISDIR(st.st_mode)) is_dir = 1;
                else if(S_ISREG(st.st_mode)) is_dir = 0;
                else continue;                                /* not a folder or a regular file */
            }
            if(is_dir != (pass == 0)) continue;               /* pass 0 = dirs only, pass 1 = files only */
            if(!is_dir && !fb_is_audio(nm)) continue;         /* files: only the audio we can play */

            fb_entry_t *slot = &g_ent[g_nent];
            snprintf(slot->name, sizeof slot->name, "%s", nm);
            slot->is_dir = is_dir;
            g_nent++;
        }
        if(pass == 0) rewinddir(d);
    }
    closedir(d);
    if(g_nent > 1) qsort(g_ent, g_nent, sizeof g_ent[0], fb_cmp);
}

static void fb_scan(void){
    if(!sd_io_begin()){ fb_fill_stop(); g_nent = 0; return; }
    fb_scan_leased();
    sd_io_end();
}
static const char *fb_basename(const char *p){
    const char *s = strrchr(p, '/');
    return (s && s[1]) ? s + 1 : p;
}

static void fb_empty_label(const char *msg){
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void fb_row_cb(lv_event_t *e);

static void fb_add_row(int i){
        fb_entry_t *en = &g_ent[i];
        lv_obj_t *r = lv_button_create(g_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, 280, 46);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x1C1C1E), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_ext_click_area(r, 2);
        lv_obj_add_event_cb(r, fb_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = lv_label_create(r);
        lv_label_set_text(ic, en->is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO);
        lv_obj_set_pos(ic, 10, 14);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ic, en->is_dir ? lv_color_hex(0xE5C158) : lv_color_hex(0x8E8E93), 0);

        lv_obj_t *nm = lv_label_create(r);
        lv_label_set_text(nm, en->name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(nm, 40, 14);
        lv_obj_set_size(nm, en->is_dir ? 206 : 228, 20);
        lv_obj_set_style_text_font(nm, ui_font_cjk(16), 0);   /* CJK filenames, like Library/Search */
        lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), 0);

        if(en->is_dir){
            lv_obj_t *ch = lv_label_create(r);
            lv_label_set_text(ch, LV_SYMBOL_RIGHT);
            lv_obj_set_pos(ch, 256, 15);
            lv_obj_set_style_text_font(ch, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(ch, lv_color_hex(0x636366), 0);
        }
}

static void fb_fill_cb(lv_timer_t *t){
    (void)t;
    int end = g_fb_i + 40; if(end > g_nent) end = g_nent;   /* render in batches so scrolling stays responsive */
    for(; g_fb_i < end; g_fb_i++) fb_add_row(g_fb_i);
    if(g_fb_i >= g_nent) fb_fill_stop();
}

static void fb_rebuild(void){
    fb_fill_stop();
    if(g_title){
        int at_root = (strcmp(g_dir, FB_ROOT) == 0);
        lv_label_set_text(g_title, at_root ? "Files" : fb_basename(g_dir));
    }
    if(!g_list) return;
    lv_obj_clean(g_list);

    /* opendir failed at scan time -> either no SD or an unreadable dir. */
    if(g_nent == 0){
        int lease = sd_io_begin();
        DIR *probe = lease ? opendir(g_dir) : NULL;
        if(!probe){
            if(lease) sd_io_end();
            fb_empty_label(strcmp(g_dir, FB_ROOT) == 0 ? "No SD card" : "Can't open folder");
            return;
        }
        closedir(probe);
        sd_io_end();
        fb_empty_label("Empty folder");
        return;
    }

    /* Render the first screenful synchronously, then the rest on a timer so a folder with
     * thousands of files doesn't stall the UI on open (mirrors the Library song list). */
    g_fb_i = 0;
    int first = g_nent < 20 ? g_nent : 20;
    for(; g_fb_i < first; g_fb_i++) fb_add_row(g_fb_i);
    if(g_fb_i < g_nent) g_fb_fill = lv_timer_create(fb_fill_cb, 16, NULL);
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);
}

static void fb_descend(const char *name){
    char next[FB_MAXPATH];
    int n = snprintf(next, sizeof next, "%s/%s", g_dir, name);
    if(n <= 0 || n >= (int)sizeof next){ ui_toast("Path too long"); return; }
    snprintf(g_dir, sizeof g_dir, "%s", next);
    fb_scan();
    fb_rebuild();
}

/* Go up one level. At the SD root, leave the screen (the header chevron is the
 * screen's back affordance there). We only ever descend from FB_ROOT, so the
 * parent is always a valid ancestor >= FB_ROOT. */
static void fb_ascend(void){
    if(strcmp(g_dir, FB_ROOT) == 0){ screen_back(); return; }
    char *slash = strrchr(g_dir, '/');
    if(!slash || slash == g_dir || (size_t)(slash - g_dir) < strlen(FB_ROOT)){
        snprintf(g_dir, sizeof g_dir, "%s", FB_ROOT);
    } else {
        *slash = 0;
    }
    fb_scan();
    fb_rebuild();
}

static void fb_play(const char *name){
    char full[FB_MAXPATH];
    int n = snprintf(full, sizeof full, "%s/%s", g_dir, name);
    if(n <= 0 || n >= (int)sizeof full){ ui_toast("Path too long"); return; }
    if(ui_play_song_by_path(full))   /* toasts "Not in library" itself on a miss */
        screen_show(SCR_NOWPLAYING);
}

static void fb_row_cb(lv_event_t *e){
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if(i < 0 || i >= g_nent) return;
    fb_entry_t *en = &g_ent[i];
    if(en->is_dir) fb_descend(en->name);
    else           fb_play(en->name);
}

static void fb_header_back_cb(lv_event_t *e){
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) fb_ascend();
}

void folderbrowser_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title = ui_header_cb(root, "Files", fb_header_back_cb);   /* back-chevron ascends a level */

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 72);
    lv_obj_set_size(g_list, 290, 272);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    snprintf(g_dir, sizeof g_dir, "%s", FB_ROOT);   /* first content built on open() */
}

void folderbrowser_open(void){
    snprintf(g_dir, sizeof g_dir, "%s", FB_ROOT);   /* always (re)enter at the SD root */
    fb_scan();
    fb_rebuild();
    screen_show(SCR_FOLDER);
}
