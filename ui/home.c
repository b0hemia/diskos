/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

LV_FONT_DECLARE(font_weather16)
static lv_font_t s_wfont;   /* montserrat_14 with the weather-icon font as fallback */





static lv_obj_t *g_clock;
static lv_obj_t *g_clock_sub;
static lv_obj_t *g_home_bg;      /* full-screen blurred album backdrop (matches Now Playing) */
static lv_obj_t *g_home_scrim;   /* dark overlay over the backdrop so text stays readable */
static lv_obj_t *g_status;       /* top status row: wifi / bt / battery */
static lv_obj_t *g_weather;      /* weather line under the date */
static lv_obj_t *g_status_arc;
static lv_obj_t *g_np_capsule;
static lv_obj_t *g_np_thumb;
static lv_obj_t *g_np_art_img;
static lv_obj_t *g_np_thumb_glyph;
static lv_obj_t *g_np_title;
static lv_obj_t *g_np_artist;
static lv_obj_t *g_np_state;
static home_settings_click_cb_t g_settings_cb;
static lv_color_t g_accent;

static void nav_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    screen_show((int)(uintptr_t)lv_event_get_user_data(e));
}

/* Tap the home weather glance -> open the full weather app (the natural glance->detail flow, so the
 * app no longer needs a buried Apps entry). No-op when the line is empty (weather off/not fetched). */
static void weather_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *t = g_weather ? lv_label_get_text(g_weather) : NULL;
    if (t && t[0]) weather_app_open();
}

static void settings_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (g_settings_cb) {
        g_settings_cb();
    } else {
        /* Requested screen enum has no settings screen yet. */
        screen_show(SCR_HOME);
    }
}

static void pp_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED){
        ui_defer_sleep();   /* a play/pause tap changes state -> don't let the sleep check race a stale read */
        ipc_send_cmd("0201000C0000");   /* play/pause toggle */
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static lv_obj_t *make_tile(lv_obj_t *parent, int x, int y, int w, const char *symbol,
                           const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 92);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *ic = make_label(btn, symbol, &lv_font_montserrat_24,
                              lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(ic, 0, 18);
    lv_obj_set_width(ic, w);

    lv_obj_t *title = make_label(btn, text, &lv_font_montserrat_16,
                                 lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(title, 0, 54);
    lv_obj_set_width(title, w);

    return btn;
}

/* A wide rounded "pill" button: icon + label, left-aligned, vertically centered. */
static lv_obj_t *make_pill(lv_obj_t *parent, int x, int y, int w, int h,
                           const char *symbol, const char *text,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, h / 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *ic = lv_label_create(btn);
    lv_label_set_text(ic, symbol);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *title = lv_label_create(btn);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 76, 0);
    return btn;
}

void home_set_settings_click_cb(home_settings_click_cb_t cb)
{
    g_settings_cb = cb;
}

void home_set_clock(const char *time_text, const char *sub_text)
{
    if (g_clock) lv_label_set_text(g_clock, time_text ? time_text : "--:--");
    if (g_clock_sub) lv_label_set_text(g_clock_sub, sub_text ? sub_text : "");
}

void home_set_weather(const char *text)
{
    if (g_weather) lv_label_set_text(g_weather, text ? text : "");
}

/* Update the top status row. batt 0-100, charging/wifi/bt are booleans. */
void home_set_status(int batt, int charging, int wifi, int bt)
{
    if (!g_status) return;
    char buf[80]; char *p = buf; *p = 0;
    if (wifi) { p += sprintf(p, LV_SYMBOL_WIFI "  "); }
    if (bt)   { p += sprintf(p, LV_SYMBOL_BLUETOOTH "  "); }
    const char *bs = batt >= 90 ? LV_SYMBOL_BATTERY_FULL :
                     batt >= 65 ? LV_SYMBOL_BATTERY_3 :
                     batt >= 40 ? LV_SYMBOL_BATTERY_2 :
                     batt >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    /* while charging show just the bolt + % (the % gives the level); a bolt jammed against the
     * battery glyph looked mashed. Not charging -> the battery-level glyph. */
    if (batt >= 0) p += sprintf(p, "%s %d%%", charging ? LV_SYMBOL_CHARGE : bs, batt);
    lv_label_set_text(g_status, buf);
}

void home_set_now_playing(const char *title, const char *artist,
                          lv_color_t accent, bool playing)
{
    g_accent = accent;

    if (g_np_title) lv_label_set_text(g_np_title, title ? title : "Not Playing");
    if (g_np_artist) lv_label_set_text(g_np_artist, artist ? artist : "Library");
    if (g_np_state) lv_label_set_text(g_np_state, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (g_np_thumb) lv_obj_set_style_bg_color(g_np_thumb, accent, 0);
    if (g_status_arc) lv_obj_set_style_arc_color(g_status_arc, accent, LV_PART_INDICATOR);
}

/* repaint the now-playing capsule with the live accent (called from ui.c apply_accent so
 * Home tracks the same colour as Now Playing - static or album-dynamic). */
void home_set_accent(lv_color_t accent)
{
    g_accent = accent;
    if (g_np_thumb) lv_obj_set_style_bg_color(g_np_thumb, accent, 0);
    if (g_status_arc) lv_obj_set_style_arc_color(g_status_arc, accent, LV_PART_INDICATOR);
}

/* full-screen blurred backdrop (the 360px gblur'd cover), or NULL to clear -> black */
void home_set_backdrop(const char *src)
{
    if (!g_home_bg || !g_home_scrim) return;
    if (src) {
        lv_image_set_src(g_home_bg, src);
        lv_obj_clear_flag(g_home_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_home_scrim, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_home_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_home_scrim, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_set_art_src(const void *src)
{
    if (!g_np_art_img || !g_np_thumb_glyph) return;

    if (src) {
        /* src is a native-size 42px thumb (decoded by ui.c) - display 1:1, no
         * runtime scaling. The 42px circular thumb clips it to a disc. */
        lv_image_set_src(g_np_art_img, src);
        lv_obj_center(g_np_art_img);
        lv_obj_clear_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_np_thumb_glyph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_np_thumb_glyph, LV_OBJ_FLAG_HIDDEN);
    }
}

void home_create(lv_obj_t *root)
{
    g_accent = lv_color_hex(0xF23260);

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* full-screen blurred album backdrop (same image Now Playing uses), behind everything;
     * a dark scrim over it keeps the clock/text readable. Both created first = lowest z. */
    g_home_bg = lv_image_create(root);
    lv_obj_remove_style_all(g_home_bg);
    lv_obj_set_pos(g_home_bg, 0, 0); lv_obj_set_size(g_home_bg, 360, 360);
    lv_obj_clear_flag(g_home_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_home_bg, LV_OBJ_FLAG_HIDDEN);
    g_home_scrim = lv_obj_create(root);
    lv_obj_remove_style_all(g_home_scrim);
    lv_obj_set_pos(g_home_scrim, 0, 0); lv_obj_set_size(g_home_scrim, 360, 360);
    lv_obj_set_style_bg_color(g_home_scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_home_scrim, LV_OPA_50, 0);
    lv_obj_clear_flag(g_home_scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_home_scrim, LV_OBJ_FLAG_HIDDEN);

    g_status_arc = NULL;   /* decorative status arc removed (read as a stray progress line) */

    /* Top status row: wifi / bt / battery (populated by main.c poll). */
    g_status = make_label(root, "", &lv_font_montserrat_14, lv_color_hex(0xC7C7CC));
    lv_obj_set_pos(g_status, 0, 18);
    lv_obj_set_width(g_status, 360);

    g_clock = make_label(root, "--:--", &lv_font_montserrat_28,
                         lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(g_clock, 0, 52);
    lv_obj_set_width(g_clock, 360);

    g_clock_sub = make_label(root, "diskOS", &lv_font_montserrat_14,
                             lv_color_hex(0x8E8E93));
    lv_obj_set_pos(g_clock_sub, 0, 88);
    lv_obj_set_width(g_clock_sub, 360);

    /* Weather line (populated by weather.c via wttr.in). Uses montserrat_14 with
     * the FA weather-icon font chained as fallback so the icon glyph renders. */
    s_wfont = lv_font_montserrat_14;
    s_wfont.fallback = &font_weather16;
    g_weather = make_label(root, "", &s_wfont, lv_color_hex(0xC7C7CC));
    lv_obj_set_pos(g_weather, 0, 120);
    lv_obj_set_width(g_weather, 360);
    lv_label_set_long_mode(g_weather, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(g_weather, LV_OBJ_FLAG_CLICKABLE);       /* glance -> tap opens the weather app */
    lv_obj_set_ext_click_area(g_weather, 12);                /* thin line: enlarge the touch target */
    lv_obj_add_event_cb(g_weather, weather_event_cb, LV_EVENT_CLICKED, NULL);

    /* search button (top-right) with a drawn magnifier glyph */
    lv_obj_t *sbtn = lv_button_create(root);
    lv_obj_remove_style_all(sbtn);
    lv_obj_set_pos(sbtn, 244, 40);         /* nudged in from the round top-right bezel */
    lv_obj_set_size(sbtn, 44, 44);
    lv_obj_set_ext_click_area(sbtn, 8);
    lv_obj_set_style_radius(sbtn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sbtn, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(sbtn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(sbtn, nav_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)SCR_SEARCH);
    lv_obj_t *ring = lv_obj_create(sbtn);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 16, 16);
    lv_obj_set_pos(ring, 11, 9);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xC7C7CC), 0);
    lv_obj_t *handle = lv_obj_create(sbtn);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 8, 2);
    lv_obj_set_pos(handle, 25, 25);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0xC7C7CC), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_rotation(handle, 450, 0);

    /* Library is the primary action: a full-width pill. Apps + Settings live on
     * the swipe-left panel (SCR_APPS). */
    make_pill(root, 80, 162, 200, 52, LV_SYMBOL_DIRECTORY, "Library",
              nav_event_cb, (void *)(uintptr_t)SCR_LIBRARY);

    /* subtle hint that swiping left reveals more */
    lv_obj_t *hint = make_label(root, LV_SYMBOL_RIGHT, &lv_font_montserrat_14,
                                lv_color_hex(0x48484A));
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -6, 0);

    g_np_capsule = lv_button_create(root);
    lv_obj_remove_style_all(g_np_capsule);
    lv_obj_set_pos(g_np_capsule, 48, 246);
    lv_obj_set_size(g_np_capsule, 264, 58);
    lv_obj_set_style_radius(g_np_capsule, 29, 0);
    lv_obj_set_style_bg_color(g_np_capsule, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(g_np_capsule, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(g_np_capsule, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(g_np_capsule, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(g_np_capsule, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(g_np_capsule, LV_OPA_10, 0);
    lv_obj_set_style_border_width(g_np_capsule, 1, 0);
    lv_obj_clear_flag(g_np_capsule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_np_capsule, nav_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)SCR_NOWPLAYING);

    g_np_thumb = lv_obj_create(g_np_capsule);
    lv_obj_remove_style_all(g_np_thumb);
    lv_obj_set_pos(g_np_thumb, 9, 8);
    lv_obj_set_size(g_np_thumb, 42, 42);
    lv_obj_set_style_radius(g_np_thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(g_np_thumb, true, 0);
    lv_obj_set_style_bg_color(g_np_thumb, g_accent, 0);
    lv_obj_set_style_bg_opa(g_np_thumb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_np_thumb, LV_OBJ_FLAG_SCROLLABLE);

    g_np_art_img = lv_image_create(g_np_thumb);
    lv_obj_set_size(g_np_art_img, 42, 42);
    lv_obj_center(g_np_art_img);
    lv_obj_add_flag(g_np_art_img, LV_OBJ_FLAG_HIDDEN);

    g_np_thumb_glyph = make_label(g_np_thumb, LV_SYMBOL_AUDIO, &lv_font_montserrat_20,
                                  lv_color_hex(0xFFFFFF));
    lv_obj_center(g_np_thumb_glyph);

    g_np_title = lv_label_create(g_np_capsule);
    lv_label_set_text(g_np_title, "Not Playing");
    lv_label_set_long_mode(g_np_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_np_title, 62, 10);
    lv_obj_set_size(g_np_title, 148, 20);
    lv_obj_set_style_text_font(g_np_title, ui_font_cjk(16), 0);   /* CJK/long titles render (was tofu) */
    lv_obj_set_style_text_color(g_np_title, lv_color_hex(0xFFFFFF), 0);

    g_np_artist = lv_label_create(g_np_capsule);
    lv_label_set_text(g_np_artist, "Library");
    lv_label_set_long_mode(g_np_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_np_artist, 62, 31);
    lv_obj_set_size(g_np_artist, 148, 18);
    lv_obj_set_style_text_font(g_np_artist, ui_font_cjk(14), 0);   /* CJK artist names render */
    lv_obj_set_style_text_color(g_np_artist, lv_color_hex(0xC7C7CC), 0);

    lv_obj_t *pp_btn = lv_button_create(g_np_capsule);
    lv_obj_remove_style_all(pp_btn);
    lv_obj_set_pos(pp_btn, 208, 7);
    lv_obj_set_size(pp_btn, 48, 44);
    lv_obj_set_style_radius(pp_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pp_btn, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pp_btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(pp_btn, pp_event_cb, LV_EVENT_CLICKED, NULL);

    g_np_state = make_label(pp_btn, LV_SYMBOL_PLAY, &lv_font_montserrat_20,
                            lv_color_hex(0xFFFFFF));
    lv_obj_center(g_np_state);
}

