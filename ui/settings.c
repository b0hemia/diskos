/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "config.h"
#include "anim.h"
#include "musicdb.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>   /* system() for the stock-UI switch */
#include <errno.h>

/* ---- setting model ------------------------------------------------------ */
typedef enum { ST_TOGGLE, ST_SLIDER, ST_CYCLER, ST_READONLY, ST_ACTION } st_type_t;

typedef struct setting_s {
    const char *group;
    const char *label;
    st_type_t   type;
    const char *cfg_key;
    int         min, max, step;            /* slider */
    const char *const *opts; int nopts;    /* cycler */
    const char *ro_val;                    /* readonly */
    void (*apply)(int v);                  /* optional apply hook / decode seam */
    int  def;
    const char *desc;                      /* detail-page description */
    const char *const *opt_descs;          /* per-option descriptions (cyclers) */
} setting_t;

/* apply hooks (decode seams live in main.c) */
void screen_set_anim(int on);
void ui_set_workmode(int mode);
int ui_apply_eq(int preset);
void ui_clock_refresh(void);
void ui_set_accent_config(int mode, int rgb);   /* accent: 0=dynamic / 1=static(rgb) */
void ui_set_prewarm_mode(int m);                /* art cache: 0=off 1=idle 2=idle&charging */
void ui_set_dre(int on);                        /* audio cluster (main.c) */
void ui_set_gain(int high);
void ui_set_output(int spdif);
void ui_set_dac_filter(int idx);
void ui_set_gapless(int on);
void ui_set_memory(int mode);
void ui_set_maxvol(int v);
void ui_set_balance(int v);

static void apply_open_colorpick(int v){ (void)v; colorpick_open(); }   /* seed sliders from cfg + open */
static void apply_debug_mode(int v){ (void)v; debug_open(); }           /* Settings -> System -> Debug Mode */
static void apply_artcache(int v){ ui_set_prewarm_mode(v); }

static void apply_swipe(int v){ ui_apply_swipe_thresh(v); }   /* live; persisted on slider release */
static void apply_anim(int v){ screen_set_anim(v); }
static void apply_weather(int v){ weather_set_enabled(v); }
static void apply_mode(int v){ if(ui_book_active()) return; ui_set_workmode(v); }   /* an active book stays in Single; cfg still updates for later music */
static void apply_eq(int v){ ui_eq_select(v); }   /* central: also records eq_last so the drawer A/B stays in sync */
/* audio cluster: v<0 = "System default" (unmanaged) -> ui_set_* no-ops, leaving the player's
 * own state untouched. A real value sends the live command; persistence is via cfg + the
 * player-ready re-apply in main.c (ui_reapply_audio). */
static void apply_dre(int v){ ui_set_dre(v); }
static void apply_replay_gain(int v){ ui_set_replay_gain(v); }
static void apply_gain(int v){ ui_set_gain(v); }
static void apply_dac_filter(int v){ ui_set_dac_filter(v); }
static void apply_gapless(int v){ ui_set_gapless(v); }
static void apply_memory(int v){ ui_set_memory(v); }
static void apply_maxvol(int v){ ui_set_maxvol(v); }
static void apply_balance(int v){ ui_set_balance(v); }
static void apply_time(int v){ (void)v; ui_clock_refresh(); }
static void apply_sleep(int idx){
    static const int M[] = {0,15,30,45,60,90};
    ui_set_sleep_timer((idx>=0 && idx<6) ? M[idx] : 0);
}
static void apply_np_style(int v){ ui_set_np_style(v); }
static void apply_rescan(int v){ (void)v;
    ui_rescan_library();
    /* 0622 has no completion oracle, so we can't say when/whether it finished -
     * acknowledge the REQUEST honestly rather than implying completion. */
    ui_toast("Rescan requested");
}
static void apply_import_m3u(int v){ (void)v;
    int n = mdb_import_m3u_sd("/tmp/sdcard");   /* root + case-insensitive Music/Playlist(s) subdirs */
    char b[48];
    if(n <= 0) snprintf(b, sizeof b, "No new playlists found");
    else       snprintf(b, sizeof b, "Imported %d playlist%s", n, n==1 ? "" : "s");
    ui_toast(b);
}
static void apply_wifi(int v){ (void)v; wifi_open(); }   /* opens SCR_WIFI */
static void apply_bt(int v){ (void)v; bt_open(); }       /* opens SCR_BT */
static void apply_workmode(int v){ (void)v; modes_open(); }  /* opens SCR_WORKMODE (source-mode picker) */
static void apply_eq_custom(int v){ (void)v; screen_show(SCR_EQ); }  /* opens Custom EQ */
static void apply_qsconfig(int v){ (void)v; screen_show(SCR_QSCONFIG); }  /* opens Quick Settings tile picker */

/* ---- Default-UI preference + Restart ------------------------------------- *
 * Boot model: the user picks a persistent default UI (diskOS or Stock) here;
 * holding Vol-Up at power-on boots the OTHER one for that boot. The boot hook
 * (fiio_init.sh) reads a flag file: /usr/data/boot_default_stock present => the
 * default is Stock; absent => the default is diskOS. We mirror the cycler value
 * to that flag so the choice persists across reboots. */
static void apply_boot_default(int v){
    /* `&&` (not `;`) so the flag write must SUCCEED before sync - with `;` the sync's exit status masked
     * a failed touch/rm (e.g. /usr/data read-only or full), silently diverging the boot flag from the
     * shown setting. Surface the failure so the user knows the boot UI didn't actually change. */
    int rc = (v == 1) ? system("touch /usr/data/boot_default_stock && sync")   /* default = Stock */
                      : system("rm -f /usr/data/boot_default_stock && sync");  /* default = diskOS (rm -f: absent is OK) */
    if(rc != 0) ui_toast("Couldn't change boot UI");
}

static lv_obj_t *g_boot_modal;
static void boot_modal_close(void){ if(g_boot_modal){ lv_obj_delete_async(g_boot_modal); g_boot_modal = NULL; } }
static void boot_cancel_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) boot_modal_close(); }
static void boot_confirm_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    /* Flush the current (NTP-corrected) system time to the RTC BEFORE rebooting. Our periodic
     * hwclock -w only runs every 5 min, and the stock player's RTC save happens on ITS power-off path,
     * not on a UI-triggered reboot - so without this a Restart soon after a time correction leaves the RTC
     * stale and the next boot comes up at the fallback clock (12:00) until NTP re-syncs. Bounded so a busy
     * I2C/RTC can't wedge the reboot. */
    system("hwclock -w 2>/dev/null; sync; reboot");
}
static void boot_modal_pill(lv_obj_t *card, int x, const char *txt, uint32_t col, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(card);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 42); lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -16);
    lv_obj_set_ext_click_area(b, 4);   /* 42px pill -> ~50px touch target */
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
static void apply_restart(int v){ (void)v;
    boot_modal_close();
    g_boot_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_boot_modal);
    lv_obj_set_size(g_boot_modal, 360, 360); lv_obj_center(g_boot_modal);
    lv_obj_set_style_bg_color(g_boot_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_boot_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(g_boot_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_boot_modal, LV_OBJ_FLAG_CLICKABLE);                       /* absorb taps */
    lv_obj_add_event_cb(g_boot_modal, boot_cancel_cb, LV_EVENT_CLICKED, NULL);  /* tap outside = cancel */
    lv_obj_t *card = lv_obj_create(g_boot_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 280, 184); lv_obj_center(card);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, "Restart now?");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *s = lv_label_create(card);
    lv_label_set_text(s, "Boots your default UI. Hold Vol-Up at power-on for the other one.");
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s, 236);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 50);
    boot_modal_pill(card, -58, "Cancel",  0xC7C7CC, boot_cancel_cb);
    boot_modal_pill(card,  58, "Restart", 0xF23260, boot_confirm_cb);
}
/* write the backlight sysfs only (no config change) - used for transient dimming.
 * v==0 fully powers the panel backlight DOWN: writing brightness 0 alone leaves the
 * LED driver enabled at its minimum (screen looks black but the backlight glows), so
 * we also toggle bl_power (0 = FB_BLANK_UNBLANK on, 4 = FB_BLANK_POWERDOWN off). */
void ui_backlight(int v){
    if(v < 0) v = 0; if(v > 40) v = 40;
    if(v > 0){                     /* set target level first, then power on (no flash) */
        FILE *f = fopen("/sys/class/backlight/backlight/brightness", "w");
        if(f){ fprintf(f, "%d", v); fclose(f); }
        else  fprintf(stderr, "backlight brightness open failed: %s\n", strerror(errno));
    }
    FILE *p = fopen("/sys/class/backlight/backlight/bl_power", "w");
    if(p){ fprintf(p, "%d", v ? 0 : 4); fclose(p); }
    else  fprintf(stderr, "backlight bl_power open failed: %s\n", strerror(errno));
}
static void apply_brightness(int v){ if(v < 1) v = 1; ui_backlight(v); }
void settings_apply_startup(void){
    apply_brightness(cfg_get_int("brightness", 16));
    cfg_set_int("sleep_idx", 0);   /* never auto-arm a sleep timer across reboots */
    apply_boot_default(cfg_get_int("boot_default", 0));  /* keep the boot-hook flag in sync */
    ui_set_accent_config(cfg_get_int("accent_mode", 0), cfg_get_int("accent_color", 0xF23260));
    apply_artcache(cfg_get_int("artcache", 0));          /* prewarm off by default */
}

/* shared brightness API (used by Quick Settings too) - persists the value */
void ui_set_brightness(int v){
    if(v < 1) v = 1; if(v > 40) v = 40;
    apply_brightness(v);
    cfg_set_int("brightness", v);
}
int ui_get_brightness(void){
    int v = cfg_get_int("brightness", 16);
    return v < 1 ? 16 : v;   /* never 0/negative: wake paths pass this to ui_backlight,
                              * and 0 would power the panel DOWN while logically awake */
}

static const char *const OPT_MODE[] = { "Sequential", "Shuffle", "Repeat One", "Repeat All", "Single" };
/* 0..10 = built-in presets; 11..20 = the ten user PEQ slots (USER1..USER10), edited in Custom EQ.
 * Index here maps 1:1 to STYLE_PRESET, so the cycler value is exactly what ui_apply_eq() selects. */
static const char *const OPT_EQ[]   = { "Off","Jazz","Rock","R&B","Hip-Hop","Pop","Dance","Classical","Retro","Sibilance 1","Sibilance 2",
                                        "USER1","USER2","USER3","USER4","USER5","USER6","USER7","USER8","USER9","USER10" };
static const char *const OPT_SLEEP[] = { "Off","15 min","30 min","45 min","60 min","90 min" };
static const char *const OPT_POWER[] = { "Off","30 sec","1 min","2 min","5 min" };  /* idx->TMAP secs in main.c */
static const char *const OPT_NPSTYLE[] = { "Cover", "Vinyl" };
static const char *const OPT_ALBUMVIEW[] = { "List", "Cover Flow" };
static const char *const OPT_SAVERSTYLE[] = { "Cover", "Analog", "Minimal", "Digital", "Vinyl" };
static const char *const OPT_BOOTDEF[] = { "diskOS", "Stock" };
static const char *const OPT_ARTCACHE[] = { "Covers only","When Idle","Idle & Charging" };

/* Public EQ preset name lookup (Quick Settings drawer toast, etc.); mirrors OPT_EQ indexing 0..20. */
const char *ui_eq_name(int i){ return (i >= 0 && i < 21) ? OPT_EQ[i] : "Off"; }
/* audio cluster cyclers - all use min=-1 so the value can be "System default" (unmanaged) */
static const char *const OPT_DRE[]    = { "Off", "On" };
static const char *const OPT_REPLAYGAIN[] = { "Off", "Track", "Album" };
static const char *const OPT_GAIN[]   = { "Low", "High" };
static const char *const OPT_DFILTER[]= { "Fast LL","Slow LL","Slow PC","Fast PC","NOS","Wideband" };
static const char *const OPT_MEMORY[] = { "Off", "Position", "Song" };
static const char *const D_ARTCACHE[] = {
    "Album covers preload in the background so browsing stays smooth. Per-track art is decoded only as you play it (default).",
    "Covers preload, plus every track's art is pre-decoded while idle. Decoding can warm the player; it pauses automatically if it gets hot.",
    "Covers preload, plus every track's art is pre-decoded while idle AND charging. Decoding can warm the player; it pauses automatically if it gets hot.",
};

/* per-option descriptions for the cyclers (parallel to the OPT_ arrays) */
static const char *const D_MODE[] = {
    "Play through the list in order, then stop.",
    "Play tracks in a random order.",
    "Repeat the current track over and over.",
    "Loop the whole list when it ends.",
    "Play the current track once, then stop.",
};
static const char *const D_NPSTYLE[] = {
    "Album cover shown as a rounded square.",
    "Spinning vinyl record while a track plays.",
};

static const setting_t TABLE[] = {
    { "Playback", "Play Mode",   ST_CYCLER, "work_mode", 0,0,0, OPT_MODE, 5, NULL, apply_mode, 0,
      "How the player advances through tracks.", D_MODE },
    { "Playback", "Equalizer",   ST_CYCLER, "eq_preset", 0,0,0, OPT_EQ,  21, NULL, apply_eq,   0,
      "Tone preset sent to the player; audible effect is still being verified.", NULL },
    { "Playback", "Custom EQ",    ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_eq_custom, 0,
      "Adjust the 10-band custom EQ. Applies live and saves on the device.", NULL },
    { "Playback", "DSD Output",  ST_READONLY, NULL, 0,0,0, NULL,0, "Auto", NULL, 0,
      "The player auto-selects the DSD mode; this control isn't user-adjustable yet.", NULL },
    { "Playback", "Resume Playback", ST_CYCLER, "memory_play", 0,0,0, OPT_MEMORY, 3, NULL, apply_memory, 0,
      "On power-on: Off = start fresh, Position = resume the exact spot, Song = reopen the last track.", NULL },
    /* Audio/DAC cluster - cyclers with min=-1 so they can read "System default" (unmanaged):
     * until you pick a value diskOS sends nothing + the player keeps its own setting. */
    { "Audio",    "Working Mode", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_workmode, 0,
      "Switch the audio source: local playback, USB DAC, Bluetooth receiving, or USB storage.", NULL },
    { "Audio",    "Gain",        ST_CYCLER, "audio_gain",   0,0,0, OPT_GAIN, 2, NULL, apply_gain, 0,
      "Headphone output gain. High drives demanding headphones louder.", NULL },
    { "Audio",    "DAC Filter",  ST_CYCLER, "audio_filter", 0,0,0, OPT_DFILTER, 6, NULL, apply_dac_filter, 1,
      "CS43131 digital filter roll-off. A subtle tone/transient tradeoff.", NULL },
    { "Audio",    "ReplayGain",  ST_CYCLER, "replay_gain",  0,0,0, OPT_REPLAYGAIN, 3, NULL, apply_replay_gain, 0,
      "Level the volume across tracks. Track uses each song's gain; Album keeps an album's relative dynamics.", NULL },
    { "Audio",    "DRE",         ST_CYCLER, "audio_dre",    0,0,0, OPT_DRE, 2, NULL, apply_dre, 1,
      "Dynamic Range Enhancement (CS43131) for quieter listening.", NULL },
    { "Audio",    "Gapless",     ST_CYCLER, "gapless",      0,0,0, OPT_DRE, 2, NULL, apply_gapless, 0,
      "Play tracks with no silent gap between them.", NULL },
    { "Audio",    "Max Volume",  ST_SLIDER, "max_vol",      10,120,5, NULL,0, NULL, apply_maxvol, 120,
      "Cap the maximum volume level (protects your ears / headphones).", NULL },
    { "Audio",    "Balance",     ST_SLIDER, "balance",      -10,10,1, NULL,0, NULL, apply_balance, 0,
      "Left/right channel balance. 0 = centred; negative = left, positive = right.", NULL },
    /* SPDIF removed: raw 0666 output-route switch wedges the player mid-playback (tears down
     * the local player, g_fiio_local null). Needs the stock stop->switch->resume sequence,
     * not a raw command - revisit if that sequence is decoded. */
    { "Display",  "Brightness",  ST_SLIDER, "brightness", 4,40,2, NULL,0, NULL, apply_brightness, 16,
      "Screen backlight level.", NULL },
    { "Display",  "Quick Settings", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_qsconfig, 0,
      "Choose which tiles appear in the pull-down Quick Settings drawer.", NULL },
    { "Display",  "Now Playing", ST_CYCLER, "np_style",   0,0,0, OPT_NPSTYLE, 2, NULL, apply_np_style, 0,
      "Album art style on the Now Playing screen.", D_NPSTYLE },
    { "Display",  "Album View",  ST_CYCLER, "album_view", 0,0,0, OPT_ALBUMVIEW, 2, NULL, NULL, 0,
      "How the Albums list looks: a text list, or a cover flow you flick through.", NULL },
    { "Display",  "Accent Colour", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_open_colorpick, 0,
      "Now Playing accent: derived from album art, or any fixed colour you pick.", NULL },
    { "Display",  "Album Art Cache", ST_CYCLER, "artcache", 0,0,0, OPT_ARTCACHE, 3, NULL, apply_artcache, 0,
      "Pre-decode album art so covers load instantly. Background decoding can warm the player; it throttles on heat.", D_ARTCACHE },
    { "Display",  "Screensaver", ST_CYCLER, "saver_idx",  0,0,0, OPT_POWER, 5, NULL, NULL, 2,
      "Idle time before the clock screensaver appears and the screen dims.", NULL },
    { "Display",  "Saver Style", ST_CYCLER, "saver_style", 0,0,0, OPT_SAVERSTYLE, 5, NULL, NULL, 0,
      "Screensaver look: Cover art, Analog clock, Minimal, Digital, or spinning Vinyl.", NULL },
    { "Display",  "Screen Off",  ST_CYCLER, "screenoff_idx", 0,0,0, OPT_POWER, 5, NULL, NULL, 3,
      "How long after the screensaver the screen turns fully off.", NULL },
    { "Display",  "24-Hour Time", ST_TOGGLE, "time_24h",  0,1,1, NULL, 0, NULL, apply_time, 1,
      "Use a 24-hour clock instead of AM/PM.", NULL },
    { "Display",  "Animations",  ST_TOGGLE, "anim",      0,1,1, NULL, 0, NULL, apply_anim, 1,
      "Slide animations between screens.", NULL },
    { "Display",  "Weather on Home", ST_TOGGLE, "weather_on", 0,1,1, NULL, 0, NULL, apply_weather, 1,
      "Show the weather glance on the home screen and screensaver (tap it to open the full forecast). Off skips the background fetch to save battery.", NULL },
    { "Display",  "Back-swipe",  ST_SLIDER, "swipe_thresh", 30,120,5, NULL,0, NULL, apply_swipe, 60,
      "Swipe distance needed to go back. Lower is more sensitive.", NULL },
    { "Network",  "Wi-Fi",       ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_wifi, 0,
      "Scan for and connect to Wi-Fi networks.", NULL },
    { "Network",  "Bluetooth", ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_bt, 0,
      "Pair Bluetooth devices. Audio routes to connected headphones or speakers (SBC, beta).", NULL },
    { "System",   "Sleep Timer", ST_CYCLER, "sleep_idx", 0,0,0, OPT_SLEEP, 6, NULL, apply_sleep, 0,
      "Pause playback after this long. Resets on restart.", NULL },
    { "System",   "Rescan Library", ST_ACTION, NULL, 0,0,0, NULL,0, "Scan", apply_rescan, 0,
      "Re-scan the SD card for new or removed music.", NULL },
    { "System",   "Import Playlists", ST_ACTION, NULL, 0,0,0, NULL,0, "Import", apply_import_m3u, 0,
      "Import .m3u / .m3u8 playlists found on the SD card.", NULL },
    { "System",   "Default UI",  ST_CYCLER, "boot_default", 0,0,0, OPT_BOOTDEF, 2, NULL, apply_boot_default, 0,
      "Which UI boots by default. Hold Vol-Up at power-on to boot the other one.", NULL },
    { "System",   "Restart",     ST_ACTION, NULL, 0,0,0, NULL,0, "Restart", apply_restart, 0,
      "Restart the device. Boots your default UI; hold Vol-Up for the other one.", NULL },
    { "System",   "Debug Mode",  ST_ACTION, NULL, 0,0,0, NULL,0, LV_SYMBOL_RIGHT, apply_debug_mode, 0,
      "Enable SSH (random password) + a USB serial root shell for debugging. Off by default.", NULL },
    { "System",   "Temperature", ST_READONLY, NULL, 0,0,0, NULL,0, "@temp", NULL, 0,
      "Battery/board temperature from the fuel gauge (this SoC exposes no core sensor).", NULL },
    { "System",   "About",       ST_READONLY, NULL, 0,0,0, NULL,0, "diskOS beta", NULL, 0,
      "diskOS - a custom music player UI.", NULL },
};
#define N_SETTINGS ((int)(sizeof(TABLE)/sizeof(TABLE[0])))

static int  get_val(const setting_t *s){ return s->cfg_key ? cfg_get_int(s->cfg_key, s->def) : 0; }
static void set_val(const setting_t *s, int v){
    /* EQ persists itself only on a successful send (apply_eq -> ui_eq_select), so a failed send never
     * leaves a stale eq_preset; every other setting persists up front here. */
    if(s->cfg_key && strcmp(s->cfg_key, "eq_preset")) cfg_set_int(s->cfg_key, v);
    if(s->apply) s->apply(v);
}
/* battery/board temp from the fuel gauge; power-supply ABI = tenths of a degree C */
static void read_batt_temp(char *buf, int n){
    FILE *f = fopen("/sys/class/power_supply/cw221X-bat/temp", "r");
    int t;
    if(f && fscanf(f, "%d", &t) == 1){ fclose(f);
        unsigned at = t<0 ? 0u-(unsigned)t : (unsigned)t;   /* unsigned negate: INT_MIN-safe; sign explicit */
        snprintf(buf, n, "%s%u.%u\xC2\xB0""C", t<0?"-":"", at/10, at%10);   /* 305->"30.5°C", -5->"-0.5°C" */
    } else { if(f) fclose(f); snprintf(buf, n, "--"); }
}
/* Format a slider value WITH its unit - shared by val_text (committed value) and the live drag
 * callback (uncommitted value) so brightness shows "%" and swipe-thresh "px" while dragging too. */
static void fmt_slider(const setting_t *s, int v, char *buf, int n){
    if(s->cfg_key && !strcmp(s->cfg_key, "swipe_thresh"))    snprintf(buf,n, "%d px", v);
    else if(s->cfg_key && !strcmp(s->cfg_key, "brightness")) snprintf(buf,n, "%d%%", v*100/40);  /* 4..40 -> 10..100% */
    else                                                    snprintf(buf,n, "%d", v);
}
static void val_text(const setting_t *s, char *buf, int n){
    int v = get_val(s);
    switch(s->type){
        case ST_TOGGLE:   snprintf(buf,n, v?"On":"Off"); break;
        case ST_SLIDER:   fmt_slider(s, v, buf, n); break;
        case ST_CYCLER:   snprintf(buf,n, "%s", (v>=0&&v<s->nopts)?s->opts[v]:"?"); break;
        case ST_READONLY:
            if(s->ro_val && !strcmp(s->ro_val, "@temp")) read_batt_temp(buf, n);
            else snprintf(buf,n, "%s", s->ro_val?s->ro_val:"");
            break;
        case ST_ACTION:   snprintf(buf,n, "%s", s->ro_val?s->ro_val:""); break;
    }
}

/* ---- shared widgets ----------------------------------------------------- */
#define ACCENT 0xFF375F

static lv_obj_t *g_list_rows[N_SETTINGS];   /* value labels, to refresh in place */
static const setting_t *g_active;           /* setting shown in the detail screen */
static lv_obj_t *g_detail_root;
static lv_obj_t *g_setlist_root;            /* SCR_SETLIST root: one category's rows, rebuilt per entry */
static const char *g_active_group;          /* which category SCR_SETLIST is currently showing */
/* Category order for the top-level Settings screen (must match the group strings used in TABLE). */
static const char *const GROUPS[] = { "Playback", "Audio", "Display", "Network", "System" };
#define N_GROUPS ((int)(sizeof(GROUPS)/sizeof(GROUPS[0])))



/* ---- detail screen ------------------------------------------------------ */
static void detail_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

/* VALUE_CHANGED: apply live only (no flash write); RELEASED persists once. This
 * avoids rewriting the whole config file on every pixel of a slider drag. */
static void detail_slider_cb(lv_event_t *e){
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    if(g_active->apply) g_active->apply(v);
    lv_obj_t *vl = lv_event_get_user_data(e);
    if(vl){ char b[16]; fmt_slider(g_active, v, b, sizeof b); lv_label_set_text(vl, b); }   /* keep %/px unit while dragging */
}
static void detail_slider_release_cb(lv_event_t *e){
    lv_obj_t *sl = lv_event_get_target(e);
    if(g_active->cfg_key) cfg_set_int(g_active->cfg_key, lv_slider_get_value(sl));
}
static void detail_cycle_cb(lv_event_t *e){
    int dir = (int)(uintptr_t)lv_event_get_user_data(e);
    int v = get_val(g_active) + dir;
    if(v < 0) v = g_active->nopts-1;
    if(v >= g_active->nopts) v = 0;
    set_val(g_active, v);
    /* rebuild detail body to reflect new value */
    void setting_detail_refresh(void); setting_detail_refresh();
}
static void detail_toggle_cb(lv_event_t *e){
    lv_obj_t *sw = lv_event_get_target(e);
    set_val(g_active, lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

void setting_detail_refresh(void){
    if(!g_detail_root || !g_active) return;
    lv_obj_clean(g_detail_root);
    lv_obj_set_style_bg_color(g_detail_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_detail_root, LV_OPA_COVER, 0);
    ui_header_cb(g_detail_root, g_active->label, detail_back_cb);   /* shared header */

    const setting_t *s = g_active;
    int v = get_val(s);

    if(s->type == ST_SLIDER){
        lv_obj_t *sl = lv_slider_create(g_detail_root);
        lv_obj_set_size(sl, 220, 12);
        lv_obj_set_ext_click_area(sl, 14);
        lv_obj_align(sl, LV_ALIGN_CENTER, 0, -6);
        lv_slider_set_range(sl, s->min, s->max);
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl, lv_color_hex(ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
        lv_obj_t *vl = lv_label_create(g_detail_root);
        char b[16]; val_text(s,b,sizeof b); lv_label_set_text(vl,b);
        lv_obj_set_width(vl, 360); lv_obj_align(vl, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vl, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_font(vl, &lv_font_montserrat_20, 0);
        lv_obj_add_event_cb(sl, detail_slider_cb, LV_EVENT_VALUE_CHANGED, vl);
        lv_obj_add_event_cb(sl, detail_slider_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(sl, detail_slider_release_cb, LV_EVENT_PRESS_LOST, NULL);
    } else if(s->type == ST_TOGGLE){
        lv_obj_t *sw = lv_switch_create(g_detail_root);
        lv_obj_align(sw, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if(v) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, detail_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
    } else if(s->type == ST_CYCLER){
        /* < value > stepper */
        lv_obj_t *vl = lv_label_create(g_detail_root);
        char b[24]; val_text(s,b,sizeof b); lv_label_set_text(vl,b);
        lv_obj_set_width(vl, 220); lv_obj_align(vl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vl, lv_color_hex(ACCENT), 0);
        lv_obj_set_style_text_font(vl, &lv_font_montserrat_24, 0);
        lv_obj_t *lb = lv_button_create(g_detail_root);
        lv_obj_remove_style_all(lb); lv_obj_set_size(lb, 48, 48);
        lv_obj_align(lb, LV_ALIGN_CENTER, -110, 0);
        lv_obj_t *li = lv_label_create(lb); lv_label_set_text(li, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(li, lv_color_hex(0xFFFFFF), 0); lv_obj_center(li);
        lv_obj_add_event_cb(lb, detail_cycle_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)-1);
        lv_obj_t *rb = lv_button_create(g_detail_root);
        lv_obj_remove_style_all(rb); lv_obj_set_size(rb, 48, 48);
        lv_obj_align(rb, LV_ALIGN_CENTER, 110, 0);
        lv_obj_t *ri = lv_label_create(rb); lv_label_set_text(ri, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(ri, lv_color_hex(0xFFFFFF), 0); lv_obj_center(ri);
        lv_obj_add_event_cb(rb, detail_cycle_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)1);
    } else { /* readonly */
        lv_obj_t *vl = lv_label_create(g_detail_root);
        lv_label_set_text(vl, s->ro_val?s->ro_val:"");
        lv_obj_set_width(vl, 360); lv_obj_align(vl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(vl, lv_color_hex(0xC7C7CC), 0);
        lv_obj_set_style_text_font(vl, &lv_font_montserrat_20, 0);
    }

    /* description at the bottom - per-option for cyclers (refreshes on change) */
    const char *desc = s->desc;
    if(s->type==ST_CYCLER && s->opt_descs && v>=0 && v<s->nopts) desc = s->opt_descs[v];
    if(desc && desc[0]){
        lv_obj_t *d = lv_label_create(g_detail_root);
        lv_label_set_text(d, desc);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, 240);
        lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, -40);
        lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(d, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(d, lv_color_hex(0x8E8E93), 0);
    }
}

void setting_detail_create(lv_obj_t *root){ g_detail_root = root; }

/* open a specific setting's detail page directly (verification / deep-link) */
void settings_open_detail(int idx){
    if(idx<0 || idx>=N_SETTINGS) return;
    g_active = &TABLE[idx];
    screen_show(SCR_SETTING_DETAIL);
    setting_detail_refresh();
}

/* open a setting's detail page by its cfg key (robust to TABLE reordering). No-op if not found. */
void settings_open_key(const char *key){
    if(!key) return;
    for(int i=0;i<N_SETTINGS;i++)
        if(TABLE[i].cfg_key && !strcmp(TABLE[i].cfg_key, key)){ settings_open_detail(i); return; }
}

/* ---- master list -------------------------------------------------------- */
static void list_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

static void row_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    const setting_t *s = &TABLE[idx];
    if(s->type == ST_TOGGLE){
        int nv = !get_val(s); set_val(s, nv);
        char b[16]; val_text(s,b,sizeof b);
        if(g_list_rows[idx]) lv_label_set_text(g_list_rows[idx], b);
        return;
    }
    if(s->type == ST_ACTION){
        /* Actions provide their own completion feedback via ui_toast (in-place
         * actions like Rescan/Import) or by navigating to a screen (Wi-Fi/BT).
         * No permanent "Working..." label (it never resolved). */
        if(s->apply) s->apply(0);
        return;
    }
    if(s->type == ST_READONLY) return;   /* info rows (Temperature/About) aren't tappable */
    g_active = s;
    screen_show(SCR_SETTING_DETAIL);
    setting_detail_refresh();
}

/* the scrolling column that holds setting rows, positioned in the round screen's safe area */
static lv_obj_t *make_list(lv_obj_t *root){
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 30, 70);
    lv_obj_set_size(list, 300, 250);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(list, 8, 0);       /* small gap between cards (matches the design) */
    lv_obj_set_style_pad_bottom(list, 44, 0);   /* round bottom bezel: last row must scroll fully clear */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    return list;
}

/* one row card: a plain dark rounded pill (no accent bar - kept clean). */
static lv_obj_t *setting_card(lv_obj_t *list){
    lv_obj_t *row = lv_button_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 288, 50);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/* ---- top-level category list (SCR_SETTINGS): Playback / Audio / Display / Network / System ---- */
static void cat_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int gi = (int)(uintptr_t)lv_event_get_user_data(e);
    if(gi<0 || gi>=N_GROUPS) return;
    g_active_group = GROUPS[gi];
    screen_show(SCR_SETLIST);   /* transition() rebuilds the rows via setlist_refresh */
}

void settings_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Settings", list_back_cb);   /* shared header */

    lv_obj_t *list = make_list(root);
    for(int g=0; g<N_GROUPS; g++){
        lv_obj_t *row = setting_card(list);
        lv_obj_add_event_cb(row, cat_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)g);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, GROUPS[g]);
        lv_obj_set_pos(lbl, 18, 15);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);

        lv_obj_t *ch = lv_label_create(row);   /* drill-down chevron */
        lv_obj_clear_flag(ch, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(ch, LV_SYMBOL_RIGHT);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_text_font(ch, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ch, lv_color_hex(0x8E8E93), 0);
    }
}

/* Category list is static; nothing to re-sync when SCR_SETTINGS is (re)shown. */
void settings_refresh_list(void){ }

/* ---- one category's rows (SCR_SETLIST), rebuilt on every entry so values are always live ---- */
void setlist_create(lv_obj_t *root){ g_setlist_root = root; }

void setlist_refresh(void){
    if(!g_setlist_root || !g_active_group) return;
    lv_obj_clean(g_setlist_root);
    lv_obj_set_style_bg_color(g_setlist_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_setlist_root, LV_OPA_COVER, 0);
    ui_header_cb(g_setlist_root, g_active_group, list_back_cb);   /* title = category; back pops to categories */

    lv_obj_t *list = make_list(g_setlist_root);
    for(int i=0;i<N_SETTINGS;i++){
        const setting_t *s = &TABLE[i];
        if(strcmp(s->group, g_active_group)) continue;   /* only rows in this category */

        lv_obj_t *row = setting_card(list);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, s->label);
        lv_obj_set_pos(lbl, 18, 15);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);

        char b[24]; val_text(s, b, sizeof b);
        lv_obj_t *vl = lv_label_create(row);
        lv_label_set_text(vl, b);
        lv_obj_set_pos(vl, 150, 16);
        lv_obj_set_size(vl, 120, 20);
        lv_obj_set_style_text_align(vl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(vl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(vl, lv_color_hex(s->type==ST_READONLY?0x8E8E93:0xC7C7CC), 0);
        g_list_rows[i] = vl;
    }
}
