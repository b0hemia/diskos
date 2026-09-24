/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "ui.h"
#include "lvgl/lvgl.h"
#include "art.h"
#include "ipc.h"
#include "musicdb.h"   /* persistent per-song accent cache */
#include "scanner.h"   /* scan_read_chapters: Now Playing shows a book's current chapter */
#include "artcache.h"   /* persistent decoded-cover cache on SD */
#include "config.h"
#include "screens.h"
#include "sdio.h"
#include "anim.h"
#include "fonts_intl.h"   /* Cyrillic/Greek/Latin-ext fallback (issue #3) */

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>

#define C_BLACK      0x000000
#define C_LINE       0x1C1C1E
#define C_LINE_SOFT  0x2C2C2E
#define C_WHITE      0xFFFFFF
#define C_SECONDARY  0xC7C7CC
#define C_TERTIARY   0x8E8E93

LV_FONT_DECLARE(font_icons_28)
#define HEART_FILLED  "\xEF\x80\x84"   /* FA f004 solid heart */
#define HEART_OUTLINE "\xEF\x82\x8A"   /* FA f08a outline heart */
#define MOON_ICON     "\xEF\x86\x86"   /* FA f186 moon (audiobook sleep timer) */
#define MODE_ARROW    "\xEF\x85\xB8"   /* FA f178 long-arrow-right (sequential) */
#define C_ACCENT     0xFF375F

#define ARC_D        332
#define ARC_SWEEP    286
#define ARC_ROT      127

#define COVER_D      148

static lv_obj_t *backdrop;
static lv_obj_t *ring;
static lv_obj_t *cover;
static lv_obj_t *cover_img;
static lv_obj_t *cover_note;
static lv_obj_t *spindle;       /* vinyl-style centre label (hidden in Cover mode) */
/* full-screen album-art view - reuses the stock player's /usr/data/fiio/cover.png (364px sharp,
 * incl. its online-fetched art). Tap the cover to open, tap again to close. Overlay on the NP root. */
static lv_obj_t *fsart, *fsart_img, *fsart_title, *fsart_artist;
static int fsart_on;
static lv_obj_t *title;
static lv_obj_t *artist;
static lv_obj_t *album;   /* also hosts the track position: "N / M" or "Album · N/M" */
static lv_obj_t *btn_pp;
static lv_obj_t *btn_prev;
static lv_obj_t *btn_next;
static lv_obj_t *btn_fav;
static lv_obj_t *fav_icon;
static lv_obj_t *btn_sleep;    /* audiobook sleep-timer moon (occupies the heart slot for books) */
static lv_obj_t *sleep_icon;
static int g_np_fav, g_np_have, g_fav_px, g_fav_py;
/* Optimistic-favourite hold: a heart tap flips the widget + sends 0104 before the player
 * confirms via a2. Position (a1) frames trigger a full NP refresh from the OLD is_favorite,
 * which would flip the heart back. We hold the tapped value for this exact track until the
 * player confirms it, the track changes, or a timeout - so the heart doesn't visibly bounce. */
static char g_np_curpath[520];
static int  g_favp_active = 0, g_favp_val = 0;
static uint32_t g_favp_set = 0;
static char g_favp_path[520];
static lv_obj_t *btn_mode;
static lv_obj_t *mode_icon;
static lv_obj_t *mode_one;
static int g_np_mode = -1, g_mode_px, g_mode_py;
static lv_obj_t *t_elapsed;
static lv_obj_t *t_remain;

static lv_color_t accent;
static char last_path[256];
static char g_stat_path[256];   /* last track counted in play history (count once per new track) */
/* Art cache key = album title + the track's PARENT DIRECTORY (not artist). This reuses art
 * within a real album AND within a compilation/various-artists album (same folder), but does
 * NOT reuse across two genuinely-different albums that merely share a title in different
 * folders. For a flat library it degrades to album-only - no worse than before. */
static char last_art_key[420];  /* key of the art currently shown */
static char want_art_key[420];  /* key the CURRENT track wants; a finished decode applies only if it matches */
static char last_seed_a[160];
static char last_seed_b[160];
static char cover_src[48];
static int  cover_valid;     /* 1 when cover_src points at a freshly decoded cover */
static int  coverdsc_valid;  /* 1 only when g_coverdsc holds THIS track's RAM decode (else stale) */
static char thumb_src[48]; /* tiny 42px thumb for the Home pill */
static int  thumb_valid;
static char backdrop_src[48]; /* full-screen blurred backdrop */
static int  backdrop_valid;
static int displayed_idx;   /* art buffer (0/1) currently shown; new decodes target ^1 */
static int32_t shown_progress;
static int  g_scrubbing;     /* finger on the seek arc - don't fight it */
static long g_track_dur;     /* current track duration (ms) for seek math */
static long g_seek_lo, g_seek_hi;  /* the ms window the ring spans + seeks within: a chapter for a book, the whole track otherwise */
/* After releasing a seek, the player keeps streaming the OLD position for a
 * beat before it processes the jump, which makes the arc snap back then jump
 * forward. Hold the display at the seeked target and ignore stale echoes until
 * the player's stream reaches it (or the window lapses). */
static uint32_t g_seek_hold_until = 0;
static long     g_seek_target_ms = -1;   /* ABSOLUTE ms we seeked to - window-independent, so a seek that lands on a chapter boundary (window flips) isn't mistaken for a stale echo */
static long     g_scrub_lo, g_scrub_hi;  /* the ms window LATCHED when a ring drag starts - playback advancing under the finger (a chapter boundary crossed mid-drag) must not reinterpret the arc */
static char     g_scrub_path[520];       /* the track the drag STARTED on - if the track changes before release, the release seek (computed from the old track's window) must NOT be applied to the new track */
/* A seek and a back/hub swipe can both start anywhere on the ring, so we tell
 * them apart by DIRECTION: a seek follows the ring (curved / has a vertical
 * component) while back/hub is a long, straight, horizontal slide. Once a drag
 * looks horizontal we freeze the bar (no seek preview) and let the main loop
 * navigate; otherwise the ring scrubs as normal. */
static int g_seek_sx, g_seek_sy;   /* seek gesture press origin */
static int g_seek_cand;            /* press landed on the ring band (seek candidate) */
static int g_seek_on;              /* seek confirmed & actively scrubbing */

static lv_color_t accent_from(const char *a, const char *b)
{
    unsigned h = 2166136261u;
    for(const char *p = a; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    for(const char *p = b; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return lv_color_hsv_to_rgb(h % 360, 65, 84);   /* no-art fallback: forced good S/V */
}

static void mmss(long ms, char *buf, size_t len)
{
    if(ms < 0) ms = 0;
    long total = ms / 1000;
    long sec = total % 60;
    long hr  = total / 3600;
    if(hr > 0){                                  /* audiobook-length: H:MM:SS (music tracks stay M:SS) */
        long min = (total / 60) % 60;
        if(hr > 999) hr = 999;
        snprintf(buf, len, "%ld:%02ld:%02ld", hr, min, sec);
    } else {
        snprintf(buf, len, "%ld:%02ld", total / 60, sec);
    }
}

static void copy_cstr(char *dst, size_t dst_len, const char *src)
{
    if(dst_len == 0) return;
    if(src == NULL) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static void set_label_text_changed(lv_obj_t *obj, const char *txt)
{
    const char *cur;

    if(obj == NULL) return;
    if(txt == NULL) txt = "";

    cur = lv_label_get_text(obj);
    if(cur == NULL || strcmp(cur, txt) != 0) {
        lv_label_set_text(obj, txt);
    }
}

/* ---- Now Playing seek recognizer ---------------------------------------
 * The ring is DISPLAY-ONLY (not clickable); main.c feeds raw touch into these.
 * A seek is recognized only when the press lands on the ring band within the
 * progress sweep (and below the top drawer zone) AND the drag follows the ring
 * rather than being a straight horizontal nav slide -- so nav swipes and the
 * top-drawer pull never move the arc. "Classify first, move the arc second." */
#define RING_CX  180
#define RING_CY  180
#define RING_R   (ARC_D / 2)

static int seek_on_band(int x, int y, int tol){
    int dx = x - RING_CX, dy = y - RING_CY;
    int d2 = dx*dx + dy*dy;
    int lo = RING_R - tol, hi = RING_R + tol;
    return d2 >= lo*lo && d2 <= hi*hi;
}
static double seek_rel_angle(int x, int y){
    double a = atan2((double)(y - RING_CY), (double)(x - RING_CX)) * 57.2957795;
    double rel = a - ARC_ROT;
    while(rel < 0) rel += 360.0;
    while(rel >= 360.0) rel -= 360.0;
    return rel;   /* degrees from the start of the sweep, clockwise */
}
static int32_t seek_pt_to_value(int x, int y){
    double rel = seek_rel_angle(x, y);
    if(rel > ARC_SWEEP) rel = (rel - ARC_SWEEP < 360.0 - rel) ? ARC_SWEEP : 0.0;
    int32_t v = (int32_t)(rel / ARC_SWEEP * 1000.0 + 0.5);
    if(v < 0) v = 0;
    if(v > 1000) v = 1000;
    return v;
}

/* press: returns 1 if this touch could be a seek (and arms the recognizer) */
int ui_np_seek_press(int x, int y){
    g_seek_cand = 0; g_seek_on = 0; g_scrubbing = 0;   /* also clears a stuck scrub from a missed release */
    if(y < 40) return 0;                          /* top drawer zone */
    if(g_track_dur <= 0) return 0;                /* nothing to seek */
    if(!seek_on_band(x, y, 34)) return 0;         /* not on the ring (grab band) */
    if(seek_rel_angle(x, y) > ARC_SWEEP) return 0;/* in the bottom gap (transport buttons), not the arc */
    g_seek_sx = x; g_seek_sy = y; g_seek_cand = 1;
    return 1;
}
/* move: returns 1 while a seek owns the gesture (so main.c skips navigation) */
int ui_np_seek_move(int x, int y){
    if(!g_seek_cand) return 0;
    int dx = x - g_seek_sx, dy = y - g_seek_sy;
    int adx = dx<0?-dx:dx, ady = dy<0?-dy:dy;
    if(!seek_on_band(x, y, 48)){                   /* wandered off the ring -> a nav swipe */
        if(g_seek_on){ g_seek_on = 0; g_scrubbing = 0; lv_arc_set_value(ring, shown_progress); }
        g_seek_cand = 0; return 0;
    }
    if(!g_seek_on){
        if(adx < 26 && ady < 26) return 0;            /* not enough travel to classify; hold the arc */
        if(adx > ady*2){ g_seek_cand = 0; return 0; } /* straight horizontal -> nav swipe (top-of-arc is ambiguous; bias to no-glitch) */
        g_seek_on = 1; g_scrubbing = 1;               /* confirmed: a deliberate ring drag */
        g_scrub_lo = g_seek_lo; g_scrub_hi = g_seek_hi;   /* latch the window NOW so it can't shift under the finger */
        if(g_scrub_hi <= g_scrub_lo){ g_scrub_lo = 0; g_scrub_hi = g_track_dur; }
        copy_cstr(g_scrub_path, sizeof g_scrub_path, g_np_curpath);   /* latch WHICH track this drag is on */
    }
    int32_t v = seek_pt_to_value(x, y);
    lv_arc_set_value(ring, v);
    shown_progress = v;   /* keep the render cache in sync with the direct arc write, else set_progress_changed() can skip a needed reset (e.g. drag to a chapter end -> next chapter's progress 0 == a stale cached 0 -> arc stuck at 100%) */
    {
        long lo = g_scrub_lo, hi = g_scrub_hi;   /* latched window (music: whole track) */
        if(hi > lo){
            long span = hi - lo;
            long rel  = (long)((int64_t)v * span / 1000);   /* preview is window-relative (chapter for a book) */
            char b[12]; mmss(rel, b, sizeof b); set_label_text_changed(t_elapsed, b);
            char r[12]; mmss(span - rel, r, sizeof r);
            char rr[14]; snprintf(rr, sizeof rr, "-%s", r); set_label_text_changed(t_remain, rr);
        }
    }
    return 1;
}
/* release: commit the seek if one was active. returns 1 if it consumed the gesture */
int ui_np_seek_release(int x, int y){
    (void)x; (void)y;
    int consumed = 0;
    /* If the track changed while the finger was down, the drag was computed against the OLD track's
     * window - applying it to the NEW track would scramble its position and cancel its resume. Compare
     * against a FRESH player-state read (g_np_curpath is only a per-tick UI snapshot and can lag the
     * real track). Drop the seek on a mismatch. */
    if(g_seek_on && g_scrub_path[0]){
        track_state_t s; ipc_get_state(&s);
        if(strcmp(g_scrub_path, s.path) != 0){
            g_seek_cand = 0; g_seek_on = 0; g_scrubbing = 0;
            lv_arc_set_value(ring, shown_progress);   /* snap the arc back to the real position */
            return 1;                                 /* consumed the gesture, but issued NO seek */
        }
    }
    if(g_seek_on && g_track_dur > 0){
        int32_t v = lv_arc_get_value(ring);
        long lo = g_scrub_lo, hi = g_scrub_hi;   /* the window latched at drag start */
        if(hi <= lo){ lo = 0; hi = g_track_dur; }   /* fall back to the whole track */
        long ms = (hi > lo) ? lo + (long)((int64_t)v * (hi - lo) / 1000) : 0;   /* latched window-relative v -> absolute ms */
        g_seek_target_ms = ms;
        g_seek_hold_until = lv_tick_get() + 2500;   /* suppress stale echo ~2.5s */
        /* only hand control away from a pending book-resume if the seek actually goes out; a failed
         * send must leave the resume pending so the saved position isn't lost. */
        if(ui_seek_to(ms) == 0) ui_book_user_seeked(ms);
        consumed = 1;
    }
    g_seek_cand = 0; g_seek_on = 0; g_scrubbing = 0;
    return consumed;
}

static void set_progress_changed(int32_t value)
{
    if(g_scrubbing) return;   /* the finger owns the arc while scrubbing */
    if(value < 0) value = 0;
    if(value > 1000) value = 1000;

    if(ring && shown_progress != value) {
        lv_arc_set_value(ring, value);
        shown_progress = value;
    }
}

static void style_text(lv_obj_t *obj, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(obj, 0, LV_PART_MAIN);
}

int ui_transport_command(const char *cmd)
{
    if(!cmd) return -1;
    int is_next = !strcmp(cmd, "0201000C0001");
    int is_prev = !strcmp(cmd, "0201000C0002");
    int is_toggle = !strcmp(cmd, "0201000C0000");
    if(!is_next && !is_prev && !is_toggle) return -1;
    if(!ui_local_playback_allowed()){
        ui_toast("Return to local playback first"); return -1;
    }
    ui_defer_sleep();
    if(is_next || is_prev){
        track_state_t st; ipc_get_state(&st);
        if(mdb_is_book_path(st.path)){
            long tgt = st.position_ms + (is_next ? 30000 : -15000);
            if(tgt < 0) tgt = 0;
            if(st.duration_ms > 0 && tgt > st.duration_ms) tgt = st.duration_ms;
            if(ui_seek_to(tgt) < 0){ ui_toast("Couldn't seek - try again"); return -1; }
            g_seek_target_ms = tgt;
            g_seek_hold_until = lv_tick_get() + 2500;
            ui_book_user_seeked(tgt);
            return 0;
        }
    }
    if(ipc_send_cmd(cmd) < 0){ ui_toast("Player busy - try again"); return -1; }
    if(is_toggle){
        ui_pp_tap_hint();
        set_label_text_changed(btn_pp, ui_pp_icon_playing(ui_is_playing()) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    } else {
        ui_cancel_book_resume();
        ui_disarm_book_eoc();
    }
    return 0;
}
static void transport_cb(lv_event_t *e)
{
    ui_transport_command((const char *)lv_event_get_user_data(e));
}

/* The cover opens Song Info on a TAP, but it is also where a horizontal
 * slide-to-the-side (-> Now Playing hub) often starts.  A clickable-but-not-
 * scrollable object still fires CLICKED after a slide, which used to open Song
 * Info instead of letting the swipe reach the hub.  So only treat it as a tap
 * when the finger barely moved; a slide is ignored here and handled by the
 * main-loop swipe gesture (-> SCR_NPHUB). */
static int g_cover_px, g_cover_py;
/* ---- full-screen album art (stock cover.png reuse) ---------------------------------------- */
static char fsart_path[256];
static void fsart_reload_img(void){
    if(!fsart_img) return;
    lv_image_set_src(fsart_img, NULL);                 /* cache off -> force a fresh decode */
    /* Use the stock player's cover.JPG (364px sharp) - LVGL's JPEG decoder (TJPGD) is enabled;
     * PNG (lodepng) is NOT, so cover.png can't be loaded. Fall back to our own BMP art. */
    if(access("/usr/data/fiio/cover.jpg", 0) == 0)
        lv_image_set_src(fsart_img, "A:/usr/data/fiio/cover.jpg");
    else if(backdrop_valid)
        lv_image_set_src(fsart_img, backdrop_src);     /* fallback: our own blurred backdrop (BMP) */
    else if(cover_valid)
        lv_image_set_src(fsart_img, cover_src);        /* last resort: the 148px cover (BMP) */
}
static void fsart_refresh_text(void){
    if(fsart_title)  lv_label_set_text(fsart_title,  lv_label_get_text(title));
    if(fsart_artist) lv_label_set_text(fsart_artist, lv_label_get_text(artist));
}
void ui_np_fsart_open(void){
    if(!fsart || fsart_on || !g_np_have) return;       /* nothing to show if no track */
    fsart_path[0] = '\0';
    fsart_reload_img();
    fsart_refresh_text();
    lv_obj_set_style_opa(fsart, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(fsart, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(fsart);
    anim_fade(fsart, LV_OPA_TRANSP, LV_OPA_COVER, 240, NULL);
    fsart_on = 1;
}
static void fsart_hidden_cb(lv_anim_t *a){ (void)a; if(fsart) lv_obj_add_flag(fsart, LV_OBJ_FLAG_HIDDEN); }
void ui_np_fsart_close(void){
    if(!fsart || !fsart_on) return;
    fsart_on = 0;
    anim_fade(fsart, LV_OPA_COVER, LV_OPA_TRANSP, 200, fsart_hidden_cb);
}
int ui_np_fsart_active(void){ return fsart_on; }
static void fsart_click_cb(lv_event_t *e){
    /* LVGL is the sole closer. The opening tap's CLICKED targets the cover (the press
     * target), not this overlay, so raising fsart mid-click can't retarget it here -
     * no timing guard needed. main.c only swallows the raw gesture; it never mutates fsart. */
    if(lv_event_get_code(e)==LV_EVENT_CLICKED) ui_np_fsart_close();
}

static void cover_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *id = lv_indev_active();
    if(code == LV_EVENT_PRESSED) {
        if(id){ lv_point_t p; lv_indev_get_point(id, &p); g_cover_px = p.x; g_cover_py = p.y; }
    }
    /* Cover-tap no longer opens full-screen art (removed per product decision - the
     * vinyl screensaver is the album-art showcase; full-screen art stays available via
     * Options -> Full-screen Art). The PRESSED tracking above + main.c's cover-tap
     * seek-skip remain so a tap on the cover can't accidentally seek. */
}
static void make_clickable(lv_obj_t *o, const char *cmd)
{
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(o, 18);
    lv_obj_add_event_cb(o, transport_cb, LV_EVENT_CLICKED, (void *)cmd);
}

static void apply_accent(void)
{
    if(ring) {
        lv_obj_set_style_arc_color(ring, accent, LV_PART_INDICATOR);
    }

    if(cover_note) {
        lv_obj_set_style_text_color(cover_note, lv_color_hex(C_WHITE), LV_PART_MAIN);
    }

    if(btn_pp) {
        lv_obj_set_style_text_color(btn_pp, accent, LV_PART_MAIN);
    }

    if(fav_icon && g_np_fav) {
        lv_obj_set_style_text_color(fav_icon, accent, LV_PART_MAIN);
    }

    if(mode_icon && g_np_mode > 0) {
        lv_obj_set_style_text_color(mode_icon, accent, LV_PART_MAIN);
    }

    if(spindle) {
        lv_obj_set_style_bg_color(spindle, accent, LV_PART_MAIN);
    }

    /* keep the other accent-bearing surfaces in lockstep so no screen shows a stale
     * (hardcoded-pink) accent: Home's now-playing capsule + the saver decorations. */
    home_set_accent(accent);
    saver_set_accent(accent);
}

/* the live accent (user-picked static, or the album-derived dynamic colour). Other
 * modules paint with this instead of a hardcoded constant. */
lv_color_t ui_current_accent(void){ return accent; }

/* ---- shared standard header (back chevron + centred title) ------------------------------------- */
static void ui_header_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }
/* full form: custom back handler (e.g. Library pops its view stack before leaving the screen). */
lv_obj_t *ui_header_cb(lv_obj_t *root, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *back = lv_button_create(root);
    lv_obj_remove_style_all(back);
    lv_obj_set_pos(back, 72, 24); lv_obj_set_size(back, 44, 40);   /* inset from the clipped corner */
    lv_obj_set_ext_click_area(back, 10);                            /* easier near the round bezel */
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x1C1C1E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, back_cb ? back_cb : ui_header_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = lv_label_create(back);
    lv_label_set_text(ic, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xC7C7CC), 0);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -2);

    lv_obj_t *t = lv_label_create(root);
    lv_label_set_text(t, title);
    /* Screen-centred (label centre = 104+76 = 180) but bounded to a zone that clears the back chevron on
     * both sides, so a long album/playlist name truncates with an ellipsis instead of sliding under it. */
    lv_obj_set_pos(t, 104, 30); lv_obj_set_size(t, 152, 26);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(t, ui_font_cjk(18), 0);   /* headers show folder/album/playlist names: chain (issue #3) */
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    return t;
}
lv_obj_t *ui_header(lv_obj_t *root, const char *title){ return ui_header_cb(root, title, NULL); }

/* Favorites heart on Now Playing: filled+accent when the current track is a
 * favorite, outline+grey otherwise. Tap toggles it (movement-guarded so a swipe
 * doesn't accidentally favorite). The 0104 command is sent via ui_set_favorite. */
static void fav_refresh(int on, int have)
{
    g_np_fav = on; g_np_have = have;
    if(!btn_fav || !fav_icon) return;
    if(have) lv_obj_remove_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);
    else     lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(fav_icon, on ? HEART_FILLED : HEART_OUTLINE);
    lv_obj_set_style_text_color(fav_icon, on ? accent : lv_color_hex(C_TERTIARY), LV_PART_MAIN);
}

static void fav_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *id = lv_indev_active();
    if(code == LV_EVENT_PRESSED) {
        if(id) { lv_point_t p; lv_indev_get_point(id, &p); g_fav_px = p.x; g_fav_py = p.y; }
    } else if(code == LV_EVENT_CLICKED) {
        if(!g_np_have) return;
        if(id) {
            lv_point_t p; lv_indev_get_point(id, &p);
            int dx = p.x - g_fav_px, dy = p.y - g_fav_py;
            if((dx<0?-dx:dx) > 18 || (dy<0?-dy:dy) > 18) return;   /* was a swipe (cst816t wobble tolerance) */
        }
        fav_refresh(!g_np_fav, 1);      /* g_np_fav now holds the NEW (toggled) value */
        ui_set_favorite(g_np_fav);
        /* hold this value for the current track until the player confirms via a2 */
        g_favp_active = 1; g_favp_val = g_np_fav; g_favp_set = lv_tick_get();
        snprintf(g_favp_path, sizeof g_favp_path, "%s", g_np_curpath);
        ui_toast(g_np_fav ? "Added to Favourites" : "Removed from Favourites");
    }
}

/* Play-mode toggle on Now Playing: cycles work_mode 0..4 via 0102 (ui_set_workmode).
 * Stock order (ground-truth captured 2026-06-25): 0=Sequential, 1=Shuffle,
 * 2=Repeat One, 3=Repeat All, 4=Single (play one, stop). Icons (existing glyphs):
 *   0 arrow         (grey/inactive base)
 *   1 shuffle
 *   2 loop + "1"
 *   3 loop
 *   4 arrow + "1"   (single = play-one)
 * The "1" overlay (mode_one) is shown for Repeat One and Single. */
#define WORKMODE_COUNT 5
static void mode_refresh(int wm)
{
    if(!btn_mode || !mode_icon) return;
    if(wm == g_np_mode) return;
    g_np_mode = wm;
    const char *icon = (wm == 1) ? LV_SYMBOL_SHUFFLE
                     : (wm == 2 || wm == 3) ? LV_SYMBOL_LOOP
                     : MODE_ARROW;                       /* 0 and 4 use the arrow */
    lv_label_set_text(mode_icon, icon);
    lv_obj_set_style_text_color(mode_icon, wm ? accent : lv_color_hex(C_TERTIARY), LV_PART_MAIN);
    if(wm == 2 || wm == 4) lv_obj_remove_flag(mode_one, LV_OBJ_FLAG_HIDDEN);
    else                   lv_obj_add_flag(mode_one, LV_OBJ_FLAG_HIDDEN);
}

static void mode_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *id = lv_indev_active();
    if(code == LV_EVENT_PRESSED) {
        if(id) { lv_point_t p; lv_indev_get_point(id, &p); g_mode_px = p.x; g_mode_py = p.y; }
    } else if(code == LV_EVENT_CLICKED) {
        if(id) {
            lv_point_t p; lv_indev_get_point(id, &p);
            int dx = p.x - g_mode_px, dy = p.y - g_mode_py;
            if((dx<0?-dx:dx) > 18 || (dy<0?-dy:dy) > 18) return;   /* was a swipe (cst816t wobble tolerance) */
        }
        int wm = (cfg_get_int("work_mode", 0) + 1) % WORKMODE_COUNT;
        cfg_set_int("work_mode", wm);
        ui_set_workmode(wm);
        mode_refresh(wm);
        static const char *const MODE_NAMES[WORKMODE_COUNT] =
            { "Sequential", "Shuffle", "Repeat One", "Repeat All", "Single" };
        if(wm >= 0 && wm < WORKMODE_COUNT) ui_toast(MODE_NAMES[wm]);
    }
}

static int g_np_vinyl = 0;
static int g_spinning  = 0;

static void spin_cb(void *var, int32_t v){ lv_image_set_rotation((lv_obj_t *)var, v % 3600); }

/* Start/stop the vinyl spin. Idempotent - main.c calls this every loop with the
 * live condition (vinyl style && playing && Now Playing visible && screen on),
 * so the disc only spins when you're actually watching it. On pause it FREEZES
 * at the current angle (no reset) and resumes from there. */
void ui_vinyl_spin(int want)
{
    want = want && g_np_vinyl && cover_img;
    if(want == g_spinning) return;
    g_spinning = want;
    if(want){
        int32_t cur = lv_image_get_rotation(cover_img);   /* resume from here */
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, cover_img);
        lv_anim_set_exec_cb(&a, spin_cb);
        lv_anim_set_values(&a, cur, cur + 3600);   /* +one turn; spin_cb wraps %3600 */
        lv_anim_set_time(&a, 9000);                /* ~9s/rev - relaxed */
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(cover_img, spin_cb);        /* freeze at current angle */
    }
}

/* Now Playing style: 0 = album cover (rounded square), 1 = vinyl disc. */
void ui_set_np_style(int vinyl)
{
    if(!cover) return;
    g_np_vinyl = vinyl;
    lv_obj_set_style_radius(cover, vinyl ? (COVER_D/2) : 14, LV_PART_MAIN);
    if(spindle) {
        if(vinyl) lv_obj_remove_flag(spindle, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(spindle, LV_OBJ_FLAG_HIDDEN);
    }
    if(!vinyl){
        ui_vinyl_spin(0);                                  /* stop the spin */
        if(cover_img) lv_image_set_rotation(cover_img, 0); /* Cover must sit upright */
    }
}

static void set_cover_fallback(bool show)
{
    if(show) {
        if(cover_img) lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        if(cover_note) lv_obj_remove_flag(cover_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        if(cover_img) lv_obj_remove_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
        if(cover_note) lv_obj_add_flag(cover_note, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Decode the 148px cover BMP into a RAM ARGB8888 descriptor. A file-sourced
 * lv_bmp image can't be rotated by the SW renderer (renders black) because the
 * image cache is off; a true-colour RAM buffer transforms fine, so the vinyl
 * can spin. The BMP is 24-bit BGR, bottom-up, 148*3=444 bytes/row (no padding). */
#define CBMP 148
static uint8_t g_coverbuf[CBMP*CBMP*4];
static lv_image_dsc_t g_coverdsc;
static int load_cover_dsc(const char *path)
{
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    uint8_t hdr[54];
    if(fread(hdr,1,54,f)!=54){ fclose(f); return -1; }
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((uint32_t)hdr[13]<<24);
    int w = hdr[18]|(hdr[19]<<8), h = hdr[22]|(hdr[23]<<8);
    int bpp = hdr[28]|(hdr[29]<<8);
    uint32_t comp = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|((uint32_t)hdr[33]<<24);
    if(w!=CBMP || h!=CBMP || bpp!=24 || comp!=0){ fclose(f); return -1; }
    static uint8_t row[CBMP*3];
    if(fseek(f, off, SEEK_SET)!=0){ fclose(f); return -1; }
    for(int yy=0; yy<CBMP; yy++){
        if(fread(row,1,CBMP*3,f)!=(size_t)(CBMP*3)){ fclose(f); return -1; }
        uint8_t *d = g_coverbuf + (CBMP-1-yy)*CBMP*4;   /* bottom-up -> top-down */
        for(int x=0;x<CBMP;x++){
            d[x*4+0]=row[x*3+0]; d[x*4+1]=row[x*3+1];   /* B, G */
            d[x*4+2]=row[x*3+2]; d[x*4+3]=0xFF;          /* R, A */
        }
    }
    fclose(f);
    g_coverdsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_coverdsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    g_coverdsc.header.w      = CBMP;
    g_coverdsc.header.h      = CBMP;
    g_coverdsc.header.stride = CBMP*4;
    g_coverdsc.data          = g_coverbuf;
    g_coverdsc.data_size     = sizeof g_coverbuf;
    return 0;
}

/* The accent path (OKLab helpers + read_cover_bmp + accent_from_buf) is compiled
 * at -O0: at -Os this float-heavy code SIGSEGVs on the worker/prewarm thread
 * (heisenbug - adding any logging hid it; bounds/indices verified clean by two
 * static-analysis passes). -O0 makes the codegen deterministic. It runs off the UI thread
 * so the speed cost is irrelevant. */
#pragma GCC push_options
#pragma GCC optimize ("O0")

/* ---- Apple-grade album accent: OKLab k-means clustering ->
 * Vibrant-on-dark swatch scoring -> tone-map -> WCAG contrast-lift vs the dark UI
 * -> neutral fallbacks. Runs once per cover decode. */
static float g_lin_lut[256]; static int g_lin_lut_init = 0;
static void lin_lut_init(void){
    for(int i=0;i<256;i++){ float x=i/255.0f; g_lin_lut[i]= x<=0.04045f ? x/12.92f : powf((x+0.055f)/1.055f, 2.4f); }
    g_lin_lut_init=1;
}
static float a_clamp01(float x){ return x<0?0:(x>1?1:x); }
static float a_smooth(float a,float b,float x){ if(b<=a) return x>=b?1.0f:0.0f; float t=a_clamp01((x-a)/(b-a)); return t*t*(3.0f-2.0f*t); }
static void rgb2oklab(int R,int G,int B, float *L,float *a,float *bb){
    float r=g_lin_lut[R], g=g_lin_lut[G], bl=g_lin_lut[B];
    float l=0.4122214708f*r+0.5363325363f*g+0.0514459929f*bl;
    float m=0.2119034982f*r+0.6806995451f*g+0.1073969566f*bl;
    float s=0.0883024619f*r+0.2817188376f*g+0.6299787005f*bl;
    float l_=cbrtf(l), m_=cbrtf(m), s_=cbrtf(s);
    *L =0.2104542553f*l_+0.7936177850f*m_-0.0040720468f*s_;
    *a =1.9779984951f*l_-2.4285922050f*m_+0.4505937099f*s_;
    *bb=0.0259040371f*l_+0.7827717662f*m_-0.8086757660f*s_;
}
/* OKLab -> sRGB (Ottosson inverse). Returns 1 if in gamut; always fills R/G/B (clamped). */
static int oklab2rgb(float L,float a,float b, int *R,int *G,int *B){
    float l_=L+0.3963377774f*a+0.2158037573f*b;
    float m_=L-0.1055613458f*a-0.0638541728f*b;
    float s_=L-0.0894841775f*a-1.2914855480f*b;
    float l=l_*l_*l_, m=m_*m_*m_, s=s_*s_*s_;
    float lin[3];
    lin[0]= 4.0767416621f*l-3.3077115913f*m+0.2309699292f*s;
    lin[1]=-1.2684380046f*l+2.6097574011f*m-0.3413193965f*s;
    lin[2]=-0.0041960863f*l-0.7034186147f*m+1.7076147010f*s;
    int in = 1; int *o[3]={R,G,B};
    for(int i=0;i<3;i++){
        float x=lin[i];
        if(x<-0.001f||x>1.001f) in=0;
        if(x<0)x=0; if(x>1)x=1;
        float v = x<=0.0031308f ? 12.92f*x : 1.055f*powf(x,1.0f/2.4f)-0.055f;
        *o[i] = (int)(v*255.0f+0.5f);
    }
    return in;
}
static float wcag_lum(int R,int G,int B){ return 0.2126f*g_lin_lut[R]+0.7152f*g_lin_lut[G]+0.0722f*g_lin_lut[B]; }

/* Read a 148x148 BGR24 BMP (as written by art_make_all) into a caller-owned
 * CBMP*CBMP*4 BGRA buffer. Thread-safe (no globals) so the art worker / prewarm
 * thread can compute accents off the UI thread. Returns 0 on success. */
static int read_cover_bmp(const char *path, uint8_t *buf)
{
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    uint8_t hdr[54];
    if(fread(hdr,1,54,f)!=54){ fclose(f); return -1; }
    uint32_t off = hdr[10]|(hdr[11]<<8)|(hdr[12]<<16)|((uint32_t)hdr[13]<<24);
    int w = hdr[18]|(hdr[19]<<8), h = hdr[22]|(hdr[23]<<8);
    int bpp = hdr[28]|(hdr[29]<<8);
    uint32_t comp = hdr[30]|(hdr[31]<<8)|(hdr[32]<<16)|((uint32_t)hdr[33]<<24);
    if(w!=CBMP || h!=CBMP || bpp!=24 || comp!=0){ fclose(f); return -1; }   /* only the tightly-packed 24bpp BMPs we write */
    uint8_t row[CBMP*3];
    if(fseek(f, off, SEEK_SET)!=0){ fclose(f); return -1; }
    for(int yy=0; yy<CBMP; yy++){
        if(fread(row,1,CBMP*3,f)!=(size_t)(CBMP*3)){ fclose(f); return -1; }
        uint8_t *d = buf + (CBMP-1-yy)*CBMP*4;       /* bottom-up -> top-down */
        for(int x=0;x<CBMP;x++){
            d[x*4+0]=row[x*3+0]; d[x*4+1]=row[x*3+1];
            d[x*4+2]=row[x*3+2]; d[x*4+3]=0xFF;
        }
    }
    fclose(f);
    return 0;
}

/* Core accent computation from a 148x148 BGRA buffer. Reentrant: heap scratch,
 * reads only the passed buffer + the read-only g_lin_lut (seeded once at startup),
 * so the UI thread, art worker, and prewarm thread can all call it concurrently.
 * Returns 1 and fills *out, or 0 if no usable colour. */
static int accent_from_buf(const uint8_t *cbuf, uint32_t *out_rgb)
{
    if(!cbuf) return 0;
    float *pL=malloc(2025*sizeof(float)), *pA=malloc(2025*sizeof(float)),
          *pB=malloc(2025*sizeof(float)), *pW=malloc(2025*sizeof(float));
    if(!pL||!pA||!pB||!pW){ free(pL);free(pA);free(pB);free(pW); return 0; }
    int rc = 0;
    int n=0; float wsum=0,csum=0;
    for(int y=8;y<140;y+=3){
        const uint8_t *rp=cbuf+(size_t)y*CBMP*4;
        for(int x=8;x<140 && n<2025;x+=3){
            const uint8_t *p=rp+x*4; int B=p[0],G=p[1],R=p[2];   /* B,G,R,A */
            float L,a,b; rgb2oklab(R,G,B,&L,&a,&b);
            float C=sqrtf(a*a+b*b), w=1.0f;
            if(C<0.025f||L<0.08f||L>0.96f) w=0.35f;             /* de-weight near grey/black/white */
            pL[n]=L;pA[n]=a;pB[n]=b;pW[n]=w; wsum+=w; csum+=C*w; n++;
        }
    }
    if(n<32) goto done;
    if(wsum>0 && csum/wsum < 0.025f){ *out_rgb=0xAEB4BE; rc=1; goto done; }   /* greyscale -> soft cool neutral */

    /* k-means (k=8), farthest-point seeding (deterministic) */
    { const int K=8; float cL[8],cA[8],cB[8],pop[8];
    { int bi=0; float bc=-1; for(int i=0;i<n;i++){ float c=pA[i]*pA[i]+pB[i]*pB[i]; if(c>bc){bc=c;bi=i;} }
      cL[0]=pL[bi];cA[0]=pA[bi];cB[0]=pB[bi]; }
    for(int k=1;k<K;k++){ int bi=0; float bd=-1;
        for(int i=0;i<n;i++){ float md=1e9f;
            for(int j=0;j<k;j++){ float dL=pL[i]-cL[j],da=pA[i]-cA[j],db=pB[i]-cB[j]; float d=1.25f*dL*dL+da*da+db*db; if(d<md)md=d; }
            if(md>bd){bd=md;bi=i;} }
        cL[k]=pL[bi];cA[k]=pA[bi];cB[k]=pB[bi]; }
    for(int it=0;it<10;it++){
        float sL[8]={0},sA[8]={0},sB[8]={0},sw[8]={0};
        for(int i=0;i<n;i++){ int bj=0; float bd=1e9f;
            for(int j=0;j<K;j++){ float dL=pL[i]-cL[j],da=pA[i]-cA[j],db=pB[i]-cB[j]; float d=1.25f*dL*dL+da*da+db*db; if(d<bd){bd=d;bj=j;} }
            float w=pW[i]; sL[bj]+=pL[i]*w; sA[bj]+=pA[i]*w; sB[bj]+=pB[i]*w; sw[bj]+=w; }
        for(int j=0;j<K;j++){ if(sw[j]>0){cL[j]=sL[j]/sw[j];cA[j]=sA[j]/sw[j];cB[j]=sB[j]/sw[j];} pop[j]=sw[j]; }
    }
    /* score clusters against a Vibrant-on-dark target (L 0.72, C 0.16) */
    float best=-1; int bk=-1;
    for(int j=0;j<K;j++){
        float L=cL[j],a=cA[j],b=cB[j]; float C=sqrtf(a*a+b*b);
        float pf = wsum>0 ? pop[j]/wsum : 0;
        if(pf<0.008f||C<0.035f||L<0.18f||L>0.92f) continue;
        float popS=sqrtf(pf);
        float lS=1.0f-a_clamp01(fabsf(L-0.72f)/0.32f);
        float cS=1.0f-a_clamp01(fabsf(C-0.16f)/0.14f);
        float vivid=a_smooth(0.07f,0.18f,C);
        float darkP=a_smooth(0.18f,0.38f,L);
        float lightP=1.0f-a_smooth(0.88f,0.98f,L);
        float sc=(0.42f*popS+0.28f*cS+0.22f*lS+0.08f*vivid)*darkP*lightP;
        if(pf<0.02f && C<0.12f) sc*=0.5f;                  /* stability: distrust small low-chroma specks */
        if(sc>best){best=sc;bk=j;}
    }
    if(bk<0){ *out_rgb=0xAEB4BE; rc=1; goto done; }    /* no usable colour -> neutral */

    float L=cL[bk],a=cA[bk],b=cB[bk]; float C=sqrtf(a*a+b*b), H=atan2f(b,a);
    /* tone-map (preserve hue) */
    if(L<0.58f)L=0.58f; else if(L>0.84f)L=0.84f;
    if(C<0.075f)C=0.075f; else if(C>0.22f)C=0.22f;
    if(C>0.18f&&L>0.72f) C*=0.92f;
    if(C<0.10f) C=C+(0.10f-C)*0.35f;
    a=C*cosf(H); b=C*sinf(H);
    /* WCAG contrast lift vs #1C1C1E (>=4.5:1 for glyphs); shrink C only if out of gamut */
    float Ybg=wcag_lum(0x1C,0x1C,0x1E); int R=0,G=0,Bo=0;
    for(int iter=0;iter<48;iter++){
        a=C*cosf(H); b=C*sinf(H);
        int in=oklab2rgb(L,a,b,&R,&G,&Bo);
        if(!in && C>0.04f){ C*=0.94f; continue; }
        float Y=wcag_lum(R,G,Bo); float hi=Y>Ybg?Y:Ybg, lo=Y>Ybg?Ybg:Y;
        if((hi+0.05f)/(lo+0.05f) >= 4.5f || L>=0.88f) break;
        L+=0.015f;
    }
    a=C*cosf(H); b=C*sinf(H); oklab2rgb(L,a,b,&R,&G,&Bo);
    *out_rgb = ((uint32_t)R<<16)|((uint32_t)G<<8)|(uint32_t)Bo;
    rc=1;
    }
done:
    free(pL);free(pA);free(pB);free(pW);
    return rc;
}
#pragma GCC pop_options

static int g_accent_gray = 0;   /* currently showing the idle/no-track neutral */
/* Accent config, mirrored from settings (read on the UI thread + art workers).
 * mode 0 = dynamic (album-art OKLab); 1 = static (fixed user colour: no OKLab
 * compute, no SONG.ACCENT read/write). g_accent_color is 0xRRGGBB. */
static _Atomic int g_accent_mode = 0;   /* read by art worker/prewarm threads -> atomic */
static uint32_t    g_accent_color = 0xF23260;   /* UI-thread only */
static uint32_t    g_static_last  = 0xFFFFFFFFu; /* last static accent actually applied (skip redundant re-applies) */
int ui_accent_is_static(void){ return g_accent_mode == 1; }   /* worker/prewarm gate */

static void update_accent_seed(const track_state_t *st)
{
    if(g_accent_mode == 1){            /* static: fixed colour; repaint only when it actually changed -
                                        * the 332x332 arc invalidation is ~85% of the screen every tick */
        if(g_static_last != g_accent_color){
            accent = lv_color_hex(g_accent_color);
            apply_accent();
            g_static_last = g_accent_color;
        }
        return;
    }
    if(!st || !st->have_track){
        /* no track (idle, or the ~1s while playback is starting): show a neutral light
         * grey instead of a stale/weird accent from the last track. Apply once. */
        if(!g_accent_gray){
            accent = lv_color_hex(0xC4C6CB);
            g_accent_gray = 1;
            last_seed_a[0] = 0; last_seed_b[0] = 0;
            apply_accent();
        }
        return;
    }
    g_accent_gray = 0;
    const char *seed_a = st->album[0] ? st->album : st->title;
    const char *seed_b = st->artist;

    if(strcmp(last_seed_a, seed_a) == 0 && strcmp(last_seed_b, seed_b) == 0) {
        return;
    }

    copy_cstr(last_seed_a, sizeof(last_seed_a), seed_a);
    copy_cstr(last_seed_b, sizeof(last_seed_b), seed_b);
    /* Use the cached per-song album-art accent if we have it (instant, consistent). If not,
     * DON'T flash an arbitrary text-hash colour (that looked "random" between tracks) - keep
     * the current accent until apply_art() applies the real OKLab colour when the cover
     * finishes decoding. So the accent only ever shows a real, album-derived colour. */
    int rgb = mdb_song_accent(st->path);
    if(rgb){ accent = lv_color_hex(rgb); apply_accent(); }
}

/* set by clear_art_state / apply_art (main thread); main.c re-pushes the
 * art-dependent surfaces (Home pill / Saver backdrop / Options thumb) when set. */
static int g_art_applied = 0;

/* Drop all album-art state to the no-art fallback (used for no-track, empty
 * path, and failed decode) so stale art isn't republished to Home/Saver. */
static void clear_art_state(void)
{
    cover_valid = thumb_valid = backdrop_valid = 0;
    coverdsc_valid = 0;
    last_art_key[0] = '\0';
    if(backdrop) lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    if(cover_note) lv_label_set_text(cover_note, LV_SYMBOL_AUDIO);  /* no-art / failed decode = music note */
    set_cover_fallback(true);
    g_art_applied = 1;   /* re-push: clears Home/Saver since validity flags are now 0 */
}

/* ---- album-art worker ---------------------------------------------------
 * art_make_all() forks ffmpeg (hundreds of ms) - running it inline stalled
 * touch/render on every cross-album track change. It now runs on a detached
 * worker thread that ONLY writes the /tmp BMPs (touches no LVGL / g_coverbuf
 * state); the main thread applies the result in ui_art_poll() (LVGL is not
 * thread-safe). A new decode ALWAYS targets the non-displayed buffer
 * (displayed_idx ^ 1), so an in-flight decode can never overwrite the /tmp BMP the
 * shown art (Home pill / Saver backdrop) may still be rendering from. */
typedef struct {
    char track[256];
    int  idx;
    char out[40], tout[40], bout[40];
    char art_key[420];       /* album+dir reuse/staleness key (see last_art_key) */
    int  is_clear;           /* "no track" - handled on the main thread; worker skips */
} art_req_t;

/* Serializes the two ffmpeg art decoders (the live NP worker below + the background cover prewarm) so
 * they never run two ~19MB image pipelines at once (OOM/stall). Held only around the ffmpeg call. */
static pthread_mutex_t g_decode_mu = PTHREAD_MUTEX_INITIALIZER;
/* Shared by ALL ffmpeg artwork decoders (live NP, prewarm, AND the vinyl saver) so at most one runs at
 * once - two ~19MB pipelines together risk OOM. saver.c wraps its decode with these. */
void ui_decode_lock(void){ pthread_mutex_lock(&g_decode_mu); }
void ui_decode_unlock(void){ pthread_mutex_unlock(&g_decode_mu); }

/* Album-cover prewarm work queue. The MAIN thread enqueues representative track paths (built with the
 * in-memory album/track caches, which are NOT thread-safe); the prewarm worker only pops + decodes, so
 * no mdb in-memory cache is ever touched off the UI thread. Bounded ring: enqueue fails (0) when full,
 * and the enumerator then pauses rather than dropping albums. */
#define PWQ_MAX 48   /* the worker drains at ~2/s; the seed self-throttles on a full queue (resumes the same album), so a small ring is plenty */
static char            pwq[PWQ_MAX][512];
static int             pwq_head, pwq_tail;
static pthread_mutex_t pwq_mu = PTHREAD_MUTEX_INITIALIZER;
int ui_prewarm_enqueue(const char *path){          /* MAIN thread; returns 1 if queued, 0 if full/dup */
    if(!path || !path[0]) return 1;
    int ok = 0;
    pthread_mutex_lock(&pwq_mu);
    int nt = (pwq_tail+1) % PWQ_MAX;
    if(nt != pwq_head){
        int dup = 0;                                /* cheap dedup: same path already pending */
        for(int i=pwq_head; i!=pwq_tail; i=(i+1)%PWQ_MAX) if(!strcmp(pwq[i], path)){ dup=1; break; }
        if(dup) ok = 1;
        else { copy_cstr(pwq[pwq_tail], sizeof pwq[pwq_tail], path); pwq_tail = nt; ok = 1; }
    }
    pthread_mutex_unlock(&pwq_mu);
    return ok;
}
static int pwq_pop(char *out, int cap){            /* worker thread; 1 if an item was dequeued */
    int got = 0;
    pthread_mutex_lock(&pwq_mu);
    if(pwq_head != pwq_tail){ copy_cstr(out, cap, pwq[pwq_head]); pwq_head = (pwq_head+1)%PWQ_MAX; got = 1; }
    pthread_mutex_unlock(&pwq_mu);
    return got;
}
static int pwq_pending(void){ pthread_mutex_lock(&pwq_mu); int p = (pwq_head != pwq_tail); pthread_mutex_unlock(&pwq_mu); return p; }
static void pw_sleep(int secs){                    /* idle sleep, but wake early the moment cover work is queued */
    for(int i=0;i<secs;i++){ if(pwq_pending()) return; sleep(1); }
}

static pthread_mutex_t g_art_mu = PTHREAD_MUTEX_INITIALIZER;
static art_req_t g_art_target;      /* latest requested decode          (guarded) */
static unsigned  g_art_req      = 0;/* bumps on each request            (guarded) */
static int       g_art_inflight = 0;/* a worker is running              (guarded) */
static int       g_art_ready    = 0;/* a result is waiting for the main thread (guarded) */
static int       g_art_rc       = -1;
static unsigned  g_art_done_req = 0;
static art_req_t g_art_result;      /* the req the waiting result is for (guarded) */
/* g_art_applied is declared above clear_art_state (main-thread only) */

/* main thread only: push a finished decode onto the NP cover + validity flags */
static void apply_art(const art_req_t *job)
{
    snprintf(cover_src,    sizeof cover_src,    "A:/tmp/cover%d.bmp",    job->idx);
    snprintf(thumb_src,    sizeof thumb_src,    "A:/tmp/thumb%d.bmp",    job->idx);
    snprintf(backdrop_src, sizeof backdrop_src, "A:/tmp/backdrop%d.bmp", job->idx);
    coverdsc_valid = (load_cover_dsc(job->out) == 0);   /* track whether g_coverdsc is THIS track's */
    if(coverdsc_valid) lv_image_set_src(cover_img, &g_coverdsc);
    else               lv_image_set_src(cover_img, cover_src);
    lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_pivot(cover_img, COVER_D/2, COVER_D/2);
    set_cover_fallback(false);
    cover_valid = thumb_valid = backdrop_valid = 1;
    /* accent was computed + cached off the UI thread (art worker / prewarm), so just
     * read it - never run OKLab on the UI thread. If absent (no art, or no usable
     * colour), keep the text-hash accent set by update_accent_seed().
     * In static mode the user picked a fixed accent - never let cached art override it. */
    if(g_accent_mode != 1){
        int acc_rgb = mdb_song_accent(job->track);
        if(acc_rgb){ accent = lv_color_hex(acc_rgb); apply_accent(); }
    }
    copy_cstr(last_art_key, sizeof last_art_key, job->art_key);   /* cache key (album+dir) */
    if(backdrop) {
        lv_image_set_src(backdrop, backdrop_src);
        lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_background(backdrop);
    }
    displayed_idx = job->idx;   /* this buffer is now the one on screen */
    g_art_applied = 1;          /* main.c re-pushes Home/Saver/npmenu art next loop */
}

static void *art_worker(void *arg)
{
    (void)arg;
    for(;;){
        art_req_t job; unsigned my_req, my_gen;
        pthread_mutex_lock(&g_art_mu);
        job = g_art_target; my_req = g_art_req;
        my_gen = art_cancel_gen();   /* capture the cancel token WITH the request: art_cancel() is only called
                                      * by the setter under g_art_mu, so it can't interleave this snapshot */
        pthread_mutex_unlock(&g_art_mu);

        int rc;
        if(job.is_clear) rc = -1;
        else if(artcache_get(job.track, job.out, job.tout, job.bout) == 0) rc = 0;   /* cached -> fast copy, no ffmpeg */
        else { pthread_mutex_lock(&g_decode_mu);                                     /* miss -> decode (killable), then cache */
               /* Pass OUR captured token: if a skip supersedes this decode any time after the snapshot (it
                * always bumps g_cancel_gen via art_cancel), the per-child gen check trips and no obsolete
                * decode runs - closing the window between request-validation and the decode. */
               rc = art_make_all_ex_gen(job.track, job.out, job.tout, job.bout, 1, my_gen);
               pthread_mutex_unlock(&g_decode_mu);
               if(rc == 0) artcache_put(job.track, job.out, job.tout, job.bout); }

        /* compute + cache the OKLab accent HERE (off the UI thread) so apply_art()
         * never blocks: by the time the main thread applies this decode, the accent
         * is already in the DB and is just read back. Skipped in static-accent mode. */
        if(rc == 0 && !job.is_clear && !ui_accent_is_static() && mdb_song_accent(job.track) == 0){
            uint8_t *buf = malloc(CBMP*CBMP*4);
            if(buf){
                uint32_t rgb;
                if(read_cover_bmp(job.out, buf) == 0 && accent_from_buf(buf, &rgb))
                    mdb_set_song_accent(job.track, (int)(rgb & 0xFFFFFF));
                free(buf);
            }
        }

        pthread_mutex_lock(&g_art_mu);
        if(g_art_req == my_req){            /* still the latest request -> publish + stop */
            g_art_rc = rc; g_art_result = job; g_art_done_req = my_req;
            g_art_ready = 1; g_art_inflight = 0;
            pthread_mutex_unlock(&g_art_mu);
            return NULL;
        }
        pthread_mutex_unlock(&g_art_mu);    /* a newer request arrived -> loop, serve it */
    }
}

/* main thread: clear art NOW + invalidate any in-flight decode (so a track that
 * was mid-decode can't publish stale art after a no-track / clear). */
static void art_request_clear(void)
{
    clear_art_state();
    pthread_mutex_lock(&g_art_mu);
    g_art_req++;
    memset(&g_art_target, 0, sizeof g_art_target);
    g_art_target.is_clear = 1;
    pthread_mutex_unlock(&g_art_mu);
    art_cancel();   /* kill any in-flight decode so it can't publish stale art */
}

/* ---- audiobook chapters: Now Playing shows the current chapter instead of a meaningless album ----
 * A book's chapters are read once per book (chpl parse), then the current one is picked by position. */
#define NP_MAXCHAP 256   /* a chpl count is a single byte (<=255) - cover every declared chapter */
static chapter_t g_chaps[NP_MAXCHAP];
static int       g_nchaps;
static char      g_chap_path[256];

/* Build the Now Playing secondary line for a BOOK. Returns 1 if st is a book (out is set - possibly
 * empty when the book carries no chapters), 0 if it isn't a book (caller shows the album line). */
static int book_meta_line(const track_state_t *st, char *out, int cap)
{
    if(!mdb_is_book_path(st->path)) return 0;
    if(strcmp(g_chap_path, st->path) != 0){          /* new book -> load its chapters once (udta chpl parse) */
        g_nchaps = scan_read_chapters(st->path, g_chaps, NP_MAXCHAP);
        copy_cstr(g_chap_path, sizeof g_chap_path, st->path);
    }
    if(g_nchaps <= 0){ out[0] = '\0'; return 1; }    /* a book without chapters: show nothing, not an album */
    int cur = 0;
    for(int i = 0; i < g_nchaps; i++){ if(g_chaps[i].start_ms <= st->position_ms) cur = i; else break; }
    /* The chpl reader fills untitled chapters with "Chapter N"; if this chapter has only that
     * auto-name, show "Chapter N/M" rather than the redundant "N/M · Chapter N". */
    char autoname[24]; snprintf(autoname, sizeof autoname, "Chapter %d", cur + 1);
    if(strcmp(g_chaps[cur].title, autoname) == 0)
        snprintf(out, cap, "Chapter %d/%d", cur + 1, g_nchaps);
    else
        snprintf(out, cap, "%d/%d  \xC2\xB7  %s", cur + 1, g_nchaps, g_chaps[cur].title);   /* 2/3 · Into the Future */
    return 1;
}

/* Swap the prev/next transport glyphs between music (track skip) and audiobook (-15s/+30s). Only
 * touches the widgets when the mode actually changes, so it's cheap to call every update. */
static int g_np_bookmode = -1;   /* -1 unknown, 0 music, 1 book */
static void np_transport_glyphs(int book)
{
    if(book == g_np_bookmode) return;
    g_np_bookmode = book;
    lv_obj_set_style_text_font(btn_prev, book ? &lv_font_montserrat_22 : &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_next, book ? &lv_font_montserrat_22 : &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(btn_prev, book ? "-15" : LV_SYMBOL_PREV);
    lv_label_set_text(btn_next, book ? "+30" : LV_SYMBOL_NEXT);
}

/* ---- audiobook sleep timer (moon on Now Playing) --------------------------------------------- */
/* End of the chapter that currently contains the playback position (in book ms), for "End of chapter"
 * sleep. Reuses the g_chaps cache loaded by book_meta_line. */
static long book_cur_chapter_end(const track_state_t *st)
{
    if(strcmp(g_chap_path, st->path) != 0){
        g_nchaps = scan_read_chapters(st->path, g_chaps, NP_MAXCHAP);
        copy_cstr(g_chap_path, sizeof g_chap_path, st->path);
    }
    if(g_nchaps <= 0) return st->duration_ms;
    int cur = 0;
    for(int i = 0; i < g_nchaps; i++){ if(g_chaps[i].start_ms <= st->position_ms) cur = i; else break; }
    if(cur + 1 < g_nchaps) return g_chaps[cur+1].start_ms;
    return st->duration_ms > 0 ? st->duration_ms : g_chaps[cur].start_ms;   /* last chapter ends at the book end */
}

/* The [start,end] ms window of the chapter currently under the position, for a book with chapters.
 * Returns 1 (window set) or 0 (not a chaptered book -> caller uses the whole track). Reuses g_chaps. */
static int book_chapter_window(const track_state_t *st, long *lo, long *hi)
{
    if(!mdb_is_book_path(st->path)) return 0;
    if(strcmp(g_chap_path, st->path) != 0){
        g_nchaps = scan_read_chapters(st->path, g_chaps, NP_MAXCHAP);
        copy_cstr(g_chap_path, sizeof g_chap_path, st->path);
    }
    if(g_nchaps <= 0) return 0;
    int cur = 0;
    for(int i = 0; i < g_nchaps; i++){ if(g_chaps[i].start_ms <= st->position_ms) cur = i; else break; }
    long a = g_chaps[cur].start_ms;
    long b = (cur + 1 < g_nchaps) ? g_chaps[cur+1].start_ms
                                  : (st->duration_ms > 0 ? st->duration_ms : a);
    if(b <= a) return 0;                 /* degenerate chapter -> fall back to the whole track */
    *lo = a; *hi = b;
    return 1;
}

static lv_obj_t *g_sleep_dlg;
void ui_np_close_overlays(void){ if(g_sleep_dlg){ lv_obj_del(g_sleep_dlg); g_sleep_dlg = NULL; } }
int  ui_np_overlay_active(void){ return g_sleep_dlg != NULL; }   /* 1 while the sleep popover is up (main loop suppresses the ring/nav gesture) */

static void sleep_opt_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int v = (int)(intptr_t)lv_event_get_user_data(e);   /* minutes; 0 = off; -1 = end of chapter */
    if(v == -1){
        track_state_t st; ipc_get_state(&st);
        if(!mdb_is_book_path(st.path)){ ui_set_sleep_timer(0); ui_np_close_overlays(); return; }  /* context changed away from a book */
        long tgt = book_cur_chapter_end(&st);
        int at_end = (st.duration_ms > 0 && tgt >= st.duration_ms);   /* book_cur_chapter_end returns EXACTLY the duration only for the last / chapterless chapter -> the target is the file end (a rollover may fulfil it); a mid-book boundary is < duration */
        if(tgt > st.position_ms){ ui_set_sleep_eoc(tgt, st.path, at_end); ui_toast("Sleep at end of chapter"); }
        else { ui_set_sleep_timer(0); ui_toast("Sleep timer off"); }
    } else if(v > 0){
        ui_set_sleep_timer(v);
        char b[24]; snprintf(b, sizeof b, "Sleep in %d min", v); ui_toast(b);
    } else {
        ui_set_sleep_timer(0); ui_toast("Sleep timer off");
    }
    ui_np_close_overlays();
}
static void sleep_scrim_cb(lv_event_t *e){ if(lv_event_get_code(e) == LV_EVENT_CLICKED) ui_np_close_overlays(); }

static void sleep_click_cb(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if(g_sleep_dlg){ ui_np_close_overlays(); return; }              /* tap the moon again to dismiss */
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());               /* full-screen dim; tap outside closes */
    g_sleep_dlg = scrim;
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, 360, 360); lv_obj_center(scrim);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scrim, sleep_scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(scrim);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 224, 296); lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_set_style_pad_ver(panel, 14, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(panel);
    lv_label_set_text(hdr, "Sleep Timer");
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xFFFFFF), 0);

    static const char *OPT_T[] = { "15 min", "30 min", "45 min", "60 min", "End of chapter", "Off" };
    static const int   OPT_V[] = { 15, 30, 45, 60, -1, 0 };
    for(unsigned i = 0; i < sizeof OPT_V / sizeof OPT_V[0]; i++){
        lv_obj_t *b = lv_button_create(panel);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 196, 34);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, sleep_opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)OPT_V[i]);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, OPT_T[i]);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, OPT_V[i] == -1 ? accent : lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
    }
}

/* art reuse/staleness key: album title + the track's parent directory. Audiobooks (.m4b) each carry
 * their own cover, so key them by full path - two books sharing an album tag in one folder must not
 * reuse each other's cover. */
static void make_art_key(const track_state_t *st, char *out, int cap)
{
    if(mdb_is_book_path(st->path)){ snprintf(out, cap, "%s", st->path); return; }
    char dir[256]; snprintf(dir, sizeof dir, "%s", st->path);
    char *slash = strrchr(dir, '/'); if(slash) *slash = '\0'; else dir[0] = '\0';
    snprintf(out, cap, "%s\n%s", st->album, dir);
}

/* main thread: decide whether art needs (re)decoding and dispatch to the worker */
static void update_cover_for_path(const track_state_t *st)
{
    if(strcmp(last_path, st->path) == 0) return;
    char art_key[420]; make_art_key(st, art_key, sizeof art_key);
    copy_cstr(want_art_key, sizeof want_art_key, art_key);   /* what the live track wants now */

    /* Art is shared within an album/folder: same key + valid art -> reuse, skip ffmpeg. */
    if(st->album[0] && cover_valid && strcmp(last_art_key, art_key) == 0) {
        copy_cstr(last_path, sizeof(last_path), st->path);
        /* art is already valid for this album - ensure the cover is shown (recovers
         * if a previous in-flight load had swapped in the loading glyph and then a
         * same-album track arrived before that decode landed). */
        if(cover_note) lv_label_set_text(cover_note, LV_SYMBOL_AUDIO);
        set_cover_fallback(false);
        return;
    }

    copy_cstr(last_path, sizeof(last_path), st->path);

    if(!st->path[0]) { art_request_clear(); return; }

    /* New album/art incoming: hide the now-wrong NP cover and show a loading glyph
     * while the worker decodes, so the previous album's art doesn't masquerade as
     * this track's. apply_art() (success) or clear_art_state() (fail) restores it.
     * NB: do NOT touch cover_valid here - it's read by ui_current_cover_dsc() for the
     * vinyl saver, which (like the Home thumb/backdrop) keeps last-good art until the
     * new decode lands; only this prominent NP cover shows the loading state. */
    if(cover_note) lv_label_set_text(cover_note, LV_SYMBOL_REFRESH);
    set_cover_fallback(true);
    coverdsc_valid = 0;   /* the RAM decode is now the PREVIOUS album's - don't let the accent use stale art */

    /* ALWAYS target the non-displayed buffer so an in-flight decode never
     * overwrites the /tmp BMP the shown art may still reference. */
    int nidx = displayed_idx ^ 1;
    art_req_t job; memset(&job, 0, sizeof job);
    job.idx = nidx;
    copy_cstr(job.track, sizeof job.track, st->path);
    copy_cstr(job.art_key, sizeof job.art_key, art_key);
    snprintf(job.out,  sizeof job.out,  "/tmp/cover%d.bmp",    nidx);
    snprintf(job.tout, sizeof job.tout, "/tmp/thumb%d.bmp",    nidx);
    snprintf(job.bout, sizeof job.bout, "/tmp/backdrop%d.bmp", nidx);

    int launch = 0;
    pthread_mutex_lock(&g_art_mu);
    g_art_req++;
    g_art_target = job;
    if(!g_art_inflight){ g_art_inflight = 1; launch = 1; }
    else art_cancel();   /* a worker is mid-decode on a now-stale track -> kill its ffmpeg so it serves this
                          * one. Done UNDER g_art_mu (not after unlocking): otherwise the worker could read
                          * the NEW target and start its ffmpeg in the gap, and this cancel would kill THAT
                          * valid decode instead (leaving art missing). Deadlock-free: no path holds g_pid_mu
                          * (which art_cancel takes) while acquiring g_art_mu, and kill() doesn't block. */
    pthread_mutex_unlock(&g_art_mu);

    if(launch){
        pthread_t th;
        if(pthread_create(&th, NULL, art_worker, NULL) == 0){
            pthread_detach(th);
        } else {                            /* spawn failed -> decode synchronously */
            pthread_mutex_lock(&g_art_mu); g_art_inflight = 0; pthread_mutex_unlock(&g_art_mu);
            pthread_mutex_lock(&g_decode_mu);   /* still exclude the prewarm decoder (spawn failed under memory pressure) */
            int r = art_make_all(job.track, job.out, job.tout, job.bout);
            pthread_mutex_unlock(&g_decode_mu);
            if(r == 0) apply_art(&job);
            else clear_art_state();
        }
    }
    /* else: a worker is already running; it will pick up g_art_target when it loops */
}

/* main-thread timer: apply a finished decode (or re-clear) */
void ui_art_poll(lv_timer_t *t)
{
    (void)t;
    int ready = 0, rc = -1; art_req_t res; unsigned done_req = 0, cur_req = 0;
    pthread_mutex_lock(&g_art_mu);
    if(g_art_ready){
        ready = 1; g_art_ready = 0;
        rc = g_art_rc; res = g_art_result; done_req = g_art_done_req; cur_req = g_art_req;
    }
    pthread_mutex_unlock(&g_art_mu);
    if(!ready) return;
    if(done_req != cur_req) return;        /* superseded - a newer decode is in flight */
    if(res.is_clear)      clear_art_state();/* main already cleared; idempotent */
    else if(rc == 0){
        /* only apply if the live track still wants this album - guards the rare
         * A(X)->B(Y, decoding)->C(X) race where Y would otherwise stomp correct X art. */
        if(strcmp(res.art_key, want_art_key) == 0) apply_art(&res);
    }
    else                  clear_art_state();
}

/* main.c calls this each loop: returns 1 once after art was (re)applied so the
 * dependent surfaces (Home pill / Saver backdrop / Options thumb) get re-pushed. */
int ui_take_art_applied(void)
{
    if(!g_art_applied) return 0;
    g_art_applied = 0;
    return 1;
}

/* Settings hook: apply the chosen accent mode/colour immediately (main thread). */
void ui_set_accent_config(int mode, int rgb){
    g_accent_mode  = mode ? 1 : 0;
    g_accent_color = (uint32_t)rgb & 0xFFFFFF;
    if(g_accent_mode){
        accent = lv_color_hex(g_accent_color);   /* paint the fixed colour now */
        g_accent_gray = 0;
    } else {
        last_seed_a[0] = '\0'; last_seed_b[0] = '\0';   /* force dynamic recompute next ui_update */
    }
    apply_accent();
    g_static_last = g_accent_mode ? g_accent_color : 0xFFFFFFFFu;   /* config just repainted; keep the guard in sync */
    g_art_applied = 1;   /* re-push Home/Saver/npmenu accent surfaces */
}

/* ---- background art prewarm (user-gated, heat-safe) ------------------------
 * Fills the SD cover cache (+ DB accent, dynamic mode only) so song-switching is
 * instant even on first play. The earlier always-on version overheated the device
 * to 50C, so this only works while the user-chosen window holds AND the battery is
 * cool, and it paces hard. Modes: 0=off 1=when idle 2=when charging 3=idle|charging. */
static _Atomic int g_prewarm_mode = 0;   /* cfg mirror: settings (main) writes, worker reads */
void ui_set_prewarm_mode(int m){ g_prewarm_mode = (m<0)?0:(m>2?2:m); }   /* 0=off 1=idle 2=idle&charging */

static int pw_temp_dc(void){            /* battery temp, tenths-C; -1 on read failure */
    FILE *f=fopen("/sys/class/power_supply/cw221X-bat/temp","r"); if(!f) return -1;
    int t=-1; if(fscanf(f,"%d",&t)!=1) t=-1; fclose(f); return t;
}
static int pw_charging(void){
    /* cw221X exposes NO `status` node - charging shows via current_now (1=charging, 0=battery on
     * this driver). The old `status` read always failed -> prewarm mode "idle & charging" never ran. */
    long cur=0;
    FILE *f=fopen("/sys/class/power_supply/cw221X-bat/current_now","r"); if(!f) return 0;
    if(fscanf(f,"%ld",&cur)!=1) cur=0; fclose(f);
    return cur > 0;
}
/* 1 if the user-selected window currently allows background work. IDLE is mandatory
 * for every active mode: never decode while the user is actively looking at the screen
 * (heat-/UX-safety), even on charge. m1=when idle, m2=idle AND charging. */
static int pw_window_open(void){
    if(!sd_io_allowed()) return 0;
    int m=g_prewarm_mode; if(m==0) return 0;
    if(!ui_main_is_idle()) return 0;   /* screen dimmed/off required */
    if(m==2) return pw_charging();     /* idle & charging */
    return 1;                          /* m==1: when idle */
}

/* Block until heat + the live decode allow another ffmpeg. Shared by both prewarm phases. */
static void pw_wait_window(int *hot){
    for(;;){
        if(!sd_io_allowed()){ usleep(200000); continue; }
        int t = pw_temp_dc();                       /* battery temp, tenths-C; fail-closed if unreadable */
        if(t < 0){ sleep(15); continue; }
        if(*hot){ if(t < 400) *hot=0; else { sleep(15); continue; } }   /* hysteresis: pause >=42C, resume <40C */
        else if(t >= 420){ *hot=1; sleep(15); continue; }
        int busy; pthread_mutex_lock(&g_art_mu); busy=g_art_inflight; pthread_mutex_unlock(&g_art_mu);
        if(busy){ usleep(300*1000); continue; }     /* never a 2nd ffmpeg while the user's decode runs */
        return;
    }
}

/* PHASE 1 (boot): proactively decode ONE cover per album so the Album (cover flow) view is populated
 * without playing every album first. Per-album (hundreds) not per-track (thousands), and strictly
 * battery-temp throttled, so it fills in a couple of minutes on first boot and is near-instant on later
 * boots (the SD cache persists). Runs regardless of the idle gate - users want covers ready while they
 * browse - but yields to the live decode and to heat. DB access is safe: g_db is opened FULLMUTEX. */
static void *art_prewarm_worker(void *arg)
{
    (void)arg;
    nice(15);                 /* lowest CPU priority */
    const char *PC="/tmp/pw_cover.bmp", *PT="/tmp/pw_thumb.bmp", *PB="/tmp/pw_backdrop.bmp";
    int id = 0, hot = 0, did_work = 0;   /* hot = temp hysteresis latch */
    for(;;){
        /* PHASE 1: album-cover prewarm. The MAIN thread enqueues representative track paths (built with
         * the non-thread-safe album/track caches); the worker only pops + decodes here, so no mdb
         * in-memory cache is touched off the UI thread. Runs regardless of the idle gate (users want
         * covers while browsing) but stays temp-throttled and serialized with the live NP decoder. */
        char qpath[512];
        if(pwq_pop(qpath, sizeof qpath)){
            pw_wait_window(&hot);                        /* heat gate + yields to the live decode */
            if(!artcache_has(qpath)){
                pthread_mutex_lock(&g_decode_mu);
                int r = art_make_all(qpath, PC, PT, PB);
                pthread_mutex_unlock(&g_decode_mu);
                if(r == 0) artcache_put(qpath, PC, PT, PB);
            }
            usleep(120*1000);                            /* light pace; the temp gate is the real limiter */
            continue;
        }
        /* PHASE 2: per-track idle sweep (accents + any remaining covers), gated on the user setting. */
        if(!pw_window_open()){ id=0; did_work=0; pw_sleep(5); continue; }   /* not allowed now -> wait (wake for queued covers) */

        /* temp throttle: fail-closed if unreadable; hysteresis pause >=42C, resume <40C. */
        int t = pw_temp_dc();
        if(t < 0){ sleep(15); continue; }
        if(hot){ if(t < 400) hot=0; else { sleep(15); continue; } }
        else if(t >= 420){ hot=1; sleep(15); continue; }

        /* never run a 2nd ffmpeg while the live (user) decode is in flight */
        { int busy; pthread_mutex_lock(&g_art_mu); busy=g_art_inflight; pthread_mutex_unlock(&g_art_mu);
          if(busy){ usleep(300*1000); continue; } }

        char path[300];
        if(!mdb_prewarm_next(id, &id, path, sizeof path)){   /* swept the whole library */
            id=0; pw_sleep(did_work?60:1800); did_work=0; continue;   /* rest longer if nothing pending (wake for queued covers) */
        }
        if(!path[0]) continue;

        int have_art = artcache_has(path);
        int need_acc = !ui_accent_is_static() && (mdb_song_accent(path)==0);
        if(have_art && !need_acc){ usleep(15*1000); continue; }   /* already done -> light skip */

        if(!have_art){
            pthread_mutex_lock(&g_decode_mu);
            int r = art_make_all(path, PC, PT, PB);
            pthread_mutex_unlock(&g_decode_mu);
            if(r == 0) artcache_put(path, PC, PT, PB);
            else { usleep(300*1000); continue; }                  /* undecodable -> skip */
        }
        if(need_acc){
            /* Materialise THIS path's cover before reading it. If the cached read fails (SD vanished mid-
             * sweep), PC still holds the PREVIOUS track's image; computing+storing an accent from it would
             * mislabel this path permanently (a nonzero accent skips future correction). Only proceed on a
             * confirmed fresh cover for this path (a just-decoded !have_art cover is already in PC). */
            int cover_ready = have_art ? (artcache_get(path, PC, PT, PB) == 0) : 1;
            if(cover_ready){
                uint8_t *buf = malloc(CBMP*CBMP*4);
                if(buf){
                    uint32_t rgb;
                    if(read_cover_bmp(PC, buf) == 0 && accent_from_buf(buf, &rgb))
                        mdb_set_song_accent(path, (int)(rgb & 0xFFFFFF));
                    free(buf);
                }
            }
        }
        did_work = 1;
        sleep(3);     /* hard pace between decodes: keep heat low */
    }
    return NULL;
}

/* main.c calls this once at startup. The worker self-gates on g_prewarm_mode, so
 * it's safe to always spawn (it just sleeps while mode==off). */
void ui_start_art_prewarm(void)
{
    pthread_t th;
    if(pthread_create(&th, NULL, art_prewarm_worker, NULL) == 0) pthread_detach(th);
}

/* Expose the current decoded cover to other surfaces (e.g. the Home pill).
 * Returns the "A:/tmp/coverN.bmp" path, or NULL when there's no usable art. */
const char *ui_current_cover_src(void)
{
    return cover_valid ? cover_src : NULL;
}

/* Rotatable RAM cover (ARGB8888 dsc) for the vinyl screensaver - the file-BMP
 * renders black when rotated, this one transforms fine. NULL if no art. */
const void *ui_current_cover_dsc(void)
{
    return coverdsc_valid ? (const void *)&g_coverdsc : NULL;   /* NULL if RAM decode failed (no stale art) */
}

/* Native-size 42px thumb path for the Home pill, or NULL when no art. */
const char *ui_current_thumb_src(void)
{
    return thumb_valid ? thumb_src : NULL;
}

/* Full-screen blurred backdrop path (for the screensaver), or NULL. */
const char *ui_current_backdrop_src(void)
{
    return backdrop_valid ? backdrop_src : NULL;
}

/* Montserrat covers Latin + Latin-Extended only; it has no Cyrillic/Greek (issue #3:
 * "Ленинград" rendered as boxes) and no CJK ("北京"). Build a per-size fallback chain:
 *   Montserrat(size) -> font_intl(size) [Cyrillic/Greek/Latin-ext, Noto, uncompressed]
 *                    -> Source Han 16 [CJK] -> LVGL placeholder.
 * The intl fonts are generated at matched ascent/descent (base_line 4/5/6 == Montserrat's)
 * so fallback glyphs sit on the same baseline; LVGL uses the PRIMARY font's line-height, and
 * each intl line-height (20/23/28) fits within Montserrat's (20/24/28) so nothing clips.
 * We keep mutable copies because .fallback must be set on non-const fonts. */
static lv_font_t s_cjk; /* existing subset plus the small music-metadata supplement */
static lv_font_t s_font20, s_font18, s_font16, s_font14;      /* Montserrat, chain heads */
static lv_font_t s_intl20, s_intl18, s_intl16, s_intl14;      /* intl link (fallback -> Source Han) */
static void ui_fonts_init(void)
{
    if(s_font20.get_glyph_dsc) return;   /* once */
    s_cjk = lv_font_source_han_16_cjk; s_cjk.fallback = &font_cjk_extra_16;
    s_intl20 = font_intl_20; s_intl20.fallback = &s_cjk;
    s_intl18 = font_intl_18; s_intl18.fallback = &s_cjk;
    s_intl16 = font_intl_16; s_intl16.fallback = &s_cjk;
    s_intl14 = font_intl_14; s_intl14.fallback = &s_cjk;
    s_font20 = lv_font_montserrat_20; s_font20.fallback = &s_intl20;
    s_font18 = lv_font_montserrat_18; s_font18.fallback = &s_intl18;
    s_font16 = lv_font_montserrat_16; s_font16.fallback = &s_intl16;
    s_font14 = lv_font_montserrat_14; s_font14.fallback = &s_intl14;
}

/* Public accessor for the fallback-chained text font (Montserrat -> intl -> CJK) so other screens
 * render user text (track/album/artist names) with full glyph coverage. px = 14/16/18/20. */
const lv_font_t *ui_text_font(int px){
    ui_fonts_init();
    switch(px){ case 20: return &s_font20; case 18: return &s_font18; case 14: return &s_font14; default: return &s_font16; }
}

/* Shared CJK-capable user-text font (montserrat + Source Han Sans fallback) for any screen
 * that shows track/artist/album/lyrics text. Idempotently initialises on first use. */
const lv_font_t *ui_font_cjk(int size)
{
    ui_fonts_init();
    if(size >= 20) return &s_font20;
    if(size >= 18) return &s_font18;
    if(size >= 16) return &s_font16;
    return &s_font14;
}

void ui_create(lv_obj_t *root)
{
    lv_obj_t *scr = root;

    ui_fonts_init();
    lin_lut_init();   /* seed the OKLab sRGB LUT once, up front: dynamic accent compute
                       * (art worker) needs it whether or not the prewarm ever runs. */
    /* settings_apply_startup() ran before us and already loaded g_accent_mode/g_accent_color
     * from cfg, so honor a saved STATIC accent here - otherwise Home/Saver (created right
     * after, and now reading the live accent) would paint the default until the first
     * ui_update() corrected them. Dynamic mode keeps the default until an album accent lands. */
    accent = (g_accent_mode == 1) ? lv_color_hex(g_accent_color) : lv_color_hex(C_ACCENT);
    last_path[0] = '\0';
    last_art_key[0] = '\0';
    want_art_key[0] = '\0';
    last_seed_a[0] = '\0';
    last_seed_b[0] = '\0';
    cover_src[0] = '\0';
    cover_valid = 0;
    coverdsc_valid = 0;
    thumb_src[0] = '\0';
    thumb_valid = 0;
    backdrop_valid = 0;
    displayed_idx = 0;
    shown_progress = 0;

    lv_obj_clean(scr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BLACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* full-screen blurred album-art backdrop (created first = behind everything) */
    backdrop_src[0] = '\0';
    backdrop = lv_image_create(scr);
    lv_obj_set_size(backdrop, 360, 360);
    lv_obj_align(backdrop, LV_ALIGN_CENTER, 0, 0);
    /* darken the blurred art so foreground text/controls stay readable */
    lv_obj_set_style_image_recolor(backdrop, lv_color_hex(C_BLACK), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(backdrop, 150, LV_PART_MAIN);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ring = lv_arc_create(scr);
    lv_obj_set_size(ring, ARC_D, ARC_D);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(ring, 0, 1000);
    lv_arc_set_value(ring, 0);
    lv_arc_set_bg_angles(ring, 0, ARC_SWEEP);
    lv_arc_set_rotation(ring, ARC_ROT);
    lv_arc_set_mode(ring, LV_ARC_MODE_NORMAL);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(C_LINE_SOFT), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_COVER, LV_PART_INDICATOR);
    /* draggable seek: a small knob thumb + scrub handler */
    lv_obj_set_style_bg_color(ring, lv_color_hex(C_WHITE), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ring, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ring, 5, LV_PART_KNOB);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    /* display-only: seek is driven by the NP recognizer (ui_np_seek_*) fed from
     * main.c's raw touch loop, so nav swipes / drawer pulls never grab the arc */
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    cover = lv_obj_create(scr);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, COVER_D, COVER_D);
    lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_radius(cover, 14, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cover, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cover, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(cover, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cover, LV_OBJ_FLAG_CLICKABLE);   /* tap art -> Song Info */
    lv_obj_add_event_cb(cover, cover_click_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(cover, cover_click_cb, LV_EVENT_CLICKED, NULL);

    cover_img = lv_image_create(cover);
    lv_obj_set_size(cover_img, COVER_D, COVER_D);
    lv_obj_center(cover_img);
    /* CENTER (not STRETCH): the BMP is already COVER_D, and STRETCH forces the
     * pivot to (0,0) which would break the vinyl spin. Pivot at centre. */
    lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_pivot(cover_img, COVER_D/2, COVER_D/2);
    lv_obj_add_flag(cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cover_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    cover_note = lv_label_create(cover);
    style_text(cover_note, &lv_font_montserrat_28, lv_color_hex(C_WHITE));
    lv_label_set_text(cover_note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_opa(cover_note, LV_OPA_90, LV_PART_MAIN);
    lv_obj_center(cover_note);
    lv_obj_clear_flag(cover_note, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* vinyl centre label: accent disc with a small spindle hole, on top of the
     * art. Shown only in Vinyl style. */
    spindle = lv_obj_create(cover);
    lv_obj_remove_style_all(spindle);
    lv_obj_set_size(spindle, 34, 34);
    lv_obj_center(spindle);
    lv_obj_set_style_radius(spindle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(spindle, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(spindle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(spindle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hole = lv_obj_create(spindle);
    lv_obj_remove_style_all(hole);
    lv_obj_set_size(hole, 8, 8);
    lv_obj_center(hole);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hole, lv_color_hex(C_BLACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(spindle, LV_OBJ_FLAG_HIDDEN);

    /* The Playback (tuning) and Options menus moved off this screen - they're
     * reached by a right-edge swipe (-> SCR_NPHUB); back is a left-edge swipe.
     * The seek ring fills the screen, so the ring previews a seek anywhere you
     * drag, but the actual seek is only COMMITTED on release if the gesture
     * wasn't a deliberate edge swipe (see ui_seek_commit/ui_seek_cancel, driven
     * by the main loop). So seek works everywhere except a bezel-edge flick. */

    /* Favorites heart - upper-right, just outside the cover's right edge */
    btn_fav = lv_button_create(scr);
    lv_obj_remove_style_all(btn_fav);
    lv_obj_set_size(btn_fav, 44, 44);
    lv_obj_align(btn_fav, LV_ALIGN_TOP_MID, 118, 126);
    lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_fav, fav_click_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn_fav, fav_click_cb, LV_EVENT_CLICKED, NULL);
    fav_icon = lv_label_create(btn_fav);
    lv_obj_set_style_text_font(fav_icon, &font_icons_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(fav_icon, lv_color_hex(C_TERTIARY), LV_PART_MAIN);
    lv_label_set_text(fav_icon, HEART_OUTLINE);
    lv_obj_center(fav_icon);
    lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);   /* shown once a track loads */

    /* Audiobook sleep-timer moon - occupies the heart slot for books (heart is hidden for them). */
    btn_sleep = lv_button_create(scr);
    lv_obj_remove_style_all(btn_sleep);
    lv_obj_set_size(btn_sleep, 44, 44);
    lv_obj_align(btn_sleep, LV_ALIGN_TOP_MID, 118, 126);
    lv_obj_add_flag(btn_sleep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_sleep, sleep_click_cb, LV_EVENT_CLICKED, NULL);
    sleep_icon = lv_label_create(btn_sleep);
    lv_obj_set_style_text_font(sleep_icon, &font_icons_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(sleep_icon, lv_color_hex(C_TERTIARY), LV_PART_MAIN);
    lv_label_set_text(sleep_icon, MOON_ICON);
    lv_obj_center(sleep_icon);
    lv_obj_add_flag(btn_sleep, LV_OBJ_FLAG_HIDDEN);   /* shown only for audiobooks */

    /* Play-mode toggle - left side, mirrors the heart */
    btn_mode = lv_button_create(scr);
    lv_obj_remove_style_all(btn_mode);
    lv_obj_set_size(btn_mode, 44, 44);
    lv_obj_align(btn_mode, LV_ALIGN_TOP_MID, -118, 126);
    lv_obj_add_flag(btn_mode, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_mode, mode_click_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn_mode, mode_click_cb, LV_EVENT_CLICKED, NULL);
    mode_icon = lv_label_create(btn_mode);
    lv_obj_set_style_text_font(mode_icon, &font_icons_28, LV_PART_MAIN);
    lv_label_set_text(mode_icon, MODE_ARROW);
    lv_obj_center(mode_icon);
    mode_one = lv_label_create(btn_mode);
    lv_obj_set_style_text_font(mode_one, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(mode_one, lv_color_hex(C_WHITE), LV_PART_MAIN);
    lv_label_set_text(mode_one, "1");
    lv_obj_center(mode_one);   /* sits between the loop arrows */
    lv_obj_add_flag(mode_one, LV_OBJ_FLAG_HIDDEN);
    mode_refresh(cfg_get_int("work_mode", 0));

    title = lv_label_create(scr);
    lv_obj_set_width(title, 260);
    style_text(title, &s_font20, lv_color_hex(C_WHITE));
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(title, "No Track");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 198);

    artist = lv_label_create(scr);
    lv_obj_set_width(artist, 238);
    style_text(artist, &s_font16, lv_color_hex(C_SECONDARY));
    lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(artist, "");
    lv_obj_align(artist, LV_ALIGN_TOP_MID, 0, 226);

    album = lv_label_create(scr);
    lv_obj_set_width(album, 218);
    style_text(album, &s_font14, lv_color_hex(C_TERTIARY));
    lv_label_set_long_mode(album, LV_LABEL_LONG_DOT);
    lv_label_set_text(album, "");
    lv_obj_align(album, LV_ALIGN_TOP_MID, 0, 246);

    btn_prev = lv_label_create(scr);
    style_text(btn_prev, &lv_font_montserrat_28, lv_color_hex(C_TERTIARY));
    lv_label_set_text(btn_prev, LV_SYMBOL_PREV);
    lv_obj_align(btn_prev, LV_ALIGN_TOP_MID, -60, 284);

    btn_pp = lv_label_create(scr);
    style_text(btn_pp, &lv_font_montserrat_32, accent);
    lv_label_set_text(btn_pp, LV_SYMBOL_PLAY);
    lv_obj_align(btn_pp, LV_ALIGN_TOP_MID, 0, 280);

    btn_next = lv_label_create(scr);
    style_text(btn_next, &lv_font_montserrat_28, lv_color_hex(C_TERTIARY));
    lv_label_set_text(btn_next, LV_SYMBOL_NEXT);
    lv_obj_align(btn_next, LV_ALIGN_TOP_MID, 60, 284);

    make_clickable(btn_prev, "0201000C0002");
    make_clickable(btn_pp,   "0201000C0000");
    make_clickable(btn_next, "0201000C0001");

    /* Times sit as a centered pair below the transport buttons, inside the
     * arc's bottom gap so they never clash with the green progress fill. */
    t_elapsed = lv_label_create(scr);
    lv_obj_set_width(t_elapsed, 80);   /* fits -999:59:59 at Montserrat 14 */
    style_text(t_elapsed, &lv_font_montserrat_14, lv_color_hex(C_TERTIARY));
    lv_label_set_text(t_elapsed, "0:00");
    lv_obj_align(t_elapsed, LV_ALIGN_TOP_MID, -46, 318);

    lv_obj_t *t_sep = lv_label_create(scr);
    style_text(t_sep, &lv_font_montserrat_14, lv_color_hex(C_LINE_SOFT));
    lv_label_set_text(t_sep, "/");
    lv_obj_align(t_sep, LV_ALIGN_TOP_MID, 0, 318);

    t_remain = lv_label_create(scr);
    lv_obj_set_width(t_remain, 80);   /* fits -999:59:59 at Montserrat 14 */
    style_text(t_remain, &lv_font_montserrat_14, lv_color_hex(C_TERTIARY));
    lv_label_set_text(t_remain, "0:00");
    lv_obj_align(t_remain, LV_ALIGN_TOP_MID, 46, 318);

    /* page dots - Now Playing is the left page, the hub (right swipe) the right */
    for(int i=0;i<2;i++){
        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, i==0 ? -8 : 8, -14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(i==0 ? C_WHITE : C_LINE_SOFT), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, i==0 ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
    }

    /* ---- full-screen album-art overlay (hidden until the cover is tapped) ---- */
    fsart = lv_obj_create(scr);
    lv_obj_remove_style_all(fsart);
    lv_obj_set_size(fsart, 360, 360);
    lv_obj_set_pos(fsart, 0, 0);
    lv_obj_set_style_bg_color(fsart, lv_color_hex(C_BLACK), 0);
    lv_obj_set_style_bg_opa(fsart, LV_OPA_COVER, 0);
    lv_obj_add_flag(fsart, LV_OBJ_FLAG_CLICKABLE);          /* tap anywhere -> close */
    lv_obj_clear_flag(fsart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(fsart, fsart_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(fsart, LV_OBJ_FLAG_HIDDEN);

    fsart_img = lv_image_create(fsart);                    /* the 364px stock cover, centred (bezel crops) */
    lv_obj_center(fsart_img);
    lv_obj_clear_flag(fsart_img, LV_OBJ_FLAG_CLICKABLE);

    /* bottom scrim so title/artist stay legible over any cover */
    lv_obj_t *scrim = lv_obj_create(fsart);
    lv_obj_remove_style_all(scrim);
    lv_obj_set_size(scrim, 360, 130);
    lv_obj_align(scrim, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(C_BLACK), 0);
    lv_obj_set_style_bg_grad_color(scrim, lv_color_hex(C_BLACK), 0);
    lv_obj_set_style_bg_grad_dir(scrim, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(scrim, LV_OPA_TRANSP, 0);  /* transparent at top */
    lv_obj_set_style_bg_grad_opa(scrim, LV_OPA_80, 0);      /* darker at the bottom */
    lv_obj_set_style_bg_opa(scrim, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

    fsart_title = lv_label_create(fsart);
    lv_obj_set_width(fsart_title, 240);   /* round-screen chord at y~310 is ~249px wide; 300 overran the bezel */
    style_text(fsart_title, &s_font20, lv_color_hex(C_WHITE));
    lv_obj_set_style_text_align(fsart_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(fsart_title, LV_LABEL_LONG_DOT);
    lv_obj_align(fsart_title, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_clear_flag(fsart_title, LV_OBJ_FLAG_CLICKABLE);

    fsart_artist = lv_label_create(fsart);
    lv_obj_set_width(fsart_artist, 176);   /* chord at y~334 is only ~186px wide; 280 was clipped by the bezel */
    style_text(fsart_artist, &s_font16, lv_color_hex(C_SECONDARY));
    lv_obj_set_style_text_align(fsart_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(fsart_artist, LV_LABEL_LONG_DOT);
    lv_obj_align(fsart_artist, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_clear_flag(fsart_artist, LV_OBJ_FLAG_CLICKABLE);

    apply_accent();
    ui_set_np_style(cfg_get_int("np_style", 0));
}

void ui_update(const track_state_t *st)
{
    char elapsed_buf[12];   /* room for H:MM:SS (audiobook-length) - mmss now emits hours */
    char remain_core[12];
    char remain_buf[14];    /* "-" + H:MM:SS */
    long dur;
    long pos;
    long remain;
    int32_t progress;

    mode_refresh(cfg_get_int("work_mode", 0));   /* keep in sync with Tune menu */

    if(st == NULL || !st->have_track) {
        last_path[0] = '\0';
        g_stat_path[0] = '\0';   /* clear so a later replay of the same track counts as a new play */
        update_accent_seed(st);   /* no-track -> neutral light grey (resets seed + applies once via the gate) */

        set_label_text_changed(title, "No Track");
        set_label_text_changed(artist, "");
        set_label_text_changed(album, "");
        set_label_text_changed(t_elapsed, "0:00");
        set_label_text_changed(t_remain, "0:00");
        set_label_text_changed(btn_pp, LV_SYMBOL_PLAY);
        art_request_clear();   /* clear art + invalidate any in-flight decode (was a bare fallback) */
        g_np_curpath[0] = '\0'; g_favp_active = 0;   /* no track -> drop any optimistic-favourite hold */
        fav_refresh(0, 0);
        if(btn_sleep) lv_obj_add_flag(btn_sleep, LV_OBJ_FLAG_HIDDEN);   /* no track -> hide the sleep moon too */
        ui_np_close_overlays();   /* no track -> dismiss any open sleep popover */
        g_track_dur = 0;
        set_progress_changed(0);
        return;
    }

    update_cover_for_path(st);   /* first: invalidates stale RAM art on a new album */
    update_accent_seed(st);      /* then: art-accent if THIS track's cover is ready, else text-hash */

    /* count a play once per new track (diskOS play history -> Most-Played / Recently-Played), but
     * only while actually PLAYING (st->state==2). Otherwise a boot-seeded paused track, or skipping
     * through tracks while paused, would inflate play counts + recency for songs never really played. */
    if(st->state==2 && st->path[0] && strcmp(g_stat_path, st->path) != 0){
        copy_cstr(g_stat_path, sizeof g_stat_path, st->path);          /* track identity always (so a music replay after a book still counts) */
        if(!mdb_is_book_path(st->path)) mdb_record_play(st->path);     /* but audiobooks are excluded from play history */
    }

    set_label_text_changed(title, st->title[0] ? st->title : "Untitled");
    set_label_text_changed(artist, st->artist[0] ? st->artist : "");
    /* keep the full-screen art view fresh while it's open: text every update, image only on a
     * genuine track change (a PNG decode is too costly to do every position tick). */
    if(fsart_on){
        fsart_refresh_text();
        if(strcmp(fsart_path, st->path) != 0){ copy_cstr(fsart_path, sizeof fsart_path, st->path); fsart_reload_img(); }
    }
    /* Secondary line. For a BOOK it shows the current chapter (an album tag is meaningless for an
     * audiobook, and the all-songs queue position is noise); for music it's the album, carrying the
     * track position as "Album · N/M". */
    {
        char ab[240];
        if(!book_meta_line(st, ab, sizeof ab)){        /* not a book -> album + position */
            const char *alb = st->album;
            if(alb[0] && st->playing_num[0])
                snprintf(ab, sizeof ab, "%s  \xC2\xB7  %s", alb, st->playing_num);          /* Album · 3/19 */
            else if(alb[0])
                snprintf(ab, sizeof ab, "%s", alb);                                         /* Album */
            else if(st->playing_num[0]){
                const char *sl = strchr(st->playing_num, '/');
                if(sl && sl[1]) snprintf(ab, sizeof ab, "%.*s / %s", (int)(sl - st->playing_num), st->playing_num, sl + 1);  /* 3 / 19 */
                else            snprintf(ab, sizeof ab, "%s", st->playing_num);
            } else ab[0] = '\0';
        }
        set_label_text_changed(album, ab);
    }
    /* honour an optimistic-favourite hold so a1 position frames don't bounce the heart back */
    snprintf(g_np_curpath, sizeof g_np_curpath, "%s", st->path);
    int fav_show = st->is_favorite;
    if(g_favp_active){
        if(strcmp(g_favp_path, st->path) != 0)          g_favp_active = 0;   /* different track -> drop hold */
        else if(st->is_favorite == g_favp_val)          g_favp_active = 0;   /* player confirmed the value */
        else if(lv_tick_elaps(g_favp_set) > 4000)       g_favp_active = 0;   /* no confirm in 4s -> give up */
        else                                            fav_show = g_favp_val;
    }
    fav_refresh(fav_show, 1);

    /* Audiobook transport: -15/+30 skips instead of track prev/next, and no shuffle or favourite
     * (they make no sense for a book; the freed space is reserved for the sleep control later). */
    {
        int is_book = mdb_is_book_path(st->path);
        np_transport_glyphs(is_book);
        if(is_book){
            lv_obj_add_flag(btn_fav, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_mode, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(btn_sleep, LV_OBJ_FLAG_HIDDEN);   /* moon replaces the heart for books */
            int armed = ui_sleep_state(NULL);                   /* accent the moon while a timer is set */
            lv_obj_set_style_text_color(sleep_icon, armed ? accent : lv_color_hex(C_TERTIARY), LV_PART_MAIN);
        } else {
            lv_obj_remove_flag(btn_mode, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_sleep, LV_OBJ_FLAG_HIDDEN);
            ui_np_close_overlays();   /* not a book -> dismiss any open sleep popover (its context is gone) */
        }
    }

    dur = st->duration_ms;
    g_track_dur = dur;
    pos = st->position_ms;
    if(dur < 0) dur = 0;
    if(pos < 0) pos = 0;
    if(dur > 0 && pos > dur) pos = dur;

    /* Seek/progress window: a book's ring + times track the CURRENT CHAPTER (the whole-book
     * ring is unusable - 1 degree can be minutes on a long book). Music uses the whole track. */
    if(!book_chapter_window(st, &g_seek_lo, &g_seek_hi)){ g_seek_lo = 0; g_seek_hi = dur; }
    long span = g_seek_hi - g_seek_lo;
    long rel  = pos - g_seek_lo;
    if(rel < 0) rel = 0;
    if(span > 0 && rel > span) rel = span;

    progress = (span > 0) ? (int32_t)(((long long)rel * 1000LL) / (long long)span) : 0;

    /* seek echo-suppression: while the post-seek hold is active and the player
     * is still streaming a stale (far-from-target) position, keep the arc and
     * times pinned at the seeked target instead of snapping back. */
    if(g_seek_hold_until) {
        long d = pos - g_seek_target_ms; if(d < 0) d = -d;   /* ABSOLUTE ms gap - unaffected by a chapter-window flip at a seek-to-boundary */
        if(lv_tick_get() < g_seek_hold_until && d > 3000) {
            set_label_text_changed(btn_pp, ui_pp_icon_playing(st->state == 2) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            return;   /* ignore this stale echo */
        }
        g_seek_hold_until = 0; g_seek_target_ms = -1;   /* caught up or window lapsed */
    }

    mmss(rel, elapsed_buf, sizeof(elapsed_buf));   /* elapsed within the window (chapter for a book) */
    set_label_text_changed(t_elapsed, elapsed_buf);

    if(span > 0) {
        remain = span - rel;
        if(remain < 0) remain = 0;
        mmss(remain, remain_core, sizeof(remain_core));
        snprintf(remain_buf, sizeof(remain_buf), "-%s", remain_core);
    } else {
        snprintf(remain_buf, sizeof(remain_buf), "0:00");
    }
    set_label_text_changed(t_remain, remain_buf);
    set_progress_changed(progress);

    set_label_text_changed(btn_pp, ui_pp_icon_playing(st->state == 2) ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* ---- volume overlay: a draggable arc on lv_layer_top (shows over any screen).
 * It appears on a hardware-volume change (a714) and is also touch-adjustable -
 * dragging the arc calls ui_set_volume() so you can set the level on-screen
 * instead of hammering the buttons (which double-press into next/prev). */
static lv_obj_t *g_vol_panel, *g_vol_arc, *g_vol_num;
static lv_timer_t *g_vol_timer;
static int g_vol_suppress;   /* 1 while we set the arc programmatically (no echo loop) */
static uint32_t g_vol_last_send;  /* throttle: tick of the last 0715 we sent */
static int g_vol_pending;         /* 1 if a value changed but was throttled (commit on release) */
static void vol_hide_cb(lv_timer_t *t){
    if(g_vol_panel) lv_obj_add_flag(g_vol_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}
/* Throttle volume commits to <=1 per 120ms while dragging (the arc fires
 * VALUE_CHANGED every step - sending each one hammers the player). The on-screen
 * number tracks live; the final value is always committed on RELEASED. */
static void vol_arc_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(g_vol_suppress) return;                       /* programmatic update, ignore */
    int v = lv_arc_get_value(g_vol_arc);
    char b[12]; snprintf(b, sizeof b, "%d/%d", v, VOL_MAX); lv_label_set_text(g_vol_num, b);
    if(code == LV_EVENT_VALUE_CHANGED){
        if(lv_tick_elaps(g_vol_last_send) >= 120){
            ui_set_volume(v); g_vol_last_send = lv_tick_get(); g_vol_pending = 0;
        } else {
            g_vol_pending = 1;                       /* defer to release */
        }
        if(g_vol_timer){ lv_timer_reset(g_vol_timer); lv_timer_resume(g_vol_timer); }
    } else if(code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST){
        /* commit the throttled final value on release OR press-lost (LVGL clears
         * LV_STATE_PRESSED on both - without this a deferred value never sends). */
        if(g_vol_pending){ ui_set_volume(v); g_vol_pending = 0; g_vol_last_send = lv_tick_get(); }
    }
}
void ui_show_volume(int vol)
{
    if(!g_vol_panel){
        lv_obj_t *top = lv_layer_top();
        /* translucent circular backdrop, centred */
        g_vol_panel = lv_obj_create(top);
        lv_obj_remove_style_all(g_vol_panel);
        lv_obj_set_size(g_vol_panel, 200, 200);
        lv_obj_center(g_vol_panel);
        lv_obj_set_style_radius(g_vol_panel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_vol_panel, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_vol_panel, 205, LV_PART_MAIN);
        lv_obj_clear_flag(g_vol_panel, LV_OBJ_FLAG_SCROLLABLE);

        g_vol_arc = lv_arc_create(g_vol_panel);
        lv_obj_set_size(g_vol_arc, 184, 184);
        lv_obj_center(g_vol_arc);
        lv_arc_set_rotation(g_vol_arc, 135);
        lv_arc_set_bg_angles(g_vol_arc, 0, 270);
        lv_arc_set_range(g_vol_arc, 0, VOL_MAX);
        lv_obj_set_style_arc_width(g_vol_arc, 10, LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_vol_arc, 10, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(g_vol_arc, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_vol_arc, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_add_event_cb(g_vol_arc, vol_arc_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(g_vol_arc, vol_arc_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(g_vol_arc, vol_arc_cb, LV_EVENT_PRESS_LOST, NULL);

        lv_obj_t *spk = lv_label_create(g_vol_panel);
        lv_label_set_text(spk, LV_SYMBOL_VOLUME_MAX);
        lv_obj_align(spk, LV_ALIGN_CENTER, 0, -22);
        lv_obj_set_style_text_color(spk, lv_color_hex(0xC7C7CC), LV_PART_MAIN);

        g_vol_num = lv_label_create(g_vol_panel);
        lv_obj_align(g_vol_num, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_text_font(g_vol_num, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(g_vol_num, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

        g_vol_timer = lv_timer_create(vol_hide_cb, 1800, NULL);
        lv_timer_pause(g_vol_timer);
    }
    if(vol < 0) vol = 0; if(vol > VOL_MAX) vol = VOL_MAX;
    /* Don't yank the arc out from under an active finger drag: while the arc is
     * pressed, vol_arc_cb already tracks the live value, and a lagging a714 echo
     * would jump it back. Skip the programmatic value/number set in that case. */
    if(!(lv_obj_get_state(g_vol_arc) & LV_STATE_PRESSED)){
        g_vol_suppress = 1;
        lv_arc_set_value(g_vol_arc, vol);
        g_vol_suppress = 0;
        char b[12]; snprintf(b, sizeof b, "%d/%d", vol, VOL_MAX); lv_label_set_text(g_vol_num, b);
    }
    lv_obj_remove_flag(g_vol_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_vol_panel);
    lv_timer_reset(g_vol_timer);
    lv_timer_resume(g_vol_timer);
}
