/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "config.h"
#include "musicdb.h"
#include <stdint.h>
#include <stdio.h>

/* Custom graphic EQ: 10 bands (32 Hz..16 kHz) + master, -12..+12 dB, matching the stock
 * player's PEQ. The stock player keeps TEN user slots, USER1..USER10 = STYLE_PRESET 11..20;
 * this editor lets you pick and edit any of them, each remembering its own curve. Values
 * persist to per-slot cfg keys (eqN_b0..9, eqN_master, where N = 1..10) AND apply live to
 * the audio engine: on each change we write the curve into that slot's PEQ row in stock
 * format and select it with 0689 (ui_apply_eq). Format captured live from the stock UI -
 * see RE_CATALOGUE section 3. The 10 bands scroll horizontally (round screen can't show
 * them all at once), like the stock editor.
 *
 * Kept the stock graphic-EQ model on purpose: fixed frequencies, peaking filter, fixed Q.
 * Full parametric editing (filterType/frequency/Q) is out of scope - the filterType enum
 * hasn't been reverse-engineered off the device yet. */

int ui_apply_eq(int preset);             /* main.c: 0689 select; <0 = send failed */
#define ACC 0xFF375F
#define NBAND 10
#define SLOT_MIN 11                      /* USER1  = STYLE_PRESET 11 */
#define SLOT_MAX 20                      /* USER10 = STYLE_PRESET 20 */

static const int         FREQHZ[NBAND] = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
static const char *const FREQLBL[NBAND]= { "32","64","125","250","500","1k","2k","4k","8k","16k" };

static int       g_slot = SLOT_MIN;      /* currently-edited USER slot (11..20) */
static lv_obj_t *g_band[NBAND];          /* band sliders */
static lv_obj_t *g_bval[NBAND];          /* band value labels */
static lv_obj_t *g_master;               /* master gain slider */
static lv_obj_t *g_mval;                 /* master value label */
static lv_obj_t *g_slotlbl;              /* "USER1" selector label */
static int       g_curve_known = 1;      /* 0 after a FAILED slot read: the sliders show a placeholder-flat,
                                          * NOT the real curve, so an edit must not persist over the stock one */
static int       g_slot_parametric = 0;  /* 1 when the loaded stock slot uses parametric params (filterType/Q/
                                          * fractional gain) the graphic editor can't represent: block edits so
                                          * an accidental drag doesn't FLATTEN a real stock PEQ preset */
static int       g_slot_foreign = 0;     /* 1 when a slot we own in cfg has DIVERGED from the live DB curve
                                          * (the stock player changed it under us): select it, never overwrite -
                                          * a plain re-select must not clobber the external change */

/* per-slot cfg keys: slot 11 (USER1) -> "eq1_b0".."eq1_b9" + "eq1_master", etc. So every
 * user slot keeps its own 10-band curve + master preamp independently. */
static void band_key(char *buf, int n, int slot, int band){ snprintf(buf, n, "eq%d_b%d", slot-10, band); }
static void master_key(char *buf, int n, int slot){ snprintf(buf, n, "eq%d_master", slot-10); }

/* One-time upgrade: the old single-slot editor stored USER1's curve in the flat keys
 * eq_b0..eq_b9 + eq_master. Seed the new per-slot USER1 keys (eq1_*) from them so an
 * upgraded install keeps its Custom EQ curve. Never overwrites a value the user has already
 * saved under a new key (a new key that is present, even 0, is left alone). A sentinel well
 * outside the -12..12 range distinguishes "absent" from a real 0. */
#define EQ_ABSENT (-1000)
static void migrate_legacy_user1(void){
    for(int i=0;i<NBAND;i++){
        char nk[24]; band_key(nk, sizeof nk, SLOT_MIN, i);
        if(cfg_get_int(nk, EQ_ABSENT) != EQ_ABSENT) continue;   /* already has a new value */
        char ok[16]; snprintf(ok, sizeof ok, "eq_b%d", i);
        int lv = cfg_get_int(ok, EQ_ABSENT);
        if(lv != EQ_ABSENT) cfg_set_int(nk, lv);
    }
    char nmk[24]; master_key(nmk, sizeof nmk, SLOT_MIN);
    if(cfg_get_int(nmk, EQ_ABSENT) == EQ_ABSENT){
        int lm = cfg_get_int("eq_master", EQ_ABSENT);
        if(lm != EQ_ABSENT) cfg_set_int(nmk, lm);
    }
}

/* build the stock PARAMS_JSON from the current band values + write+select the current PEQ
 * slot. Returns 1 if the curve actually reached the player (DB write + select), 0 otherwise. */
static int apply_now(void){
    char json[1100]; int n = 0;
    n += snprintf(json+n, sizeof json-n, "[");
    for(int i=0;i<NBAND;i++){
        if(n < 0 || n > (int)sizeof json - 90) return 0;   /* truncation guard (never hit at 10 bands) */
        int g = g_band[i] ? lv_slider_get_value(g_band[i]) : 0;
        n += snprintf(json+n, sizeof json-n,
            "%s{\"filterType\":0,\"frequency\":%d,\"position\":%d,\"gain\":\"%d.0\",\"qValue\":\"0.7\"}",
            i?",":"", FREQHZ[i], i, g);
    }
    if(n < 0 || n > (int)sizeof json - 2) return 0;
    n += snprintf(json+n, sizeof json-n, "]");
    int master = g_master ? lv_slider_get_value(g_master) : 0;
    if(mdb_set_peq(g_slot, (double)master, json)){
        if(ui_eq_select(g_slot) < 0) return 0;   /* select + persist eq_preset/eq_last; failed = curve never reached the player */
        return 1;
    }
    return 0;
}

/* push explicit band + master values into the sliders and value labels (display only). */
static void show_curve(const int *gains, int master){
    for(int i=0;i<NBAND;i++){
        int v = gains ? gains[i] : 0;
        if(v < -12) v = -12; else if(v > 12) v = 12;
        if(g_band[i]) lv_slider_set_value(g_band[i], v, LV_ANIM_OFF);
        if(g_bval[i]){ char b[8]; snprintf(b,sizeof b,"%+d", v); lv_label_set_text(g_bval[i], v?b:"0"); }
    }
    if(master < -12) master = -12; else if(master > 12) master = 12;
    if(g_master) lv_slider_set_value(g_master, master, LV_ANIM_OFF);
    if(g_mval){ char b[8]; snprintf(b,sizeof b,"%+d", master); lv_label_set_text(g_mval, master?b:"0"); }
}

/* does diskOS already hold a saved curve for this slot? (any band or master key present) */
static int slot_has_cfg(int slot){
    char k[24];
    for(int i=0;i<NBAND;i++){ band_key(k,sizeof k,slot,i); if(cfg_get_int(k,EQ_ABSENT)!=EQ_ABSENT) return 1; }
    master_key(k,sizeof k,slot); return cfg_get_int(k,EQ_ABSENT)!=EQ_ABSENT;
}

/* persist the WHOLE current curve to this slot's cfg keys (one flush). Persisting every band
 * (not just the touched one) captures a stock-derived slot in full the first time it is edited,
 * so its untouched bands keep their real values instead of snapping to 0 on the next select. */
static void persist_slot(int slot){
    char k[24];
    for(int i=0;i<NBAND;i++){ int v=g_band[i]?lv_slider_get_value(g_band[i]):0; band_key(k,sizeof k,slot,i); cfg_set_int_deferred(k,v); }
    int m=g_master?lv_slider_get_value(g_master):0; master_key(k,sizeof k,slot); cfg_set_int_deferred(k,m);
    cfg_flush();
}

/* pull a slot's curve into the sliders (no apply). A diskOS-owned slot comes from cfg; a slot we
 * have never edited shows the player's EXISTING (stock) PEQ curve, so selecting or first-editing
 * it never silently zeroes a stock USER preset. */
static void load_slot(int slot){
    g_slot_parametric = 0;
    g_slot_foreign = 0;
    if(slot_has_cfg(slot)){
        int cg[NBAND]; char k[24];
        for(int i=0;i<NBAND;i++){ band_key(k,sizeof k,slot,i); cg[i]=cfg_get_int(k,0); }
        char mk[24]; master_key(mk,sizeof mk,slot);
        int cm = cfg_get_int(mk,0);
        /* A slot we own in cfg can still be changed underneath us by the stock player (to a different
         * graphic curve, or to a parametric one). Reconcile our cfg against the LIVE DB curve: only when
         * they still agree is cfg authoritative and safe to write back on select. Otherwise treat the slot
         * as foreign - show the DB's current curve and never let a plain re-select overwrite it. */
        int dg[NBAND] = {0}, dm = 0;
        int r  = mdb_get_peq(slot, &dm, dg);           /* 1=found, 0=empty(flat), -1=read failed */
        int gr = (r > 0) ? mdb_peq_is_graphic(slot)    /* 1 graphic / 0 parametric / -1 classify-failed */
                         : (r == 0 ? 1 : -1);          /* empty = flat = graphic; read-fail = unknown */
        if(r < 0 || gr < 0){
            show_curve(cg, cm); g_curve_known = 0; g_slot_foreign = 1;  /* DB read/classify failed: block edit + overwrite */
        } else if(gr == 0){
            show_curve(dg, dm); g_slot_parametric = 1;                  /* changed to PARAMETRIC in stock: show it, */
            g_curve_known = 0;  g_slot_foreign = 1;                     /* block edit AND overwrite */
        } else if(dm == cm && memcmp(dg, cg, sizeof dg) == 0){
            show_curve(cg, cm); g_curve_known = 1;                      /* cfg == DB: still ours, editable + safe to write back */
        } else {
            show_curve(dg, dm); g_curve_known = 1; g_slot_foreign = 1;  /* different GRAPHIC curve in stock: show truth, select-only until an explicit edit adopts it */
        }
    } else {
        int g[NBAND], m=0;
        int r = mdb_get_peq(slot,&m,g);     /* 1=found, 0=empty (flat is real), -1=read failed */
        if(r > 0){
            show_curve(g, m);
            int gr = mdb_peq_is_graphic(slot);   /* 1 = graphic, 0 = parametric, -1 = classify read failed */
            g_slot_parametric = (gr == 0);
            g_curve_known = (gr == 1);           /* ONLY a confirmed-graphic slot is editable; parametric OR a
                                                  * failed classification blocks the edit (never flatten unverified) */
        }
        else if(r == 0) { show_curve(NULL, 0); g_curve_known = 1; }   /* genuinely empty slot: flat IS the curve */
        else            { show_curve(NULL, 0); g_curve_known = 0; }   /* read FAILED: unknown curve, block edits */
    }
}

/* switch the edited slot: show its curve, then apply it live. A diskOS-owned slot is written
 * back + selected; a slot we have never edited is only SELECTED (0689) - never overwritten with
 * a flat curve, which would destroy a stock USER preset. */
static void set_slot(int slot){
    if(slot < SLOT_MIN) slot = SLOT_MIN;
    if(slot > SLOT_MAX) slot = SLOT_MAX;
    g_slot = slot;
    cfg_set_int("eq_slot", g_slot);
    if(g_slotlbl){ char b[16]; snprintf(b,sizeof b,"USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b); }
    load_slot(g_slot);
    if(slot_has_cfg(g_slot) && !g_slot_foreign){
        if(!apply_now()) ui_toast("Couldn't apply EQ");   /* our curve, DB agrees -> write back + select */
    } else if(ui_eq_select(g_slot) < 0){
        ui_toast("Couldn't apply EQ");                    /* stock OR diverged slot -> select only, never overwrite */
    }
}

static void slot_dir_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    set_slot(g_slot + dir);              /* clamps at USER1 / USER10 (no wrap) */
}

/* live preview of the value label (no persist/apply on every drag tick) */
static void band_cb(lv_event_t *e){
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_obj_t *lbl = (i==NBAND) ? g_mval : (i>=0 && i<NBAND ? g_bval[i] : NULL);
    if(lbl){ char b[8]; snprintf(b,sizeof b,"%+d", v); lv_label_set_text(lbl, v?b:"0"); }
}
/* persist this control (to the current slot) + apply the whole curve when the finger lifts */
static void band_release_cb(lv_event_t *e){
    (void)e;
    /* Blocked edit: either a stock PARAMETRIC preset (which the 10-band graphic editor would flatten), or a
     * slot whose stock curve we FAILED to read (persisting the placeholder-flat would clobber it). Either
     * way, drop the edit and reload the real curve rather than destroy it. */
    if(!g_curve_known){
        ui_toast(g_slot_parametric ? "Advanced preset - edit it on the stock player" : "EQ read failed, try again");
        load_slot(g_slot); return;
    }
    /* apply first; persist the whole curve only once it actually reached the player, so a PEQ
     * write/select failure can't leave cfg/UI showing a band the audio engine never received.
     * Persisting every band (not just the touched one) captures a stock-derived slot in full on
     * its first edit, so the untouched bands keep their real values. */
    if(apply_now()) persist_slot(g_slot);
    else ui_toast("Couldn't apply EQ");
}

static void flat_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    /* LV_ANIM_OFF so the slider VALUES are actually 0 when apply_now() reads them below - with
     * LV_ANIM_ON the sliders are still mid-animation and apply_now() would send the old/intermediate
     * curve to the player while cfg + labels already said 0. */
    for(int i=0;i<NBAND;i++){
        lv_slider_set_value(g_band[i], 0, LV_ANIM_OFF);
        if(g_bval[i]) lv_label_set_text(g_bval[i], "0");
    }
    lv_slider_set_value(g_master, 0, LV_ANIM_OFF);
    if(g_mval) lv_label_set_text(g_mval, "0");
    /* apply FIRST; persist the flat curve (to the current slot) only if it reached the player.
     * Flat is an explicit user choice, so it is allowed even after a failed read - and it makes the
     * slot's curve known again (flat), so a following band edit persists normally. */
    if(apply_now()){ persist_slot(g_slot); g_curve_known = 1; }
    else ui_toast("Couldn't apply EQ");
}

/* one EQ column: value label on top, vertical slider, freq label below. idx==NBAND = master.
 * Built inside the horizontal scroller. Slider starts at 0; load_slot() fills the real value. */
static void make_col(lv_obj_t *parent, int idx, const char *flabel,
                     lv_obj_t **slot_slider, lv_obj_t **slot_val){
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 42, 206);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *val = lv_label_create(col);
    lv_obj_set_pos(val, 0, 0); lv_obj_set_width(val, 42);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(ACC), 0);
    lv_label_set_text(val, "0");

    lv_obj_t *sl = lv_slider_create(col);
    lv_obj_set_size(sl, 16, 150);
    lv_obj_set_ext_click_area(sl, 8);
    lv_obj_set_pos(sl, 13, 22);
    lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);   /* fill from 0 dB centre */
    lv_slider_set_range(sl, -12, 12);
    lv_slider_set_value(sl, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(ACC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_event_cb(sl, band_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(sl, band_release_cb, LV_EVENT_RELEASED, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(sl, band_release_cb, LV_EVENT_PRESS_LOST, (void*)(intptr_t)idx);

    lv_obj_t *fl = lv_label_create(col);
    lv_obj_set_pos(fl, 0, 182); lv_obj_set_width(fl, 42);
    lv_label_set_text(fl, flabel);
    lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fl, lv_color_hex(idx==NBAND ? 0xC7C7CC : 0x8E8E93), 0);

    *slot_slider = sl;
    *slot_val = val;
}

/* small "< USERn >" slot selector; two 44px-touch arrows around a centred label. */
static void make_slot_selector(lv_obj_t *root){
    lv_obj_t *sel = lv_obj_create(root);
    lv_obj_remove_style_all(sel);
    lv_obj_set_pos(sel, 70, 48); lv_obj_set_size(sel, 220, 32);
    lv_obj_clear_flag(sel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lb = lv_button_create(sel); lv_obj_remove_style_all(lb);
    lv_obj_set_size(lb, 40, 30); lv_obj_align(lb, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_ext_click_area(lb, 8);
    lv_obj_t *li = lv_label_create(lb); lv_label_set_text(li, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(li, lv_color_hex(0xFFFFFF), 0); lv_obj_center(li);
    lv_obj_add_event_cb(lb, slot_dir_cb, LV_EVENT_CLICKED, (void*)(intptr_t)-1);

    g_slotlbl = lv_label_create(sel);
    lv_obj_set_width(g_slotlbl, 120); lv_obj_align(g_slotlbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(g_slotlbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_slotlbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_slotlbl, lv_color_hex(ACC), 0);
    { char b[16]; snprintf(b,sizeof b,"USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b); }

    lv_obj_t *rb = lv_button_create(sel); lv_obj_remove_style_all(rb);
    lv_obj_set_size(rb, 40, 30); lv_obj_align(rb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_t *ri = lv_label_create(rb); lv_label_set_text(ri, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ri, lv_color_hex(0xFFFFFF), 0); lv_obj_center(ri);
    lv_obj_add_event_cb(rb, slot_dir_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
}

/* Re-resolve the edited slot on every entry (display-only, no audio re-apply): if a USER slot is the
 * live preset, edit that one; else keep the last-edited slot. Without this the editor stays on the slot
 * it was BUILT with, so selecting a different USER slot via Tune/Settings would edit the wrong curve. */
void eqcustom_refresh(void){
    if(!g_slotlbl) return;   /* not built yet */
    int init = cfg_get_int("eq_slot", SLOT_MIN);
    int pre  = cfg_get_int("eq_preset", -1);
    if(pre >= SLOT_MIN && pre <= SLOT_MAX) init = pre;
    if(init < SLOT_MIN || init > SLOT_MAX) init = SLOT_MIN;
    g_slot = init;
    cfg_set_int("eq_slot", g_slot);
    char b[16]; snprintf(b, sizeof b, "USER%d", g_slot-10); lv_label_set_text(g_slotlbl, b);
    load_slot(g_slot);       /* pull the slot's curve into the sliders WITHOUT applying */
}

void eqcustom_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    migrate_legacy_user1();   /* carry a pre-slots Custom EQ curve into USER1 (one-time, non-destructive) */

    /* Resolve which slot to edit: if a USER slot is the live preset, edit that one; else fall
     * back to the editor's last-edited slot; default USER1. */
    int init = cfg_get_int("eq_slot", SLOT_MIN);
    int pre  = cfg_get_int("eq_preset", -1);
    if(pre >= SLOT_MIN && pre <= SLOT_MAX) init = pre;
    if(init < SLOT_MIN || init > SLOT_MAX) init = SLOT_MIN;
    g_slot = init;
    cfg_set_int("eq_slot", g_slot);

    ui_header(root, "Custom EQ");   /* shared standard header */

    make_slot_selector(root);

    /* horizontally-scrollable row of EQ columns (master + 10 bands) */
    lv_obj_t *scr = lv_obj_create(root);
    lv_obj_remove_style_all(scr);
    lv_obj_set_pos(scr, 0, 80);
    lv_obj_set_size(scr, 360, 212);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scr, LV_DIR_HOR);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_left(scr, 20, 0);
    lv_obj_set_style_pad_right(scr, 20, 0);
    lv_obj_set_style_pad_column(scr, 4, 0);

    make_col(scr, NBAND, "MSTR", &g_master, &g_mval);    /* master first */
    for(int i=0;i<NBAND;i++)
        make_col(scr, i, FREQLBL[i], &g_band[i], &g_bval[i]);

    load_slot(g_slot);   /* populate sliders from the resolved slot (no live apply on open) */

    lv_obj_t *flat = lv_button_create(root);
    lv_obj_remove_style_all(flat);
    lv_obj_set_size(flat, 120, 34); lv_obj_align(flat, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_set_ext_click_area(flat, 6);   /* 34px pill -> ~46px touch target */
    lv_obj_set_style_radius(flat, 17, 0);
    lv_obj_set_style_bg_color(flat, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(flat, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(flat, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_add_event_cb(flat, flat_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fll=lv_label_create(flat); lv_label_set_text(fll, "Flat");
    lv_obj_set_style_text_font(fll, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fll, lv_color_hex(0xFFFFFF), 0); lv_obj_center(fll);
}
