/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "ipc.h"
#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Quick Settings: an iOS-Control-Center-style pull-down panel (top-edge swipe-down; gesture handled
 * in main.c). It is USER-CUSTOMISABLE: the brightness bar sits on top, an optional transport row below
 * it, then a grid of up to six circular tiles chosen in Settings > Display > Quick Settings. The tile
 * palette is Wi-Fi, Bluetooth, EQ, Search, Rescan, Working Mode and Screen off. "On" tiles fill with the
 * live accent colour (album-art derived, or the user's fixed pick); everything else is a dark disc.
 * The panel is rebuilt from config on every open so a customisation change shows up immediately.
 * Dismissed by the back-swipe (screen_back). */

LV_FONT_DECLARE(font_icons_28)          /* FontAwesome 28px - brightness sun */
LV_FONT_DECLARE(font_icons_20)          /* FontAwesome 20px - EQ + Search glyphs (match the LVGL symbols) */
#define WI_SUN    "\xEF\x86\x85"        /* f185 sun */
#define WI_EQ     "\xEF\x87\x9E"        /* f1de sliders = equalizer */
#define WI_SEARCH "\xEF\x80\x82"        /* f002 magnifier = search */
#define WI_HEART  "\xEF\x80\x84"        /* f004 heart = favourite */
#define WI_CLOCK  "\xEF\x80\x97"        /* f017 clock = sleep timer */
#define WI_HP     "\xEF\x80\xA5"        /* f025 headphones = DRE */
#define WI_LINK   "\xEF\x83\x81"        /* f0c1 chain = gapless */
#define WI_MIC    "\xEF\x84\xB0"        /* f130 microphone = lyrics */

#define QS_CARD   0x1C1C1E
#define QS_PRESS  0x2C2C2E
#define QS_TRACK  0x2C2C2E
#define QS_OFF    0x3A3A3C
#define QS_GRAB   0x5A5A5E
#define QS_TXT2   0x8E8E93

#define QS_MAX_TILES 6                  /* grid holds two rows of three */

/* ---- tile palette ---------------------------------------------------------------------------- */
enum { QT_WIFI, QT_BT, QT_EQ, QT_SEARCH, QT_RESCAN, QT_MODE, QT_SCREENOFF,
       QT_SHUFFLE, QT_FAV, QT_LYRICS, QT_TIMER, QT_GAPLESS, QT_GAIN, QT_DRE, QT_RG, QT_COUNT };

typedef struct {
    const char       *cfg;      /* per-tile enable key */
    const char       *cap;      /* caption under the circle */
    const char       *glyph;
    const lv_font_t  *font;
    int               def_on;   /* enabled by default */
    int               has_long; /* has a long-press action (opens a screen) */
} qtile_t;

static const qtile_t QTILES[QT_COUNT] = {
    [QT_WIFI]      = { "qs_t_wifi",      "Wi-Fi",      LV_SYMBOL_WIFI,      &lv_font_montserrat_20, 1, 1 },
    [QT_BT]        = { "qs_t_bt",        "Bluetooth",  LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_20, 1, 1 },
    [QT_EQ]        = { "qs_t_eq",        "EQ",         WI_EQ,               &font_icons_20,         1, 1 },
    [QT_SEARCH]    = { "qs_t_search",    "Search",     WI_SEARCH,           &font_icons_20,         1, 0 },
    [QT_RESCAN]    = { "qs_t_rescan",    "Rescan",     LV_SYMBOL_REFRESH,   &lv_font_montserrat_20, 1, 0 },
    [QT_MODE]      = { "qs_t_mode",      "Source",     LV_SYMBOL_AUDIO,     &lv_font_montserrat_20, 1, 0 },
    [QT_SCREENOFF] = { "qs_t_screenoff", "Screen off", LV_SYMBOL_EYE_CLOSE, &lv_font_montserrat_20, 0, 0 },
    /* opt-in audio/playback tiles (default off; enable in Settings > Display > Quick Settings) */
    [QT_SHUFFLE]   = { "qs_t_shuffle",   "Shuffle",    LV_SYMBOL_SHUFFLE,   &lv_font_montserrat_20, 0, 1 },
    [QT_FAV]       = { "qs_t_fav",       "Favourite",  WI_HEART,            &font_icons_20,         0, 0 },
    [QT_LYRICS]    = { "qs_t_lyrics",    "Lyrics",     WI_MIC,              &font_icons_20,         0, 0 },
    [QT_TIMER]     = { "qs_t_timer",     "Timer",      WI_CLOCK,            &font_icons_20,         0, 0 },
    [QT_GAPLESS]   = { "qs_t_gapless",   "Gapless",    WI_LINK,             &font_icons_20,         0, 1 },
    [QT_GAIN]      = { "qs_t_gain",      "High Gain",  LV_SYMBOL_VOLUME_MAX,&lv_font_montserrat_20, 0, 1 },
    [QT_DRE]       = { "qs_t_dre",       "DRE",        WI_HP,               &font_icons_20,         0, 1 },
    [QT_RG]        = { "qs_t_rg",        "ReplayGain", LV_SYMBOL_VOLUME_MID,&lv_font_montserrat_20, 0, 0 },
};

static lv_obj_t *g_qs_root;
static lv_obj_t *g_bright;
static lv_obj_t *g_pp_glyph;                 /* transport play/pause glyph (NULL when transport is off) */
static lv_obj_t *g_tile_dot[QT_COUNT];       /* the circle per shown tile, recoloured on refresh */

/* the drawer shows at most this many tiles - one fewer with the transport row, so the second row never
 * pushes its outer captions off the round bezel. */
static int qs_cap(void){ return cfg_get_int("qs_transport", 0) ? QS_MAX_TILES - 1 : QS_MAX_TILES; }

/* which tiles are enabled, in palette order, capped. Returns the count. */
static int qs_enabled(int ids[QT_COUNT], int cap){
    int n = 0;
    for(int i=0; i<QT_COUNT && n<cap; i++)
        if(cfg_get_int(QTILES[i].cfg, QTILES[i].def_on)) ids[n++] = i;
    return n;
}

/* is a toggle tile currently "on"? action tiles are never lit. */
static int qtile_is_on(int id){
    switch(id){
        case QT_WIFI:    return cfg_get_int("wifi_on", 1);
        case QT_BT:      return bt_radio_on();   /* actual radio state, not just the persisted intent */
        case QT_EQ:      return cfg_get_int("eq_preset", 0) > 0;
        case QT_SHUFFLE: return cfg_get_int("work_mode", 0) == 1;
        case QT_GAPLESS: return cfg_get_int("gapless", 0) == 1;
        case QT_GAIN:    return cfg_get_int("audio_gain", 0) == 1;
        case QT_DRE:     return cfg_get_int("audio_dre", 1) == 1;
        case QT_FAV:   { track_state_t st; ipc_get_state(&st); return st.path[0] && st.is_favorite; }
        default:         return 0;   /* action tiles (Search/Rescan/Source/Screen off/Lyrics/Timer/RG) never lit */
    }
}

static void tile_recolor(int id, int on){
    if(g_tile_dot[id]) lv_obj_set_style_bg_color(g_tile_dot[id], on ? ui_current_accent() : lv_color_hex(QS_OFF), 0);
}

/* ---- EQ A/B toggle (drawer): flip Off <-> the last-used preset, all persisted centrally --------- */
static void eq_toggle(void){
    int cur = cfg_get_int("eq_preset", 0); if(cur < 0 || cur > 20) cur = 0;
    if(cur > 0){                                  /* on -> off; remember what was on only if the send took */
        if(ui_eq_select(0) == 0){ cfg_set_int("eq_last", cur); tile_recolor(QT_EQ, 0); ui_toast("EQ off"); }
        else ui_toast("Couldn't apply EQ");
    }else{                                        /* off -> on; restore last (Rock the first time) */
        int last = cfg_get_int("eq_last", 0);
        if(last <= 0 || last > 20) last = 2;      /* 2 = Rock: an audible default before any is chosen */
        if(ui_eq_select(last) == 0){ tile_recolor(QT_EQ, 1); char m[40]; snprintf(m, sizeof m, "EQ: %s", ui_eq_name(last)); ui_toast(m); }
        else ui_toast("Couldn't apply EQ");
    }
}

/* ---- unified tile handlers (tile id in user_data) ---------------------------------------------- */
static void tile_short_cb(lv_event_t *e){
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch(id){
        case QT_WIFI:      { int on = wifi_toggle(); tile_recolor(QT_WIFI, on); ui_toast(on ? "Turning on Wi-Fi..." : "Wi-Fi off"); } break;
        case QT_BT:        { int on = bt_toggle();   tile_recolor(QT_BT,   on); ui_toast(on ? "Turning on Bluetooth..." : "Bluetooth off"); } break;
        case QT_EQ:        eq_toggle(); break;
        case QT_SEARCH:    screen_show(SCR_SEARCH); break;
        case QT_RESCAN:    ui_rescan_library(); ui_toast("Rescanning library..."); break;
        case QT_MODE:      modes_open(); break;
        case QT_SCREENOFF: ui_request_sleep(); screen_show(SCR_SAVER); break;
        case QT_SHUFFLE: {
            if(ui_book_active()){ ui_toast("Not available in books"); break; }   /* books stay in Single */
            int on = cfg_get_int("work_mode", 0) == 1 ? 0 : 1;                    /* Shuffle <-> Sequential */
            cfg_set_int("work_mode", on); ui_set_workmode(on);
            tile_recolor(QT_SHUFFLE, on); ui_toast(on ? "Shuffle on" : "Shuffle off");
        } break;
        case QT_FAV: {
            track_state_t st; ipc_get_state(&st);
            if(!st.path[0] || ui_book_active()){ ui_toast("No track playing"); break; }
            int nv = !st.is_favorite; ui_set_favorite(nv);
            tile_recolor(QT_FAV, nv); ui_toast(nv ? "Loved" : "Unloved");
        } break;
        case QT_LYRICS: lyrics_open(); break;
        case QT_TIMER:  settings_open_key("sleep_idx"); break;      /* opens the Sleep Timer chooser */
        case QT_GAPLESS: {
            int on = cfg_get_int("gapless", 0) ? 0 : 1;
            cfg_set_int("gapless", on); ui_set_gapless(on);
            tile_recolor(QT_GAPLESS, on); ui_toast(on ? "Gapless on" : "Gapless off");
        } break;
        case QT_GAIN: {
            int on = cfg_get_int("audio_gain", 0) ? 0 : 1;
            cfg_set_int("audio_gain", on); ui_set_gain(on);
            tile_recolor(QT_GAIN, on); ui_toast(on ? "High gain" : "Low gain");
        } break;
        case QT_DRE: {
            int on = cfg_get_int("audio_dre", 1) ? 0 : 1;
            cfg_set_int("audio_dre", on); ui_set_dre(on);
            tile_recolor(QT_DRE, on); ui_toast(on ? "DRE on" : "DRE off");
        } break;
        case QT_RG:     settings_open_key("replay_gain"); break;    /* opens the ReplayGain chooser */
    }
}
static void tile_long_cb(lv_event_t *e){
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    switch(id){
        case QT_WIFI: wifi_open(); break;
        case QT_BT:   bt_open();   break;
        case QT_EQ:   screen_show(SCR_EQ); break;
        case QT_SHUFFLE: settings_open_key("work_mode");  break;   /* full Play Mode chooser */
        case QT_GAPLESS: settings_open_key("gapless");    break;
        case QT_GAIN:    settings_open_key("audio_gain"); break;
        case QT_DRE:     settings_open_key("audio_dre");  break;
        default: break;
    }
}

/* one circular tile + caption, parent = root, centre at (x, y_top). Records its circle for refresh. */
static void build_tile(lv_obj_t *root, int id, int x, int y){
    const qtile_t *t = &QTILES[id];
    lv_obj_t *c = lv_button_create(root);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 58, 58);
    lv_obj_align(c, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, qtile_is_on(id) ? ui_current_accent() : lv_color_hex(QS_OFF), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(c, LV_OPA_80, LV_STATE_PRESSED);   /* gentle press dim on either state colour */
    lv_obj_set_ext_click_area(c, 4);
    if(t->has_long){
        /* SHORT_CLICKED so a long press fires ONLY the long action, never the toggle too */
        lv_obj_add_event_cb(c, tile_short_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)id);
        lv_obj_add_event_cb(c, tile_long_cb,  LV_EVENT_LONG_PRESSED,  (void*)(intptr_t)id);
    }else{
        lv_obj_add_event_cb(c, tile_short_cb, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    }
    lv_obj_t *g = lv_label_create(c);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(g, t->glyph);
    lv_obj_set_style_text_font(g, t->font, 0);
    lv_obj_set_style_text_color(g, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(g);

    lv_obj_t *lb = lv_label_create(root);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(lb, t->cap);
    lv_obj_set_style_text_font(lb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(QS_TXT2), 0);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, x, y + 62);

    g_tile_dot[id] = c;
}

/* ---- optional transport row -------------------------------------------------------------------- */
static void cmd_cb(lv_event_t *e){
    const char *c = (const char*)lv_event_get_user_data(e); if(!c) return;
    ui_defer_sleep();   /* a transport tap changes state -> don't let the sleep check race a stale read */
    if(!strcmp(c,"0201000C0001") || !strcmp(c,"0201000C0002")){ ui_cancel_book_resume(); ui_disarm_book_eoc(); }  /* next/prev = explicit nav */
    ipc_send_cmd(c);
}
static lv_obj_t *tp_btn(lv_obj_t *root, const char *sym, const lv_font_t *font, int x, int y, int sz, void *cmd){
    lv_obj_t *b = lv_button_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, sz, sz);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(QS_CARD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(QS_PRESS), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cmd_cb, LV_EVENT_CLICKED, cmd);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    return l;
}
static void build_transport(lv_obj_t *root, int y){
    tp_btn(root, LV_SYMBOL_PREV, &lv_font_montserrat_22, -84, y + 5, 46, (void*)"0201000C0002");
    g_pp_glyph = tp_btn(root, LV_SYMBOL_PAUSE, &lv_font_montserrat_28, 0, y, 56, (void*)"0201000C0000");
    tp_btn(root, LV_SYMBOL_NEXT, &lv_font_montserrat_22, 84, y + 5, 46, (void*)"0201000C0001");
    if(g_pp_glyph) lv_label_set_text(g_pp_glyph, ui_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* ---- brightness bar ---------------------------------------------------------------------------- */
static void bright_change_cb(lv_event_t *e){ ui_backlight(lv_slider_get_value(lv_event_get_target(e))); }
static void bright_release_cb(lv_event_t *e){ ui_set_brightness(lv_slider_get_value(lv_event_get_target(e))); }
static void build_brightness(lv_obj_t *root, int y){
    g_bright = lv_slider_create(root);
    lv_obj_set_size(g_bright, 216, 34);
    lv_obj_set_ext_click_area(g_bright, 8);
    lv_obj_align(g_bright, LV_ALIGN_TOP_MID, 0, y);
    lv_slider_set_range(g_bright, 4, 40);
    lv_slider_set_value(g_bright, ui_get_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bright, lv_color_hex(QS_TRACK), LV_PART_MAIN);
    lv_obj_set_style_radius(g_bright, 17, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bright, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bright, 17, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_bright, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_bright, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(g_bright, bright_change_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(g_bright, bright_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_bright, bright_release_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_t *sun = lv_label_create(g_bright);
    lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(sun, WI_SUN);
    lv_obj_set_style_text_font(sun, &font_icons_28, 0);
    lv_obj_set_style_text_color(sun, lv_color_hex(QS_OFF), 0);
    lv_obj_align(sun, LV_ALIGN_LEFT_MID, 12, 0);
}

/* ---- build / refresh --------------------------------------------------------------------------- */
void quicksettings_create(lv_obj_t *root){ g_qs_root = root; }   /* defer to quicksettings_build on open */

/* Rebuild the whole panel from config. Called on every entry so customisation changes show at once. */
void quicksettings_build(void){
    if(!g_qs_root) return;
    lv_obj_clean(g_qs_root);
    g_bright = NULL; g_pp_glyph = NULL;
    for(int i=0;i<QT_COUNT;i++) g_tile_dot[i] = NULL;

    lv_obj_set_style_bg_color(g_qs_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_qs_root, LV_OPA_COVER, 0);

    /* grabber */
    lv_obj_t *grab = lv_obj_create(g_qs_root);
    lv_obj_remove_style_all(grab);
    lv_obj_clear_flag(grab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grab, 36, 5);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_set_style_bg_color(grab, lv_color_hex(QS_GRAB), 0);
    lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);

    int have_tr = cfg_get_int("qs_transport", 0) ? 1 : 0;
    int ids[QT_COUNT]; int nt = qs_enabled(ids, qs_cap());
    int rows = nt > 3 ? 2 : (nt > 0 ? 1 : 0);

    const int H_BR = 34, H_TR = 56, H_ROW = 80, GAP = 14;
    int nblocks = 1 + have_tr + rows;                 /* brightness is always present */
    int total = H_BR + (have_tr ? H_TR : 0) + rows*H_ROW + (nblocks > 1 ? (nblocks-1)*GAP : 0);
    int y = 185 - total/2;                            /* centre the stack a touch below the middle */
    if(y < 44) y = 44;                                /* never overlap the grab handle */

    build_brightness(g_qs_root, y);  y += H_BR + GAP;
    if(have_tr){ build_transport(g_qs_root, y); y += H_TR + GAP; }

    int placed = 0;
    for(int r=0; r<rows; r++){
        int k = (r == 0) ? (nt > 3 ? 3 : nt) : (nt - 3);   /* row 1 up to 3, row 2 the remainder */
        for(int i=0; i<k; i++){
            int x = (int)((i - (k-1)/2.0) * 84);           /* centre the row, 84px column pitch */
            build_tile(g_qs_root, ids[placed++], x, y);
        }
        y += H_ROW + GAP;
    }
}

/* light update while the panel is open (playstate ticks, radio/EQ state changes) - no rebuild */
void quicksettings_refresh(int playing){
    if(g_bright && !lv_obj_has_state(g_bright, LV_STATE_PRESSED))
        lv_slider_set_value(g_bright, ui_get_brightness(), LV_ANIM_OFF);
    if(g_pp_glyph) lv_label_set_text(g_pp_glyph, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    for(int i=0;i<QT_COUNT;i++)
        if(g_tile_dot[i]) tile_recolor(i, qtile_is_on(i));
}

/* ---- customisation screen (SCR_QSCONFIG): pick which tiles appear -------------------------------- */
static lv_obj_t *g_qscfg_root;

static void qscfg_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }
static void qscfg_sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    int on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    if(id < 0){ cfg_set_int("qs_transport", on); return; }   /* id -1 = the transport row */
    if(on){                                                  /* refuse to enable past the drawer's tile cap */
        int cap = qs_cap();
        int cnt = 0; for(int i=0;i<QT_COUNT;i++) if(cfg_get_int(QTILES[i].cfg, QTILES[i].def_on)) cnt++;
        if(cnt >= cap){                                     /* this tile is still off in cfg, so cnt excludes it */
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
            char m[32]; snprintf(m, sizeof m, "Up to %d tiles shown", cap); ui_toast(m);
            return;
        }
    }
    cfg_set_int(QTILES[id].cfg, on);
}
/* a labelled row with an LVGL switch on the right */
static void qscfg_row(lv_obj_t *list, const char *label, int id, int on){
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 288, 50);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(QS_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 48, 26);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    if(on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, ui_current_accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, qscfg_sw_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)id);
}

void qsconfig_create(lv_obj_t *root){ g_qscfg_root = root; }

void qsconfig_refresh(void){
    if(!g_qscfg_root) return;
    lv_obj_clean(g_qscfg_root);
    lv_obj_set_style_bg_color(g_qscfg_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_qscfg_root, LV_OPA_COVER, 0);
    ui_header_cb(g_qscfg_root, "Quick Settings", qscfg_back_cb);

    lv_obj_t *list = lv_obj_create(g_qscfg_root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 70);
    lv_obj_set_size(list, 300, 250);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 44, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    qscfg_row(list, "Playback controls", -1, cfg_get_int("qs_transport", 0));
    for(int i=0;i<QT_COUNT;i++)
        qscfg_row(list, QTILES[i].cap, i, cfg_get_int(QTILES[i].cfg, QTILES[i].def_on));

    lv_obj_t *hint = lv_label_create(list);
    lv_label_set_text(hint, "The drawer shows up to 6 tiles (5 with playback controls).");
    lv_obj_set_width(hint, 288);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(QS_TXT2), 0);
    lv_obj_set_style_pad_top(hint, 6, 0);
    lv_obj_set_style_pad_left(hint, 6, 0);
}
