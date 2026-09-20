/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "config.h"
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Idle screensaver with selectable STYLES (cfg "saver_style"):
 *   0 Cover   - dim blurred album-art backdrop + digital clock + track (classic)
 *   1 Analog  - round analog clock face (hour/minute hands) + date
 *   2 Minimal - just the big time, centered, black
 *   3 Digital - big clock + date + weather, no art, no track (clean info)
 * Any touch wakes (handled in main.c). Kept cheap: static blit + labels;
 * analog hands recompute only on the ~10s clock tick. */

LV_FONT_DECLARE(font_weather16)
static lv_font_t s_swfont;   /* montserrat_16 + weather-icon fallback */

static lv_obj_t *g_bg;
static lv_obj_t *g_clock;
static lv_obj_t *g_date;
static lv_obj_t *g_weather;
static lv_obj_t *g_track;
static lv_obj_t *g_artist;

/* analog clock */
static lv_obj_t *g_face;
static lv_obj_t *g_tick[12];
static lv_obj_t *g_hour;
static lv_obj_t *g_min;
static lv_obj_t *g_sec;
static lv_obj_t *g_hub;
static lv_point_precise_t g_hpts[2], g_mpts[2], g_spts[2];

/* vinyl (stock-style: just the spinning square cover - no disc/label/spindle) */
static lv_obj_t *g_vinyl;    /* the spinning square cover image */
static int g_vspin = 0;      /* spin state (idempotent) */
static int g_have_track = 0; /* is a track currently loaded? The art savers (Cover backdrop + Vinyl
                                cover) show ONLY when a track is loaded, so an idle player shows
                                nothing (blank/clock) instead of a stale cover. Set by
                                saver_set_track. The spin is separately gated on actually-playing
                                (main.c), so a paused-but-loaded track shows the cover, frozen. */

static int g_style = -1;   /* currently-applied style */

/* Sharp full-bleed vinyl cover: the art worker's cover is only 148px (blurry when
 * upscaled to 360 for a full-bleed spin), so decode the stock player's 364px
 * /usr/data/fiio/cover.jpg into a NATIVE 360px ARGB RAM image ONCE per track. That's
 * both crisp (no upscale) and cheap to rotate (native format, no per-frame scale). */
#define VIN_W 360
static uint8_t *g_vbuf = NULL;       /* 360x360 BGRA (== LVGL ARGB8888 byte order) */
static int g_vbuf_valid = 0;         /* does g_vbuf hold a good decode of the CURRENT track's cover?
                                        A failed/absent decode must NOT display the previous track's
                                        buffer, so visibility + brightness gate on this, not on g_vbuf. */
static lv_image_dsc_t g_vdsc;
static char g_vsig[48] = "";         /* mtime_size of the loaded cover, to skip re-decodes */

static void vinyl_update_vis(void)
{
    if (!g_vinyl) return;
    if (g_style == 4 && g_vbuf && g_vbuf_valid && g_have_track) lv_obj_remove_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);
    else                                                        lv_obj_add_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);
}

/* The vinyl saver is an album-art SHOWCASE, so it should stay at the user's set
 * brightness rather than crushing to the clock-saver dim level (which reads as a
 * washed-out/blurry cover on the panel). It still powers fully off after the
 * screen-off delay, so there's no power regression. */
int saver_wants_bright(void) { return g_style == 4 && g_have_track && g_vbuf_valid; }

/* Decode cover.jpg -> native 360px ARGB into g_vbuf (once per track). The decode is BOUNDED
 * (ffmpeg to a temp raw file via ui_run_bounded, a killable child with a hard timeout) so a stuck
 * decoder can never freeze the LVGL thread - saver_show_sync runs this during the screen transition.
 * Only runs while the vinyl saver is on-screen (idle), so the hitch is invisible. On any failure the
 * current track's cover is marked invalid + hidden (never fall back to the PREVIOUS track's art). */
/* Decode scratch lives in /usr/data (root-owned NAND), NOT world-writable /tmp - so a predictable
 * name can't be pre-planted as a symlink for `ffmpeg -y` to follow. Written only while the vinyl
 * saver is on-screen (idle), so the occasional NAND write is negligible. */
#define VIN_TMP "/usr/data/.diskos_vinyl.bgra"
static void vinyl_load_sharp_cover(void)
{
    if (!g_vinyl || !g_have_track) return;   /* no track loaded -> show no cover, not a stale one */
    struct stat stt;
    if (stat("/usr/data/fiio/cover.jpg", &stt) != 0) {        /* current track has no cover art */
        g_vbuf_valid = 0; g_vsig[0] = 0; vinyl_update_vis(); return;
    }
    char sig[48];
    snprintf(sig, sizeof sig, "%ld_%lld", (long)stt.st_mtime, (long long)stt.st_size);
    if (g_vbuf && g_vbuf_valid && strcmp(sig, g_vsig) == 0) return;  /* already loaded this exact cover */

    if (!g_vbuf) g_vbuf = malloc((size_t)VIN_W * VIN_W * 4);
    if (!g_vbuf) return;
    size_t need = (size_t)VIN_W * VIN_W * 4;
    char *a[] = { "sh", "-c",
                  "ffmpeg -y -loglevel quiet -i /usr/data/fiio/cover.jpg "
                  "-vf scale=360:360 -pix_fmt bgra -f rawvideo " VIN_TMP " 2>/dev/null", NULL };
    ui_decode_lock();                   /* serialize with the NP + prewarm decoders (no two ffmpeg at once) */
    int rc = ui_run_bounded(a, 5000);   /* hard 5s cap; a hung ffmpeg is SIGKILLed, never blocks us */
    ui_decode_unlock();
    size_t got = 0;
    if (rc == 0) {
        FILE *f = fopen(VIN_TMP, "rb");
        if (f) { size_t n; while (got < need && (n = fread(g_vbuf + got, 1, need - got, f)) > 0) got += n; fclose(f); }
    }
    unlink(VIN_TMP);
    if (got != need) {                                         /* decode failed/timed out */
        g_vbuf_valid = 0; g_vsig[0] = 0; vinyl_update_vis(); return;
    }
    g_vbuf_valid = 1;

    memset(&g_vdsc, 0, sizeof g_vdsc);
    g_vdsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    g_vdsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    g_vdsc.header.w      = VIN_W;
    g_vdsc.header.h      = VIN_W;
    g_vdsc.header.stride = VIN_W * 4;
    g_vdsc.data          = g_vbuf;
    g_vdsc.data_size     = need;
    strncpy(g_vsig, sig, sizeof g_vsig - 1); g_vsig[sizeof g_vsig - 1] = 0;

    lv_image_set_src(g_vinyl, &g_vdsc);
    lv_image_set_pivot(g_vinyl, VIN_W / 2, VIN_W / 2);   /* 180,180 -> spin about centre */
    lv_image_set_scale(g_vinyl, 256);                    /* 1x - already 360px, no upscale */
    lv_obj_center(g_vinyl);
    vinyl_update_vis();
}

#define CX 180
#define CY 180

static lv_obj_t *mk(lv_obj_t *p, const lv_font_t *font, lv_color_t c, int y)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_width(l, 320);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mk_hand(lv_obj_t *p, int w, lv_color_t c, lv_point_precise_t *pts)
{
    lv_obj_t *ln = lv_line_create(p);
    lv_obj_set_style_line_width(ln, w, 0);
    lv_obj_set_style_line_color(ln, c, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    pts[0].x = CX; pts[0].y = CY; pts[1].x = CX; pts[1].y = CY - 1;
    lv_line_set_points(ln, pts, 2);
    return ln;
}

static void set_hand(lv_obj_t *ln, lv_point_precise_t *pts, double deg, int len)
{
    double r = deg * 3.14159265 / 180.0;
    pts[0].x = CX; pts[0].y = CY;
    pts[1].x = (lv_value_precise_t)(CX + len * sin(r));
    pts[1].y = (lv_value_precise_t)(CY - len * cos(r));
    lv_line_set_points(ln, pts, 2);
    lv_obj_invalidate(ln);
}

/* 1s tick: animate the analog hands (incl. seconds) only while the analog saver
 * is actually on screen - cheap no-op otherwise (battery). */
static void saver_anim_cb(lv_timer_t *t)
{
    (void)t;
    if (g_style != 1 || screen_current() != SCR_SAVER) return;
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    set_hand(g_hour, g_hpts, (lt.tm_hour % 12) * 30.0 + lt.tm_min * 0.5, 70);
    set_hand(g_min,  g_mpts, lt.tm_min * 6.0, 104);
    set_hand(g_sec,  g_spts, lt.tm_sec * 6.0, 118);
}

/* vinyl spin - idempotent; main.c calls it each loop with the live condition
 * (vinyl style && saver visible && backlight on). Freezes on stop. */
/* Throttle the actual rotation to ~20 Hz: on this GPU-less SoC a full-bleed rotate
 * is close to a full-screen software transform, so applying it every LVGL frame
 * (~30 Hz) is wasteful. Skipping a callback causes NO invalidation, so capping to
 * 50 ms keeps the redraw cost down while ~2°/step stays smooth for a slow spin. */
static void vspin_cb(void *var, int32_t v){
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();
    /* No effective cap (16ms). The AA-on render (~52ms) is the real limiter, giving a
     * consistent ~19fps cadence = smoothest. A throttle NEAR the render time makes the
     * cadence irregular (stutter); one far above it (~10fps) is even but choppy. So we
     * run render-limited. Calm feel comes from the slow 27s/rev, not a lower fps. */
    if (now - last < 16) return;
    last = now;
    lv_image_set_rotation((lv_obj_t *)var, v % 3600);
}
void saver_vinyl_spin(int want)
{
    want = want && g_style == 4 && g_vinyl &&
           !lv_obj_has_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);   /* never spin a hidden/no-cover image */
    if (want == g_vspin) return;
    g_vspin = want;
    if (want) {
        int32_t cur = lv_image_get_rotation(g_vinyl);
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, g_vinyl);
        lv_anim_set_exec_cb(&a, vspin_cb);
        lv_anim_set_values(&a, cur, cur + 3600);
        lv_anim_set_time(&a, 27000);                /* ~27s/rev - slow, calm screensaver spin */
        lv_anim_set_path_cb(&a, lv_anim_path_linear);   /* constant angular speed */
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    } else {
        lv_anim_delete(g_vinyl, vspin_cb);
    }
}

/* show/hide + position elements for a style */
static void relayout(int style)
{
    int analog = (style == 1);
    int cover  = (style == 0);
    int minim  = (style == 2);
    int vinyl  = (style == 4);

    /* backdrop only in Cover AND only when a track is loaded (never reveal a stale cover on an
     * idle player when the style changes or the saver is entered manually) */
    if (g_bg) { if (cover && g_have_track) lv_obj_remove_flag(g_bg, LV_OBJ_FLAG_HIDDEN);
                else                        lv_obj_add_flag(g_bg, LV_OBJ_FLAG_HIDDEN); }

    /* vinyl: just the spinning square cover (shown only in vinyl style + when a cover
     * has been decoded; saver_show_sync/saver_set_track drive the decode + visibility) */
    (void)vinyl;
    vinyl_update_vis();

    /* analog parts */
    lv_obj_t *aparts[] = { g_face, g_hour, g_min, g_sec, g_hub };
    for (unsigned i = 0; i < sizeof(aparts)/sizeof(aparts[0]); i++)
        if (aparts[i]) { if (analog) lv_obj_remove_flag(aparts[i], LV_OBJ_FLAG_HIDDEN);
                         else        lv_obj_add_flag(aparts[i], LV_OBJ_FLAG_HIDDEN); }
    for (int i = 0; i < 12; i++)
        if (g_tick[i]) { if (analog) lv_obj_remove_flag(g_tick[i], LV_OBJ_FLAG_HIDDEN);
                         else         lv_obj_add_flag(g_tick[i], LV_OBJ_FLAG_HIDDEN); }

    /* digital clock: shown in every style except analog + vinyl */
    if (g_clock) {
        if (analog || vinyl) lv_obj_add_flag(g_clock, LV_OBJ_FLAG_HIDDEN);
        else { lv_obj_remove_flag(g_clock, LV_OBJ_FLAG_HIDDEN);
               lv_obj_align(g_clock, LV_ALIGN_TOP_MID, 0, minim ? 150 : 110); }
    }
    /* date: every style except vinyl, position varies */
    if (g_date) {
        if (vinyl) lv_obj_add_flag(g_date, LV_OBJ_FLAG_HIDDEN);
        else { lv_obj_remove_flag(g_date, LV_OBJ_FLAG_HIDDEN);
               lv_obj_align(g_date, LV_ALIGN_TOP_MID, 0, analog ? 250 : (minim ? 206 : 170)); }
    }
    /* weather: Cover + Digital */
    if (g_weather) { if (cover || style == 3) lv_obj_remove_flag(g_weather, LV_OBJ_FLAG_HIDDEN);
                     else lv_obj_add_flag(g_weather, LV_OBJ_FLAG_HIDDEN); }
    /* track/artist: Cover only */
    if (g_track)  { if (cover) lv_obj_remove_flag(g_track, LV_OBJ_FLAG_HIDDEN);  else lv_obj_add_flag(g_track, LV_OBJ_FLAG_HIDDEN); }
    if (g_artist) { if (cover) lv_obj_remove_flag(g_artist, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(g_artist, LV_OBJ_FLAG_HIDDEN); }
}

void saver_create(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    g_bg = lv_image_create(root);
    lv_obj_set_size(g_bg, 360, 360);
    lv_obj_align(g_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_image_recolor(g_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_image_recolor_opa(g_bg, 185, 0);
    lv_obj_add_flag(g_bg, LV_OBJ_FLAG_HIDDEN);

    /* analog face: 12 hour ticks around a circle + hands + hub */
    for (int i = 0; i < 12; i++) {
        lv_obj_t *d = lv_obj_create(root);
        lv_obj_remove_style_all(d);
        int big = (i % 3 == 0);
        lv_obj_set_size(d, big ? 8 : 5, big ? 8 : 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(big ? 0xFFFFFF : 0x8E8E93), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        double r = i * 30 * 3.14159265 / 180.0;
        int x = (int)(CX + 150 * sin(r)), y = (int)(CY - 150 * cos(r));
        lv_obj_set_pos(d, x - (big?4:2), y - (big?4:2));
        g_tick[i] = d;
    }
    g_hour = mk_hand(root, 6, lv_color_hex(0xFFFFFF), g_hpts);
    g_min  = mk_hand(root, 4, lv_color_hex(0xC7C7CC), g_mpts);
    g_sec  = mk_hand(root, 2, ui_current_accent(), g_spts);
    g_hub  = lv_obj_create(root);
    lv_obj_remove_style_all(g_hub);
    lv_obj_set_size(g_hub, 14, 14);
    lv_obj_set_style_radius(g_hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_hub, ui_current_accent(), 0);
    lv_obj_set_style_bg_opa(g_hub, LV_OPA_COVER, 0);
    lv_obj_set_pos(g_hub, CX - 7, CY - 7);

    /* vinyl: STOCK-STYLE - the plain SQUARE album cover free-rotates; the round
     * screen cuts its corners (black shows at the corners mid-tilt). No record
     * graphic (no disc/label/spindle) - copies stock, not our earlier invention.
     * g_vdisc/g_vlabel/g_vhole are intentionally left unused (NULL). */
    g_vinyl = lv_image_create(root);
    lv_obj_center(g_vinyl);
    /* AA OFF for FASTER frames: measured render is ~19ms with AA off (~50fps) vs ~52ms
     * with AA on (~19fps). On the native-res + bright cover the nearest-neighbour edges
     * are acceptable, and the ~50fps makes the spin much smoother. */
    lv_image_set_antialias(g_vinyl, false);   /* pivot + scale set per-cover in saver_set_track */
    lv_obj_add_flag(g_vinyl, LV_OBJ_FLAG_HIDDEN);

    s_swfont = lv_font_montserrat_16;
    s_swfont.fallback = &font_weather16;

    g_clock   = mk(root, &lv_font_montserrat_40, lv_color_hex(0xFFFFFF), 110);
    g_date    = mk(root, &lv_font_montserrat_16, lv_color_hex(0xC7C7CC), 170);
    g_weather = mk(root, &s_swfont,              lv_color_hex(0xC7C7CC), 200);
    g_track   = mk(root, &lv_font_montserrat_16, lv_color_hex(0xFFFFFF), 254);
    g_artist  = mk(root, &lv_font_montserrat_14, lv_color_hex(0x8E8E93), 278);
    lv_label_set_text(g_clock, "--:--");

    relayout(cfg_get_int("saver_style", 0));
    g_style = cfg_get_int("saver_style", 0);
    lv_timer_create(saver_anim_cb, 1000, NULL);   /* analog seconds hand */
}

/* repaint the saver's accent-bearing decorations (analog second hand + hub, vinyl
 * record label) with the live accent - called from ui.c apply_accent so they match the
 * rest of the UI instead of staying hardcoded pink. Elements are created once, so the
 * pointers are always valid. */
void saver_set_accent(lv_color_t c)
{
    if (g_sec)    lv_obj_set_style_line_color(g_sec, c, 0);
    if (g_hub)    lv_obj_set_style_bg_color(g_hub, c, 0);
}

/* apply the saved style + current accent immediately when the saver is shown, rather
 * than waiting for the next clock tick (which is where style changes were applied). */
void saver_show_sync(void)
{
    int s = cfg_get_int("saver_style", 0);
    if (s != g_style) { relayout(s); g_style = s; }
    saver_set_accent(ui_current_accent());
    /* decode the sharp cover as the vinyl saver appears - but only if a track is loaded;
     * otherwise make sure the vinyl stays hidden (no stale cover on an idle player). */
    if (g_style == 4) { if (g_have_track) vinyl_load_sharp_cover(); else vinyl_update_vis(); }
}

void saver_set_clock(const char *t, const char *date)
{
    /* re-apply layout if the style setting changed */
    int s = cfg_get_int("saver_style", 0);
    if (s != g_style) { relayout(s); g_style = s; }

    if (g_clock) lv_label_set_text(g_clock, t ? t : "--:--");
    if (g_date)  lv_label_set_text(g_date, date ? date : "");

    /* analog hands from the live time */
    if (g_style == 1) {
        time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
        set_hand(g_hour, g_hpts, (lt.tm_hour % 12) * 30.0 + lt.tm_min * 0.5, 70);
        set_hand(g_min,  g_mpts, lt.tm_min * 6.0, 104);
    }
}

void saver_set_weather(const char *text)
{
    if (g_weather) lv_label_set_text(g_weather, text ? text : "");
}

void saver_set_track(const char *title, const char *artist, const void *backdrop_src)
{
    /* The caller passes a non-NULL title iff st.have_track (NULL when no track), so key off
     * NULL-ness, not emptiness - a valid but untitled file still counts as a loaded track. */
    g_have_track = (title != NULL);
    if (g_track)  lv_label_set_text(g_track, title ? title : "");
    if (g_artist) lv_label_set_text(g_artist, artist ? artist : "");

    /* vinyl cover: re-decode the sharp 360px cover only when the vinyl saver is
     * actually on-screen (track auto-advanced while idle); otherwise just refresh
     * visibility - the decode runs on the next saver show. Never run ffmpeg for a
     * track change while NOT showing the vinyl saver (would hitch the live UI). */
    if (g_vinyl) {
        if (g_style == 4 && screen_current() == SCR_SAVER) vinyl_load_sharp_cover();
        else                                               vinyl_update_vis();
    }

    if (g_bg) {
        if (g_have_track && backdrop_src) {
            lv_image_set_src(g_bg, backdrop_src);
            if (g_style == 0) lv_obj_remove_flag(g_bg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(g_bg);
        } else {
            lv_obj_add_flag(g_bg, LV_OBJ_FLAG_HIDDEN);   /* no track -> no backdrop art */
        }
    }
}
