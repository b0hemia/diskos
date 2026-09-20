/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include <stdio.h>
#include <string.h>

/* On-screen keyboard shaped to the bottom SEMICIRCLE of the 360px round panel
 * the natural QWERTY taper (10/9/7 keys) already
 * follows the circle's narrowing chord, so each row is centred to the chord at
 * its bottom edge and every key stays >=32px and fully inside the circle. The
 * top half holds the field + Cancel/Save. Individually-positioned lv_buttons -
 * no per-touch math, cheap on MIPS. */

/* ---- semicircle keyboard ------------------------------------------------- */
#define SKB_W 360
#define SKB_Y 180            /* keyboard occupies y 180..360 (bottom half)      */
#define SKB_H 180
#define SKB_ROW_H 32
#define SKB_MAX_KEYS 31

typedef enum { SKB_LOWER, SKB_UPPER, SKB_NUM, SKB_SYM } skb_mode_t;
typedef enum { SKB_CHAR, SKB_SHIFT, SKB_MODE, SKB_SPACE, SKB_BKSP, SKB_OK } skb_kind_t;

typedef struct skb_s skb_t;
typedef struct { skb_t *ctx; skb_kind_t kind; uint8_t row, col; lv_obj_t *btn, *label; } skb_key_t;
struct skb_s {
    lv_obj_t *kb, *ta;
    void (*submit)(void);
    skb_mode_t mode;
    skb_key_t keys[SKB_MAX_KEYS];
    uint8_t key_count;
};

static const char skb_lower[3][11] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char skb_upper[3][11] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
static const char skb_numr [3][11] = { "1234567890", "-/:;()$&@", ".,?!'\"+" };
/* second symbol page (iOS "#+=" style) - the chars not on the number page,
 * incl. the ones Wi-Fi passwords commonly need: _ # % = * ^ etc. All printable
 * ASCII, so montserrat renders them. Row lengths must match skb_rows (10/9/7). */
static const char skb_syms [3][11] = { "[]{}#%^*+=", "_\\|~<>/-:", ";()&$!?" };

typedef struct { int16_t y; uint8_t count, gap, key_w; const uint8_t *widths; } skb_rowl_t;
static const uint8_t skb_util_w[5] = { 32, 32, 52, 32, 32 };  /* Aa 123 space bksp OK */
static const skb_rowl_t skb_rows[4] = {
    {184, 10, 3, 32, NULL},          /* qwertyuiop  total 347 (chord ~353) */
    {222,  9, 3, 33, NULL},          /* asdfghjkl   total 321 (chord ~329) */
    {260,  7, 4, 36, NULL},          /* zxcvbnm     total 276 (chord ~283) */
    {298,  5, 3,  0, skb_util_w},    /* utility     total 192 (chord ~199) */
};

static int16_t skb_row_w(const skb_rowl_t *r){
    int16_t w = 0;
    for(uint8_t i=0;i<r->count;i++) w += r->widths ? r->widths[i] : r->key_w;
    if(r->count>1) w += (int16_t)(r->count-1)*r->gap;
    return w;
}
static char skb_char(const skb_t *c, uint8_t row, uint8_t col){
    if(c->mode==SKB_SYM)   return skb_syms [row][col];
    if(c->mode==SKB_NUM)   return skb_numr [row][col];
    if(c->mode==SKB_UPPER) return skb_upper[row][col];
    return skb_lower[row][col];
}
static void skb_refresh(skb_t *c){
    for(uint8_t i=0;i<c->key_count;i++){
        skb_key_t *k=&c->keys[i];
        switch(k->kind){
        case SKB_CHAR: { char s[2]={ skb_char(c,k->row,k->col), 0 }; lv_label_set_text(k->label,s); break; }
        case SKB_SHIFT:
            /* on letter pages = case shift; on number/symbol pages = toggle the
             * two symbol pages (NUM <-> SYM), iOS "#+=" / "123" style. */
            if(c->mode==SKB_NUM){      lv_label_set_text(k->label,"#+="); lv_obj_remove_state(k->btn,LV_STATE_CHECKED); }
            else if(c->mode==SKB_SYM){ lv_label_set_text(k->label,"123"); lv_obj_remove_state(k->btn,LV_STATE_CHECKED); }
            else {
                lv_label_set_text(k->label,"Aa");
                if(c->mode==SKB_UPPER) lv_obj_add_state(k->btn,LV_STATE_CHECKED);
                else                   lv_obj_remove_state(k->btn,LV_STATE_CHECKED);
            }
            break;
        case SKB_MODE:  lv_label_set_text(k->label,
                            (c->mode==SKB_NUM||c->mode==SKB_SYM)?"abc":"123"); break;
        case SKB_SPACE: lv_label_set_text(k->label,"space"); break;
        case SKB_BKSP:  lv_label_set_text(k->label,LV_SYMBOL_BACKSPACE); break;
        case SKB_OK:    lv_label_set_text(k->label,LV_SYMBOL_OK); break;
        }
        lv_obj_center(k->label);
    }
}
static void skb_key_cb(lv_event_t *e){
    lv_event_code_t code = lv_event_get_code(e);
    skb_key_t *k=lv_event_get_user_data(e); skb_t *c=k->ctx;
    if(code == LV_EVENT_LONG_PRESSED){
        if(k->kind == SKB_BKSP) lv_textarea_set_text(c->ta, "");   /* hold backspace = clear the whole field */
        return;
    }
    if(code != LV_EVENT_CLICKED) return;
    switch(k->kind){
    case SKB_CHAR: {
        char s[2]={ skb_char(c,k->row,k->col), 0 };
        lv_textarea_add_text(c->ta,s);
        if(c->mode==SKB_UPPER){ c->mode=SKB_LOWER; skb_refresh(c); }
        break; }
    case SKB_SHIFT:
        if(c->mode==SKB_NUM)      c->mode = SKB_SYM;            /* number page -> symbol page */
        else if(c->mode==SKB_SYM) c->mode = SKB_NUM;            /* symbol page -> number page */
        else c->mode = (c->mode==SKB_UPPER)?SKB_LOWER:SKB_UPPER;/* letter case shift */
        skb_refresh(c); break;
    case SKB_MODE:  /* "123"/"abc": jump between letters and the number/symbol pages */
        c->mode = (c->mode==SKB_NUM||c->mode==SKB_SYM)?SKB_LOWER:SKB_NUM; skb_refresh(c); break;
    case SKB_SPACE: lv_textarea_add_text(c->ta," "); break;
    case SKB_BKSP:  lv_textarea_delete_char(c->ta); break;
    case SKB_OK:    if(c->submit) c->submit(); break;
    }
}
static void skb_del_cb(lv_event_t *e){ lv_free(lv_event_get_user_data(e)); }
static void skb_add(skb_t *c, skb_kind_t kind, uint8_t row, uint8_t col,
                    int16_t x, int16_t y, int16_t w){
    if(c->key_count>=SKB_MAX_KEYS) return;
    skb_key_t *k=&c->keys[c->key_count++];
    memset(k,0,sizeof *k);
    k->ctx=c; k->kind=kind; k->row=row; k->col=col;
    k->btn=lv_button_create(c->kb);
    lv_obj_set_pos(k->btn,x,y);
    lv_obj_set_size(k->btn,w,SKB_ROW_H);
    lv_obj_set_ext_click_area(k->btn,1);   /* easier taps; 1px keeps zero overlap inside the 3-4px row gaps */
    lv_obj_remove_flag(k->btn,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(k->btn,skb_key_cb,LV_EVENT_CLICKED,k);
    if(kind==SKB_BKSP) lv_obj_add_event_cb(k->btn,skb_key_cb,LV_EVENT_LONG_PRESSED,k);   /* hold to clear all */
    lv_obj_set_style_radius(k->btn,7,0);
    lv_obj_set_style_border_width(k->btn,1,0);
    lv_obj_set_style_border_color(k->btn,lv_color_hex(0x3A3A3C),0);
    lv_obj_set_style_bg_color(k->btn,lv_color_hex(0x202022),0);
    lv_obj_set_style_bg_color(k->btn,lv_color_hex(0x3A3A3C),LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(k->btn,lv_color_hex(0x425a78),LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(k->btn,0,0);
    k->label=lv_label_create(k->btn);
    lv_obj_set_style_text_font(k->label,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_color(k->label,lv_color_hex(0xF0F0F0),0);
}
static lv_obj_t *skb_create(lv_obj_t *parent, lv_obj_t *ta, void (*submit)(void)){
    skb_t *c=lv_malloc(sizeof *c);
    if(!c) return NULL;
    memset(c,0,sizeof *c);
    c->ta=ta; c->submit=submit; c->mode=SKB_LOWER;
    c->kb=lv_obj_create(parent);
    lv_obj_set_size(c->kb,SKB_W,SKB_H);
    lv_obj_set_pos(c->kb,0,SKB_Y);
    lv_obj_remove_flag(c->kb,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c->kb,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(c->kb,0,0);
    lv_obj_set_style_pad_all(c->kb,0,0);
    lv_obj_add_event_cb(c->kb,skb_del_cb,LV_EVENT_DELETE,c);
    for(uint8_t row=0;row<4;row++){
        const skb_rowl_t *r=&skb_rows[row];
        int16_t x=(SKB_W - skb_row_w(r))/2;
        int16_t y=r->y - SKB_Y;
        for(uint8_t col=0;col<r->count;col++){
            int16_t w = r->widths ? r->widths[col] : r->key_w;
            skb_kind_t kind=SKB_CHAR;
            if(row==3){
                switch(col){ case 0:kind=SKB_SHIFT;break; case 1:kind=SKB_MODE;break;
                             case 2:kind=SKB_SPACE;break; case 3:kind=SKB_BKSP;break;
                             default:kind=SKB_OK; }
            }
            skb_add(c,kind,row,col,x,y,w);
            x += w + r->gap;
        }
    }
    skb_refresh(c);
    return c->kb;
}

/* ---- modal wrapper ------------------------------------------------------- */
static lv_obj_t *g_modal, *g_ta;
static kbinput_done_cb_t g_done;

int kbinput_active(void){ return g_modal != NULL; }

static void finish(const char *result){
    kbinput_done_cb_t cb = g_done; g_done = NULL;
    if(g_modal){ lv_obj_delete_async(g_modal); g_modal = NULL; g_ta = NULL; }
    if(cb) cb(result);
}
static void do_save(void){
    static char buf[160];
    snprintf(buf, sizeof buf, "%s", g_ta ? lv_textarea_get_text(g_ta) : "");
    int n = (int)strlen(buf);
    while(n > 0 && buf[n-1]==' ') buf[--n] = 0;
    finish(buf[0] ? buf : NULL);
}
static void save_btn(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) do_save(); }
static void cancel_btn(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) finish(NULL); }

static void pill(lv_obj_t *parent, int x, int y, const char *sym, lv_color_t col, lv_event_cb_t cb){
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 60, 36);
    lv_obj_align(b, LV_ALIGN_TOP_MID, x, y);
    lv_obj_set_ext_click_area(b, 8);   /* Save/Cancel sit in open space - generous hit area */
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_center(l);
}

/* set by kbinput_open_password() for the next open only, then consumed/reset. */
static int g_mask_next = 0;
void kbinput_open_password(const char *title, const char *initial, kbinput_done_cb_t cb){
    g_mask_next = 1;
    kbinput_open(title, initial, cb);
}
void kbinput_open(const char *title, const char *initial, kbinput_done_cb_t cb){
    if(g_modal) finish(NULL);
    g_done = cb;

    g_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_modal);
    lv_obj_set_size(g_modal, 360, 360);
    lv_obj_center(g_modal);
    lv_obj_set_style_bg_color(g_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_modal, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_modal, LV_OBJ_FLAG_SCROLLABLE);

    g_ta = lv_textarea_create(g_modal);
    lv_textarea_set_one_line(g_ta, true);
    lv_obj_set_scroll_dir(g_ta, LV_DIR_HOR);   /* one line: allow only the cursor-follow HORIZONTAL scroll - no vertical jitter */
    lv_obj_set_scrollbar_mode(g_ta, LV_SCROLLBAR_MODE_OFF);
    if(title && title[0]) lv_textarea_set_placeholder_text(g_ta, title);
    if(initial && initial[0]) lv_textarea_set_text(g_ta, initial);
    if(g_mask_next){ lv_textarea_set_password_mode(g_ta, true); g_mask_next = 0; }  /* masked secret entry */
    lv_obj_set_size(g_ta, 240, 40);
    lv_obj_align(g_ta, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(g_ta, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_text_color(g_ta, lv_color_hex(0xFFFFFF), 0);
    /* typed text and the placeholder ("Password for <SSID>", a playlist name on Rename) are user
     * data: route both parts through the chain so Cyrillic/CJK don't tofu (issue #3). */
    lv_obj_set_style_text_font(g_ta, ui_font_cjk(16), 0);
    lv_obj_set_style_text_font(g_ta, ui_font_cjk(16), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_width(g_ta, 0, 0);
    /* Pin the internal label to a FIXED one-line height so the textarea's vertical centering can't drift
     * as the measured text extent (ascenders/descenders/cursor) changes per keystroke -> no vertical bounce. */
    lv_obj_set_style_pad_top(g_ta, 8, 0); lv_obj_set_style_pad_bottom(g_ta, 8, 0);   /* 40 - 24 line = 16 -> 8/8 centres one full line */
    { lv_obj_t *ta_lbl = lv_textarea_get_label(g_ta);   /* pin to the FONT's 24px line height so the text can't clip and centring can't drift */
      if(ta_lbl){ lv_obj_set_style_min_height(ta_lbl, 24, 0); lv_obj_set_style_max_height(ta_lbl, 24, 0); } }

    /* Cancel / Save in the wide mid-band, above the keyboard */
    pill(g_modal, -66, 108, LV_SYMBOL_CLOSE, lv_color_hex(0xC7C7CC), cancel_btn);
    pill(g_modal,  66, 108, LV_SYMBOL_OK,    lv_color_hex(0x34C759), save_btn);

    skb_create(g_modal, g_ta, do_save);   /* the keyboard's OK key also saves */
}
