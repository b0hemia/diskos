/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "folderbrowser.h"
#include "books.h"
#include "musicdb.h"
#include "artcache.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "config.h"

enum { VIEW_MENU, VIEW_SONGS, VIEW_ALBUMS, VIEW_ARTISTS, VIEW_PLAYLISTS, VIEW_FAVS, VIEW_GENRES, VIEW_GROUP, VIEW_MOSTPLAYED, VIEW_RECENT, VIEW_HISTORY, VIEW_COUNT };
#define CAT_FOLDERS 1000   /* menu sentinel: opens the folder browser (a screen, not a library view) */
#define CAT_BOOKS   1001   /* menu sentinel: opens the Books (audiobook) screen */

#define ROW_H 52
#define LIST_Y 70
#define LIST_H 252

static lv_obj_t *g_list;
static lv_obj_t *g_title;
static lv_obj_t *g_az_btn;    /* "A-Z" button -> opens the letter grid */
static lv_obj_t *g_grid;      /* full alphabet grid overlay */
static lv_obj_t *g_lhint = NULL, *g_lhint_lbl = NULL;  /* rim-scroll A-Z position hint */
static uint32_t  g_lhint_tick = 0;
static char      g_lhint_ch = 0;
static int g_view = VIEW_MENU;
static int g_drill_kind = 0;  /* 1 album, 2 artist */
static int g_deeplink = 0;    /* drill opened from the NP hub -> back leaves the Library */
static int g_has_header = 0;   /* a Play All / Shuffle row is the first list child */
static int g_hdr_extra_px = 0; /* extra leading height above the rows (the album cover header), for scroll math */
static char g_drill[MDB_STR];
static library_song_click_cb_t g_song_cb;

/* All per-row buffers are dynamically sized to the library song count (g_alloc_n),
 * so there is NO fixed song/group cap - every view (songs, albums≤N, artists≤N,
 * genres≤N, favourites≤N, group-songs≤N) fits. Allocated once after mdb_load(). */
static int  g_alloc_n = 0;              /* song-bounded buffers (g_buf, g_favs) */
static int  g_grp_cap = 0;             /* group buffers: artists are tokenized so DISTINCT artists can exceed the
                                        * song count - give the album/artist/genre + g_first buffers headroom */
static char *g_first = NULL;            /* per-row first letter, drives the A-Z scrubber (sized to g_grp_cap) */
static int  g_count;

/* group scratch (album/artist/genre views) */
static char (*g_gnames)[MDB_STR]  = NULL;
static char (*g_gartists)[MDB_STR] = NULL;
static int  *g_gcounts = NULL;
static void lib_ensure_group_cap(int need);   /* grow group buffers to an exact count (artist view) */
static mdb_song_t *g_favs = NULL;
static char (*g_plnames)[MDB_STR] = NULL;   /* dynamic: grown to the real playlist count (no 64 cap) */
static long *g_plids = NULL;
static int   g_plcap = 0;                    /* allocated slots in g_plnames/g_plids */

/* Streamed list fill: building all song rows at once froze the UI for seconds, so
 * we render the first screenful immediately and stream the rest in via a timer
 * (non-blocking). g_buf holds the song pointers for SONGS/GROUP. */
static const mdb_song_t **g_buf = NULL;
static int g_fill_i, g_fill_n;
static lv_timer_t *g_fill_timer;

static void library_reload(void);

static char first_letter(const char *s){
    while(*s==' ') s++;
    char c = toupper((unsigned char)*s);
    return (c>='A'&&c<='Z') ? c : '#';
}
static void fmt_dur(char *b, size_t n, int ms){
    if(ms<=0){ b[0]=0; return; }
    int t=(ms+500)/1000; snprintf(b,n,"%d:%02d", t/60, t%60);
}

/* ---- rows --------------------------------------------------------------- */
static lv_obj_t *base_row(void){
    lv_obj_t *r = lv_obj_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 268, ROW_H);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x1C1C1E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}
static void row_two(lv_obj_t *r, const char *top, const char *sub, const char *right){
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, top); lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 12, sub&&sub[0]?6:16); lv_obj_set_size(t, right&&right[0]?186:242, 21);
    lv_obj_set_style_text_font(t, ui_font_cjk(16), 0);   /* CJK titles render via Source Han Sans fallback */
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    if(sub && sub[0]){
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, sub); lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, 12, 28); lv_obj_set_size(s, 242, 17);
        lv_obj_set_style_text_font(s, ui_font_cjk(14), 0);
        lv_obj_set_style_text_color(s, lv_color_hex(0xC7C7CC), 0);
    }
    if(right && right[0]){
        lv_obj_t *rl = lv_label_create(r);
        lv_label_set_text(rl, right);
        lv_obj_set_pos(rl, 204, 17); lv_obj_set_size(rl, 52, 18);
        lv_obj_set_style_text_align(rl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(0x8E8E93), 0);
    }
}

/* ---- callbacks ---------------------------------------------------------- */
static void song_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int id=(int)(uintptr_t)lv_event_get_user_data(e);
    if(g_song_cb) g_song_cb(id);
    screen_show(SCR_NOWPLAYING);
}
static void reload_async(void *p){ (void)p; library_reload(); }

/* ---- "Remove from Favourites?" confirm (long-press is easy to trigger by
 * accident, so removal needs an explicit confirm). ------------------------ */
static lv_obj_t *g_fav_modal;
static int       g_fav_pending_id;
static void fav_modal_close(void){
    if(g_fav_modal){ lv_obj_delete_async(g_fav_modal); g_fav_modal = NULL; }
}
static void fav_cancel_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) fav_modal_close(); }
static void fav_confirm_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(mdb_unfavorite(g_fav_pending_id))
        ui_invalidate_play_scope();   /* only if a Favourites row was actually removed */
    fav_modal_close();
    lv_async_call(reload_async, NULL);
}
static void fav_modal_pill(lv_obj_t *card, int x, const char *txt, uint32_t col, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 42); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -16);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_center(l);
}
static void fav_confirm(int id){
    g_fav_pending_id = id;
    const char *title = "this song";
    for(int i=0;i<g_count;i++) if(g_favs[i].id==id){ title = g_favs[i].title; break; }
    fav_modal_close();
    g_fav_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_fav_modal);
    lv_obj_set_size(g_fav_modal, 360, 360); lv_obj_center(g_fav_modal);
    lv_obj_set_style_bg_color(g_fav_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_fav_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_fav_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_fav_modal, LV_OBJ_FLAG_CLICKABLE);             /* absorb taps */
    lv_obj_add_event_cb(g_fav_modal, fav_cancel_cb, LV_EVENT_CLICKED, NULL); /* tap outside = cancel */
    lv_obj_t *card = lv_obj_create(g_fav_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 264, 168); lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Remove from Favourites?");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_t *s = lv_label_create(card);
    lv_label_set_text(s, title);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, 224);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s, ui_font_cjk(14), 0);   /* song title is user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(s, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 52);
    fav_modal_pill(card, -58, "Cancel", 0xC7C7CC, fav_cancel_cb);
    fav_modal_pill(card,  58, "Remove", 0xFF453A, fav_confirm_cb);
}

/* Favourites rows: short tap plays, long-press asks to remove. (SHORT_CLICKED
 * so a long-press doesn't also fire a play on release.) */
static void fav_row_cb(lv_event_t *e){
    lv_event_code_t c = lv_event_get_code(e);
    int id = (int)(uintptr_t)lv_event_get_user_data(e);
    if(c == LV_EVENT_SHORT_CLICKED){
        if(g_song_cb) g_song_cb(id);
        screen_show(SCR_NOWPLAYING);
    } else if(c == LV_EVENT_LONG_PRESSED){
        fav_confirm(id);
    }
}
static void playlist_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int i=(int)(intptr_t)lv_event_get_user_data(e);
    plview_open(g_plids[i], g_plnames[i]);   /* open the playlist (don't auto-play) */
}
static void pl_new_done(const char *name){   /* keyboard finished */
    if(name && name[0] && mdb_playlist_create(name) > 0) library_reload();
}
static void pl_new_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    kbinput_open("Playlist name", "", pl_new_done);   /* create an empty playlist */
}
static void group_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_SHORT_CLICKED) return;
    int gi=(int)(uintptr_t)lv_event_get_user_data(e);
    g_drill_kind = (g_view==VIEW_ALBUMS)?1:(g_view==VIEW_GENRES)?3:2;
    snprintf(g_drill, MDB_STR, "%s", g_gnames[gi]);
    int keep=g_view; g_view=VIEW_GROUP; (void)keep;
    g_deeplink=0;   /* normal in-library drill: back returns to the category list */
    library_reload();
}
/* Long-press an album/artist/genre row -> play the whole group right away
 * (saves drilling in + tapping Play All). Short tap still drills in. */
static void group_play_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_LONG_PRESSED) return;
    int gi=(int)(uintptr_t)lv_event_get_user_data(e);
    int kind = (g_view==VIEW_ALBUMS)?1:(g_view==VIEW_GENRES)?3:2;
    int lt   = (kind==1)?3:(kind==3)?10:2;   /* 3=album, 10=genre, 2=artist */
    ui_set_workmode(0);                        /* sequential */
    ui_play_list(lt, g_gnames[gi], 1);
    screen_show(SCR_NOWPLAYING);
}
static void menu_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int v=(int)(uintptr_t)lv_event_get_user_data(e);
    if(v==CAT_FOLDERS){ folderbrowser_open(); return; }   /* opens a screen, not a library view */
    if(v==CAT_BOOKS){ books_open(); return; }             /* audiobooks: a screen, not a library view */
    if(v==VIEW_ALBUMS && cfg_get_int("album_view", 0)==1){ screen_show(SCR_ALBUMWALL); return; }  /* cover flow */
    g_view=v;
    g_drill_kind=0; library_reload();
}
/* Step one level back WITHIN the library (drill-in -> its category list -> the
 * category menu). Returns 1 if it handled an internal step, 0 if already at the
 * top menu (so the caller should leave the Library screen). */
int library_back(void){
    if(g_view==VIEW_GROUP){
        if(g_deeplink){  /* opened from the hub -> leave Library entirely (back to hub) */
            g_deeplink=0; g_view=VIEW_MENU; g_drill_kind=0; library_reload(); return 0;
        }
        g_view=(g_drill_kind==1)?VIEW_ALBUMS:(g_drill_kind==3)?VIEW_GENRES:VIEW_ARTISTS;
        g_drill_kind=0; library_reload(); return 1;
    }
    if(g_view==VIEW_MOSTPLAYED || g_view==VIEW_RECENT){ g_view=VIEW_HISTORY; library_reload(); return 1; }  /* stats -> History */
    if(g_view!=VIEW_MENU){ g_view=VIEW_MENU; library_reload(); return 1; }
    return 0;
}
static void back_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    if(!library_back()) screen_back();
}

/* ---- A-Z grid ----------------------------------------------------------- */
/* build one row for the current view at list index i (data already prepared) */
static void add_row(int i){
    char dur[12];
    lv_obj_t *r = base_row();
    switch(g_view){
        case VIEW_SONGS: case VIEW_GROUP:
            lv_obj_add_event_cb(r, song_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)g_buf[i]->id);
            fmt_dur(dur,sizeof dur,g_buf[i]->dur_ms);
            row_two(r, g_buf[i]->title[0]?g_buf[i]->title:"Untitled", g_buf[i]->artist, dur);
            break;
        case VIEW_ALBUMS: {
            lv_obj_add_event_cb(r, group_cb,      LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i);
            lv_obj_add_event_cb(r, group_play_cb, LV_EVENT_LONG_PRESSED,  (void*)(uintptr_t)i);
            char cnt[12]; snprintf(cnt,sizeof cnt,"%d",g_gcounts[i]);
            row_two(r, g_gnames[i], g_gartists[i], cnt);
            break; }
        case VIEW_ARTISTS:
            lv_obj_add_event_cb(r, group_cb,      LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i);
            lv_obj_add_event_cb(r, group_play_cb, LV_EVENT_LONG_PRESSED,  (void*)(uintptr_t)i);
            row_two(r, g_gnames[i], NULL, NULL);
            break;
        case VIEW_GENRES: {
            lv_obj_add_event_cb(r, group_cb,      LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i);
            lv_obj_add_event_cb(r, group_play_cb, LV_EVENT_LONG_PRESSED,  (void*)(uintptr_t)i);
            char cnt[12]; snprintf(cnt,sizeof cnt,"%d",g_gcounts[i]);
            row_two(r, g_gnames[i], NULL, cnt);
            break; }
        case VIEW_FAVS:
            lv_obj_add_event_cb(r, fav_row_cb, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)g_favs[i].id);
            lv_obj_add_event_cb(r, fav_row_cb, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)g_favs[i].id);
            fmt_dur(dur,sizeof dur,g_favs[i].dur_ms);
            row_two(r, g_favs[i].title, g_favs[i].artist, dur);
            break;
        case VIEW_MOSTPLAYED: case VIEW_RECENT:   /* tap plays the song (via song_cb, by id) */
            lv_obj_add_event_cb(r, song_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)g_favs[i].id);
            fmt_dur(dur,sizeof dur,g_favs[i].dur_ms);
            row_two(r, g_favs[i].title[0]?g_favs[i].title:"Untitled", g_favs[i].artist, dur);
            break;
    }
}
static void fill_stop(void){ if(g_fill_timer){ lv_timer_del(g_fill_timer); g_fill_timer=NULL; } }
static void fill_cb(lv_timer_t *t){
    int end = g_fill_i + 40; if(end > g_fill_n) end = g_fill_n;
    for(; g_fill_i < end; g_fill_i++) add_row(g_fill_i);
    if(g_fill_i >= g_fill_n) fill_stop();
    (void)t;
}
static void fill_start(int n){
    fill_stop();
    g_fill_n = n; g_fill_i = 0;
    int first = n < 18 ? n : 18;                 /* first screenful, instantly */
    for(; g_fill_i < first; g_fill_i++) add_row(g_fill_i);
    if(g_fill_i < g_fill_n) g_fill_timer = lv_timer_create(fill_cb, 16, NULL);
}
static void fill_flush(void){                    /* render the rest now (before a jump) */
    for(; g_fill_i < g_fill_n; g_fill_i++) add_row(g_fill_i);
    fill_stop();
}

static void jump_to_letter(char L){
    if(g_count<=0) return;
    fill_flush();                                /* ensure the target row exists */
    int idx=-1;
    for(int i=0;i<g_count;i++) if(g_first[i]==L){ idx=i; break; }
    if(idx<0) for(int i=0;i<g_count;i++) if(g_first[i]>=L){ idx=i; break; } /* nearest after */
    if(idx<0) idx=g_count-1;
    /* row pitch = ROW_H + the list's 4px flex pad_row, so idx*ROW_H alone lands
     * progressively short for later letters. */
    lv_obj_scroll_to_y(g_list, (idx + g_has_header)*(ROW_H+4), LV_ANIM_OFF);
}
static void letter_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    jump_to_letter((char)(intptr_t)lv_event_get_user_data(e));
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void grid_bg_cb(lv_event_t *e){    /* tap outside closes */
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void az_btn_cb(lv_event_t *e){
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
}
static void az_show(int on){
    if(on) lv_obj_clear_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN); }
}

/* ---- populate ----------------------------------------------------------- */
static lv_obj_t *empty_label(const char *txt){
    lv_obj_t *l = lv_label_create(g_list);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 268);
    return l;
}
/* empty music view: the message + a tappable "Scan Library" so a new user isn't dead-ended */
static void scan_action_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    ui_rescan_library();
}
static void empty_scan(const char *txt){
    empty_label(txt);
    lv_obj_t *b = lv_button_create(g_list);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 200, 46);
    lv_obj_set_style_radius(b, 23, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, scan_action_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_REFRESH "  Scan Library");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
}

/* ---- Play All / Shuffle All header for song lists ----------------------- */
/* list_type + filter name for the current song view (0100 jump-table types:
 * 1=all, 2=artist, 3=album, 6=favourites, 10=genre). Returns 1 if playable. */
static int current_list_context(int *lt, char *name, int cap){
    if(g_view==VIEW_SONGS){ if(lt)*lt=1; if(name&&cap>0)name[0]=0; return 1; }
    if(g_view==VIEW_FAVS){  if(lt)*lt=6; if(name&&cap>0)name[0]=0; return 1; }
    if(g_view==VIEW_GROUP)  return library_drill_context(lt, name, cap);
    return 0;
}
/* Play the current list, setting the play-mode first (0=Sequential, 1=Shuffle).
 * Play-mode = 0102 (ground-truth captured 2026-06-25); ui_set_workmode applies it. */
static void play_list_mode(int mode){
    int lt; char nm[160];
    if(!current_list_context(&lt, nm, sizeof nm)) return;
    cfg_set_int("work_mode", mode); ui_set_workmode(mode);
    int pos = 1;
    /* shuffle: start on a random track (not always the first). Skip artist lists (lt==2):
     * the UI's split-artist count can exceed the player's exact ARTIST=? list -> out-of-range pos. */
    if(mode == 1 && g_count > 1 && lt != 2) pos = 1 + (rand() % g_count);
    ui_play_list(lt, nm, pos);
    screen_show(SCR_NOWPLAYING);
}
static void play_all_cb(lv_event_t *e){    if(lv_event_get_code(e)==LV_EVENT_CLICKED) play_list_mode(0); } /* sequential */
static void shuffle_all_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) play_list_mode(1); } /* shuffle */

static void hdr_btn(lv_obj_t *row, int x, const char *txt, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 128, 44); lv_obj_set_pos(b, x, 4);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
}
/* Album detail header: the album cover on top (from the art cache) with the artist beneath it, shown
 * above the Play All / Shuffle row for an album drill - from EITHER the list view or the cover flow. */
static int g_hdr_cover_ctr;
static void add_album_cover_header(const char *album, const char *artist){
    lv_obj_t *r = lv_obj_create(g_list);
    lv_obj_remove_style_all(r);
    int has_artist = artist && artist[0];
    int hdr_h = has_artist ? 196 : 172;
    lv_obj_set_size(r, 286, hdr_h);   /* full list width so the cover screen-centres */
    g_hdr_extra_px = hdr_h + 4;        /* +flex pad_row: the A-Z scroll math subtracts this leading height */
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    const int CX = 7;   /* the list sits at x30/w286 (centre 173); +7 puts TOP_MID content at screen-centre 180 */

    /* Resolve the album's cached 148px cover (first track that has one) into a ROTATING /tmp file so
     * LVGL's image cache can't serve a previous album's bitmap for a reused filename. */
    char src[64]; src[0] = 0;
    int ids[6]; int n = mdb_album_track_ids(album, ids, 6);
    for(int i=0;i<n;i++){
        char track[512]; track[0]=0;
        char tmp[48]; snprintf(tmp, sizeof tmp, "/tmp/album_hdr%d.bmp", g_hdr_cover_ctr & 3);
        if(mdb_song_path(ids[i], track, sizeof track) && track[0] && artcache_get_cover(track, tmp)==0){
            snprintf(src, sizeof src, "A:%s", tmp); g_hdr_cover_ctr++;
            break;
        }
    }
    if(src[0]){
        lv_obj_t *img = lv_image_create(r);
        lv_image_set_src(img, src);                         /* native 148x148 BMP (no scale/rotate: SW renderer) */
        lv_obj_align(img, LV_ALIGN_TOP_MID, CX, 10);
    } else {                                                /* art-less album -> dark tile with its initial */
        lv_obj_t *ph = lv_obj_create(r);
        lv_obj_remove_style_all(ph);
        lv_obj_set_size(ph, 148, 148); lv_obj_align(ph, LV_ALIGN_TOP_MID, CX, 10);
        lv_obj_set_style_bg_color(ph, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ph, 12, 0);
        lv_obj_t *in = lv_label_create(ph);
        char init[8]={0};   /* one WHOLE UTF-8 code point (a bare first byte tofus Cyrillic/CJK) */
        if(album && album[0]){ unsigned char c0=(unsigned char)album[0];
            int len=(c0<0x80)?1:((c0>>5)==0x6)?2:((c0>>4)==0xE)?3:((c0>>3)==0x1E)?4:1;
            for(int i=0;i<len && album[i];i++) init[i]=album[i];
            if(len==1 && init[0]>='a'&&init[0]<='z') init[0]-=32;
        } else init[0]='?';
        lv_label_set_text(in, init);
        /* ui_text_font tops out at 20 (would silently shrink a 28 request to 16): use the big latin face
         * for an ASCII initial, else the largest CJK-capable size so a non-Latin initial still renders. */
        lv_obj_set_style_text_font(in, (unsigned char)init[0] < 0x80 ? &lv_font_montserrat_28 : ui_font_cjk(20), 0);
        lv_obj_set_style_text_color(in, lv_color_hex(0x8E8E93), 0);
        lv_obj_center(in);
    }
    if(has_artist){
        lv_obj_t *a = lv_label_create(r);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT); lv_obj_set_width(a, 240);
        lv_obj_set_style_text_align(a, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(a, artist);
        lv_obj_set_style_text_font(a, ui_text_font(15), 0);
        lv_obj_set_style_text_color(a, lv_color_hex(0x8E8E93), 0);
        lv_obj_align(a, LV_ALIGN_TOP_MID, CX, 166);
    }
}

/* the first row of a song list: [ Play All ] [ Shuffle ]. centered=1 screen-centres the pair (used under
 * the album cover card, so it lines up with the centred cover instead of the left-aligned list rows). */
static void add_play_header(int centered){
    lv_obj_t *r = lv_obj_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, centered ? 286 : 268, ROW_H);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    int base = centered ? 16 : 0;                 /* r-local 16 => screen-centre (list is at x30/w286) */
    hdr_btn(r, base,       LV_SYMBOL_PLAY    "  Play All", play_all_cb);
    hdr_btn(r, base + 140, LV_SYMBOL_SHUFFLE "  Shuffle",  shuffle_all_cb);
    g_has_header = 1;
}

static const char *VIEW_TITLE[] = { "Library","Songs","Albums","Artists","Playlists","Favourites","Genres","","Most Played","Recently Played","History" };
/* keep VIEW_TITLE[] in lockstep with the view enum so VIEW_TITLE[g_view] can't read out of bounds */
_Static_assert(sizeof(VIEW_TITLE)/sizeof(VIEW_TITLE[0]) == VIEW_COUNT, "VIEW_TITLE must have one entry per view");

static void library_reload(void){
    if(!g_list) return;
    if(g_lhint){ lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN); g_lhint_ch = 0; }  /* don't leave a stale A-Z hint across views */
    fill_stop();              /* cancel any in-flight stream before wiping rows */
    lv_obj_clean(g_list);
    g_count = 0;
    g_has_header = 0;
    g_hdr_extra_px = 0;
    lv_obj_scroll_to_y(g_list, 0, LV_ANIM_OFF);

    const char *ttl = (g_view==VIEW_GROUP) ? g_drill : VIEW_TITLE[g_view];
    if(g_title) lv_label_set_text(g_title, ttl);
    az_show(0);

    /* one-time discoverability hints for the invisible long-press actions */
    static int s_group_hold_hint=0, s_fav_hold_hint=0;
    if((g_view==VIEW_ALBUMS || g_view==VIEW_ARTISTS || g_view==VIEW_GENRES) && !s_group_hold_hint){
        s_group_hold_hint=1; ui_toast("Hold an item to play all");
    } else if(g_view==VIEW_FAVS && !s_fav_hold_hint){
        s_fav_hold_hint=1; ui_toast("Hold to remove");
    }

    char dur[12];

    if(g_view==VIEW_MENU){
        /* "Most Played" + "Recently Played" are play STATS, not catalog axes -> grouped under History */
        static const char *CATS[] = { "Songs","Albums","Artists","Genres","Playlists","Favourites","Folders","Books","History" };
        static const int   CATV[] = { VIEW_SONGS,VIEW_ALBUMS,VIEW_ARTISTS,VIEW_GENRES,VIEW_PLAYLISTS,VIEW_FAVS,CAT_FOLDERS,CAT_BOOKS,VIEW_HISTORY };
        for(int i=0;i<(int)(sizeof CATS/sizeof CATS[0]);i++){
            if(!DISKOS_AUDIOBOOKS && CATV[i]==CAT_BOOKS) continue;   /* compile-time gate (DISKOS_AUDIOBOOKS, currently 1): hides the Books row when the audiobook feature is built out */
            lv_obj_t *r = base_row();
            lv_obj_add_event_cb(r, menu_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)CATV[i]);
            row_two(r, CATS[i], NULL, NULL);
            lv_obj_t *ch = lv_label_create(r);
            lv_label_set_text(ch, LV_SYMBOL_RIGHT);
            lv_obj_set_pos(ch, 240, 17);
            lv_obj_set_style_text_color(ch, lv_color_hex(0x636366), 0);
        }
        return;
    }
    if(g_view==VIEW_HISTORY){          /* sub-menu: the two play-stats views */
        static const char *HCATS[] = { "Most Played","Recently Played" };
        static const int   HCATV[] = { VIEW_MOSTPLAYED, VIEW_RECENT };
        for(int i=0;i<2;i++){
            lv_obj_t *r = base_row();
            lv_obj_add_event_cb(r, menu_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)HCATV[i]);
            row_two(r, HCATS[i], NULL, NULL);
            lv_obj_t *ch = lv_label_create(r);
            lv_label_set_text(ch, LV_SYMBOL_RIGHT);
            lv_obj_set_pos(ch, 240, 17);
            lv_obj_set_style_text_color(ch, lv_color_hex(0x636366), 0);
        }
        return;
    }

    (void)dur;
    if(g_view==VIEW_SONGS || g_view==VIEW_GROUP){
        int n;
        if(g_view==VIEW_GROUP) n=(g_drill_kind==1)?mdb_album_songs(g_drill,g_buf,g_alloc_n):(g_drill_kind==3)?mdb_genre_songs(g_drill,g_buf,g_alloc_n):mdb_artist_songs(g_drill,g_buf,g_alloc_n);
        else { n=mdb_song_count(); if(n>g_alloc_n) n=g_alloc_n; for(int i=0;i<n;i++) g_buf[i]=mdb_song(i); }
        if(n<=0){ empty_scan("No songs found"); return; }
        int is_album = (g_view==VIEW_GROUP && g_drill_kind==1);
        if(is_album) add_album_cover_header(g_drill, n>0 ? g_buf[0]->artist : NULL);  /* album drill: cover on top */
        add_play_header(is_album);                                       /* centre the pair under the album cover */
        for(int i=0;i<n;i++) g_first[i]=first_letter(g_buf[i]->title);
        g_count=n; fill_start(n);
        if(g_view==VIEW_SONGS) az_show(1);
    } else if(g_view==VIEW_ALBUMS){
        int n=mdb_albums(g_gnames,g_gartists,g_gcounts,g_grp_cap);
        if(n<=0){ empty_scan("No albums found"); return; }
        for(int i=0;i<n;i++) g_first[i]=first_letter(g_gnames[i]);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_ARTISTS){
        lib_ensure_group_cap(mdb_artist_count());   /* size for every distinct artist so none are clipped */
        int n=mdb_artists(g_gnames,g_grp_cap);
        if(n<=0){ empty_scan("No artists found"); return; }
        for(int i=0;i<n;i++) g_first[i]=first_letter(g_gnames[i]);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_GENRES){
        int n=mdb_genres(g_gnames,g_gcounts,g_grp_cap);
        if(n<=0){ empty_scan("No genres found"); return; }
        for(int i=0;i<n;i++) g_first[i]=first_letter(g_gnames[i]);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_FAVS){
        int n=mdb_favorites(g_favs, g_alloc_n);
        if(n<=0){ empty_label("No Favourites yet"); return; }
        add_play_header(0);
        for(int i=0;i<n;i++) g_first[i]=first_letter(g_favs[i].title);
        g_count=n; fill_start(n); az_show(1);
    } else if(g_view==VIEW_MOSTPLAYED || g_view==VIEW_RECENT){
        int n = (g_view==VIEW_MOSTPLAYED) ? mdb_mostplayed(g_favs, g_alloc_n) : mdb_recent(g_favs, g_alloc_n);
        if(n<=0){ empty_label("Nothing played yet"); return; }
        g_count=n; fill_start(n);   /* ordered by plays / recency -> no Play-All header, no A-Z */
    } else { /* VIEW_PLAYLISTS */
        /* grow the buffers to the real playlist count (kept file-scope: playlist_cb reads
         * g_plids[i]/g_plnames[i] after this returns). A partial realloc keeps the old cap. */
        int num = mdb_playlist_num();
        /* overflow-guard the size_t multiply on 32-bit: MDB_STR is the larger stride so it
         * bounds both arrays. A pathological count just keeps the old capacity. */
        if(num > g_plcap && (size_t)num <= ((size_t)-1) / MDB_STR){
            char (*nn)[MDB_STR] = realloc(g_plnames, (size_t)num*MDB_STR);
            long  *ni           = realloc(g_plids,   (size_t)num*sizeof(long));
            if(nn) g_plnames = nn;
            if(ni) g_plids   = ni;
            if(nn && ni) g_plcap = num;
        }
        int n = (g_plcap > 0 && g_plnames && g_plids) ? mdb_playlists(g_plnames, g_plids, g_plcap) : 0;
        /* "New Playlist" is always first so an empty library can still create one */
        lv_obj_t *nr=base_row();
        lv_obj_add_event_cb(nr, pl_new_cb, LV_EVENT_CLICKED, NULL);
        row_two(nr, LV_SYMBOL_PLUS "  New Playlist", NULL, NULL);
        for(int i=0;i<n;i++){
            lv_obj_t *r=base_row();
            lv_obj_add_event_cb(r, playlist_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
            char cnt[16]; snprintf(cnt,sizeof cnt,"%d", mdb_playlist_count(g_plids[i]));
            row_two(r, g_plnames[i], NULL, cnt);
        }
    }
}

void library_set_song_click_cb(library_song_click_cb_t cb){ g_song_cb=cb; }

/* Rebuild the current Library view from the DB - used after an external change
 * (e.g. a playlist deleted from the playlist view) so the list isn't stale. */
void library_refresh(void){ if(g_list) library_reload(); }
lv_obj_t *library_scroller(void){ return g_list; }

/* If the user is inside an album/artist/genre drill-in, report the player
 * list_type (3=album,2=artist,10=genre) + name so a song tap plays the exact
 * track within that list. Returns 0 when in a flat view (Songs/Search/Favs),
 * where the caller falls back to the song's own album. */
int library_drill_context(int *list_type, char *name, int cap){
    if(g_view != VIEW_GROUP) return 0;
    int lt = (g_drill_kind==1)?3 : (g_drill_kind==3)?10 : 2;
    if(list_type) *list_type = lt;
    if(name && cap>0) snprintf(name, cap, "%s", g_drill);
    return 1;
}

/* deep-link from the Now Playing 3-dot menu: jump straight to an album/artist */
void library_open_album(const char *name){
    g_view=VIEW_GROUP; g_drill_kind=1; g_deeplink=1; snprintf(g_drill, MDB_STR, "%s", name); library_reload();
}
void library_open_artist(const char *name){
    g_view=VIEW_GROUP; g_drill_kind=2; g_deeplink=1; snprintf(g_drill, MDB_STR, "%s", name); library_reload();
}

/* ---- rim-scroll alphabet hint: a big centred letter shown while flying through
 * an alphabetical list, auto-hidden ~650ms after scrolling stops. -------------- */
static void lhint_timer_cb(lv_timer_t *t){ (void)t;
    if(g_lhint && !lv_obj_has_flag(g_lhint, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(g_lhint_tick) > 650)
        lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
}
/* called from the rim-scroll handler while the Library list is the one scrolling */
void library_scroll_letter_tick(void){
    if(!g_list || !g_lhint || g_count <= 0) return;
    /* Only the views that actually fill g_first[] (alphabetical lists) get the A-Z scrubber hint.
     * MostPlayed/Recent/History (and Menu/Playlists) never populate g_first, so reading it there
     * showed stale letters from a prior view. Allowlist, so any new view defaults to no hint. */
    if(!(g_view==VIEW_SONGS || g_view==VIEW_GROUP || g_view==VIEW_ALBUMS ||
         g_view==VIEW_ARTISTS || g_view==VIEW_GENRES || g_view==VIEW_FAVS)) return;
    int pitch = ROW_H + 4;
    int idx = (lv_obj_get_scroll_y(g_list) - g_hdr_extra_px)/pitch - g_has_header;
    if(idx < 0) idx = 0; else if(idx >= g_count) idx = g_count - 1;
    char ch = g_first[idx];
    if(ch && ch != g_lhint_ch){ g_lhint_ch = ch; char b[2]={ch,0}; lv_label_set_text(g_lhint_lbl, b); }
    g_lhint_tick = lv_tick_get();
    lv_obj_clear_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
}

/* (Re)allocate all per-row buffers to fit `songs` tracks (min 1); frees any prior buffers.
 * On any malloc failure, fails closed (g_alloc_n=0 -> empty views, never OOB). */
static void lib_alloc_buffers(int songs){
    free(g_buf); free(g_favs); free(g_first); free(g_gnames); free(g_gartists); free(g_gcounts);
    g_buf=NULL; g_favs=NULL; g_first=NULL; g_gnames=NULL; g_gartists=NULL; g_gcounts=NULL;
    g_alloc_n = songs > 0 ? songs : 1;    /* song-bounded buffers */
    g_grp_cap = g_alloc_n * 2 + 64;       /* group buffers: headroom for tokenized artists */
    g_buf      = malloc((size_t)g_alloc_n * sizeof *g_buf);
    g_favs     = malloc((size_t)g_alloc_n * sizeof *g_favs);
    g_first    = malloc((size_t)g_grp_cap);
    g_gnames   = malloc((size_t)g_grp_cap * sizeof *g_gnames);
    g_gartists = malloc((size_t)g_grp_cap * sizeof *g_gartists);
    g_gcounts  = malloc((size_t)g_grp_cap * sizeof *g_gcounts);
    if(!g_buf || !g_favs || !g_first || !g_gnames || !g_gartists || !g_gcounts){
        fprintf(stderr, "library: buffer alloc failed for %d songs\n", g_alloc_n);
        free(g_buf); free(g_favs); free(g_first); free(g_gnames); free(g_gartists); free(g_gcounts);
        g_buf=NULL; g_favs=NULL; g_first=NULL; g_gnames=NULL; g_gartists=NULL; g_gcounts=NULL;
        g_alloc_n = 0; g_grp_cap = 0;     /* release the partial allocs under memory pressure */
    }
}
/* Grow the buffers if the library outgrew them - e.g. a clean first boot allocated for 1 song
 * and a rescan then added thousands. Call after any mdb_load() that may have added tracks. */
void library_ensure_capacity(void){
    int total = mdb_total_song_count();   /* full catalog: unfiltered Favourites/history also fill these buffers */
    if(total > g_alloc_n) lib_alloc_buffers(total);
}

/* Grow ONLY the group buffers (artist/album/genre rows + A-Z first-letters) to hold `need` entries.
 * The default headroom (2*songs+64) is ample, but tokenized artists have no song-bounded ceiling,
 * so the artist view sizes to the exact distinct count first - no silent clipping. A realloc miss
 * leaves the current (smaller) capacity, which callers still respect via g_grp_cap. */
static void lib_ensure_group_cap(int need){
    if(need <= g_grp_cap) return;
    char (*a)[MDB_STR] = realloc(g_gnames,   (size_t)need * sizeof *g_gnames);   if(!a) return; g_gnames   = a;
    char (*b)[MDB_STR] = realloc(g_gartists, (size_t)need * sizeof *g_gartists); if(!b) return; g_gartists = b;
    int   *c           = realloc(g_gcounts,  (size_t)need * sizeof *g_gcounts);  if(!c) return; g_gcounts  = c;
    char  *f           = realloc(g_first,    (size_t)need);                      if(!f) return; g_first    = f;
    g_grp_cap = need;
}

void library_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_title = ui_header_cb(root, "Library", back_cb);   /* shared header; back_cb pops the internal view stack */

    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 30, LIST_Y); lv_obj_set_size(g_list, 286, LIST_H);
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* round bottom bezel */
    lv_obj_set_style_pad_row(g_list, 4, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    /* "A-Z" button (bottom-right) opens the alphabet grid */
    g_az_btn = lv_button_create(root);
    lv_obj_remove_style_all(g_az_btn);
    lv_obj_set_pos(g_az_btn, 302, 158); lv_obj_set_size(g_az_btn, 44, 44);
    lv_obj_set_style_radius(g_az_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_az_btn, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(g_az_btn, LV_OPA_90, 0);
    lv_obj_add_event_cb(g_az_btn, az_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *azl=lv_label_create(g_az_btn); lv_label_set_text(azl,"A-Z");
    lv_obj_set_style_text_font(azl,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(azl,lv_color_hex(0xFFFFFF),0); lv_obj_center(azl);
    lv_obj_add_flag(g_az_btn, LV_OBJ_FLAG_HIDDEN);

    /* alphabet grid overlay */
    g_grid = lv_obj_create(root);
    lv_obj_remove_style_all(g_grid);
    lv_obj_set_size(g_grid, 360, 360); lv_obj_set_pos(g_grid, 0, 0);
    lv_obj_set_style_bg_color(g_grid, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_grid, LV_OPA_80, 0);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_grid, grid_bg_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(g_grid, LV_OBJ_FLAG_HIDDEN);
    {   /* "Jump to" title disambiguates the grid as navigation, not a sort control (grid starts y0=48) */
        lv_obj_t *gt = lv_label_create(g_grid);
        lv_label_set_text(gt, "Jump to");
        lv_obj_align(gt, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_set_style_text_font(gt, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(gt, lv_color_hex(0x8E8E93), 0);
    }
    {
        static const char *AZ="ABCDEFGHIJKLMNOPQRSTUVWXYZ#";
        int cols=5, cw=52, ch=48, n=27;      /* taller cells (48x44); last partial row centered */
        int gw=cols*cw, x0=(360-gw)/2;
        int rows=(n+cols-1)/cols;
        int y0=48;   /* anchor top at the original grid top; taller cells grow downward (centering would clip the top row's outer corners) */
        for(int i=0;i<n;i++){
            int r=i/cols, c=i%cols;
            int cells_in_row = (r==rows-1) ? (n - r*cols) : cols;  /* last row may be partial */
            int row_x0 = x0 + ((cols - cells_in_row)*cw)/2;        /* centre it so Z/# avoid the clipped bottom-left corner */
            lv_obj_t *cell=lv_button_create(g_grid);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, row_x0+c*cw, y0+r*ch); lv_obj_set_size(cell, cw-4, ch-4);
            lv_obj_set_style_radius(cell, 8, 0);
            lv_obj_set_style_bg_color(cell, lv_color_hex(0xFF375F), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);
            lv_obj_add_event_cb(cell, letter_cb, LV_EVENT_CLICKED, (void*)(intptr_t)AZ[i]);
            lv_obj_t *l=lv_label_create(cell);
            char b[2]={AZ[i],0}; lv_label_set_text(l,b);
            lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0);
            lv_obj_set_style_text_color(l,lv_color_hex(0xFFFFFF),0); lv_obj_center(l);
        }
    }

    /* alphabet hint overlay (rim-scroll position indicator) - centred, hidden */
    g_lhint = lv_obj_create(root);
    lv_obj_remove_style_all(g_lhint);
    lv_obj_set_size(g_lhint, 96, 96);
    lv_obj_center(g_lhint);
    lv_obj_set_style_radius(g_lhint, 22, 0);
    lv_obj_set_style_bg_color(g_lhint, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(g_lhint, LV_OPA_80, 0);
    lv_obj_clear_flag(g_lhint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_lhint, LV_OBJ_FLAG_HIDDEN);
    g_lhint_lbl = lv_label_create(g_lhint);
    lv_obj_set_style_text_font(g_lhint_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(g_lhint_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(g_lhint_lbl, "A");
    lv_obj_center(g_lhint_lbl);
    lv_timer_create(lhint_timer_cb, 150, NULL);

    mdb_load();
    int total = mdb_total_song_count();          /* size for the FULL catalog: Favourites/Most-Played/Recent
                                                  * query unfiltered rows (incl. audiobooks), so music-only
                                                  * count would undersize them */
    if(total < 0) total = mdb_song_count() > 256 ? mdb_song_count() : 256;   /* COUNT failed -> generous floor, don't undersize */
    lib_alloc_buffers(total);
    library_reload();
}
