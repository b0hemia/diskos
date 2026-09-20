/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Settings -> Last.fm (SCR_LASTFM): per-user setup + account authorization + status.
 *
 * Flow, all driven by the lastfm.c engine (this file is only the view):
 *   1. "Set up"   -> lastfm_setup_start(): a QR of the on-device web page appears; the
 *                    user scans it, pastes their own api_key + secret on their phone.
 *   2. creds land -> the engine auto-runs lastfm_auth_begin(); a QR of the last.fm
 *                    approve URL appears -> user taps Allow on their phone.
 *   3. connected  -> "Connected as <user>", a Scrobbling on/off switch, Disconnect,
 *                    and the pending (offline) scrobble count.
 * A 500ms timer rebuilds the body only when the state signature changes, and stops the
 * setup web server whenever we leave the screen. Round-screen-aware header (clear corners). */
#include "screens.h"
#include "lastfm.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *g_body;
static lv_timer_t *g_refresh;
static int g_sig = -12345;

static void go_back(lv_event_t *e){ (void)e; lastfm_setup_stop(); screen_back(); }   /* POP the stack like every other back button - screen_show(SCR_APPS) forward-pushed LASTFM, so a later Back resurfaced it (lastfm opened only from Apps, so back -> Apps) */


static lv_obj_t *body_label(const char *txt, const lv_font_t *font, uint32_t color){
    lv_obj_t *l = lv_label_create(g_body);
    lv_label_set_text(l, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 250);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}
static lv_obj_t *body_button(const char *txt, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(g_body);
    lv_obj_set_height(b, 44); lv_obj_set_width(b, 190);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xD51007), 0);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_obj_center(l);
    return b;
}
static void body_qr(const char *url){
    lv_obj_t *qr = lv_qrcode_create(g_body);
    lv_qrcode_set_size(qr, 156);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url, (uint32_t)strlen(url));
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
}

static void on_setup(lv_event_t *e){ (void)e; lastfm_setup_start(); g_sig=-1; }
static void on_connect(lv_event_t *e){ (void)e; lastfm_auth_begin(); g_sig=-1; }
static void on_disconnect(lv_event_t *e){ (void)e; lastfm_logout(); g_sig=-1; }
static void on_toggle(lv_event_t *e){
    lv_obj_t *sw = lv_event_get_target(e);
    lastfm_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void rebuild(void){
    if(!g_body) return;
    lv_obj_clean(g_body);
    int st = lastfm_auth_state();

    if(lastfm_connected()){
        char b[180]; snprintf(b, sizeof b, "Connected as\n%s", lastfm_username());
        body_label(b, &lv_font_montserrat_20, 0xFFFFFF);
        /* Scrobbling on/off */
        lv_obj_t *row = lv_obj_create(g_body);
        lv_obj_remove_style_all(row); lv_obj_set_size(row, 210, 40);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *rl = lv_label_create(row);
        lv_label_set_text(rl, "Scrobbling");
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(0xC7C7CC), 0);
        lv_obj_t *sw = lv_switch_create(row);
        if(lastfm_enabled()) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0xD51007), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
        int q = lastfm_queue_count();
        if(q > 0){ char qb[48]; snprintf(qb,sizeof qb,"%d scrobble%s queued", q, q==1?"":"s");
                   body_label(qb, &lv_font_montserrat_14, 0x8E8E93); }
        body_button("Disconnect", on_disconnect);
    }
    else if(st==LFM_AUTH_WAIT && lastfm_auth_url()[0]){
        body_label("Scan, then tap \"Allow\"\non last.fm", &lv_font_montserrat_18, 0xFFFFFF);
        body_qr(lastfm_auth_url());
        body_label("Waiting for approval...", &lv_font_montserrat_14, 0x8E8E93);
    }
    else if(st==LFM_AUTH_TOKEN){
        body_label("Connecting...", &lv_font_montserrat_20, 0xFFFFFF);
    }
    else if(lastfm_setup_url()[0]){
        body_label("Scan with your phone\nto enter your Last.fm key", &lv_font_montserrat_18, 0xFFFFFF);
        body_qr(lastfm_setup_url());
        body_label("Same Wi-Fi as this player", &lv_font_montserrat_14, 0x8E8E93);
    }
    else if(st==LFM_AUTH_ERR){
        body_label("Couldn't connect.\nTry again.", &lv_font_montserrat_18, 0xFFFFFF);
        body_button(lastfm_has_creds()?"Retry":"Set up", lastfm_has_creds()?on_connect:on_setup);
    }
    else if(lastfm_has_creds()){
        body_label("Authorize this player\non your Last.fm account", &lv_font_montserrat_18, 0xFFFFFF);
        body_button("Connect account", on_connect);
        body_button("Re-enter keys", on_setup);
    }
    else {
        body_label("Scrobble the songs you play\nto your Last.fm profile", &lv_font_montserrat_18, 0xFFFFFF);
        body_button("Set up", on_setup);
        body_label("Beta: connect flow not fully\ntested yet. Key setup uses your\nlocal Wi-Fi (plain HTTP).",
                   &lv_font_montserrat_14, 0x8E8E93);
    }
}

/* signature of everything the view depends on -> rebuild only when it changes */
static int state_sig(void){
    return lastfm_connected()*1000000 + lastfm_auth_state()*100000 + lastfm_has_creds()*10000
         + (lastfm_setup_url()[0]?1:0)*1000 + lastfm_enabled()*100 + lastfm_queue_count();
}
static void refresh_cb(lv_timer_t *t){
    (void)t;
    if(screen_current()!=SCR_LASTFM){ lastfm_setup_stop(); g_sig=-12345; return; }
    int s = state_sig();
    if(s != g_sig){ g_sig = s; rebuild(); }
}

void lastfm_create(lv_obj_t *root){
    ui_header_cb(root, "Last.fm (beta)", go_back);   /* shared header; go_back stops setup then pops */
    /* body: a centered vertical flex column in the clear middle band */
    g_body = lv_obj_create(root);
    lv_obj_remove_style_all(g_body);
    lv_obj_set_size(g_body, 300, 250);
    lv_obj_set_style_pad_bottom(g_body, 44, 0);   /* last row scrolls clear of the round bezel */
    lv_obj_align(g_body, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_flex_flow(g_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_body, 12, 0);
    lv_obj_set_scroll_dir(g_body, LV_DIR_VER);
    if(!g_refresh) g_refresh = lv_timer_create(refresh_cb, 500, NULL);
}
void lastfm_open(void){
    screen_show(SCR_LASTFM);
    g_sig = -12345; rebuild(); g_sig = state_sig();
}
