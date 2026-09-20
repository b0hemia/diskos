/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* toast.c - transient bottom-of-screen message for completion feedback.
 * ui_toast("Imported 3 playlists") shows a pill on lv_layer_top that auto-dismisses.
 * Non-blocking, non-clickable (doesn't eat touches). Replaces any prior toast. */
#include "screens.h"

static lv_obj_t   *g_toast;
static lv_timer_t *g_toast_timer;

static void toast_hide_cb(lv_timer_t *t)
{
    if(g_toast){ lv_obj_delete_async(g_toast); g_toast = NULL; }
    if(g_toast_timer){ lv_timer_delete(g_toast_timer); g_toast_timer = NULL; }
}

void ui_toast(const char *msg)
{
    if(!msg) return;
    if(g_toast){ lv_obj_delete_async(g_toast); g_toast = NULL; }
    if(g_toast_timer){ lv_timer_delete(g_toast_timer); g_toast_timer = NULL; }

    g_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_toast);
    lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_toast, 16, 0);
    lv_obj_set_style_pad_all(g_toast, 12, 0);
    lv_obj_set_size(g_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(g_toast, 230, 0);   /* round-screen: bottom chord is narrow, keep the pill inside it */
    lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -64);  /* raised off the clipped bottom edge into the wider chord */
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_CLICKABLE);   /* let touches pass through */
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(g_toast);
    lv_label_set_text(l, msg);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 204);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, ui_font_cjk(16), 0);   /* some toasts embed a filename/name: chain (issue #3) */
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);

    g_toast_timer = lv_timer_create(toast_hide_cb, 2200, NULL);
}
