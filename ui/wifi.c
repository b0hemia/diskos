/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "screens.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

/* Wi-Fi settings (SCR_WIFI) + a details screen (SCR_WIFI_INFO).
 * Radio on/off reuses the stock /usr/bin/wifi_up.sh / wifi_down.sh (rfkill +
 * wpa_supplicant + udhcpc).  Scan/connect drive the running supplicant via
 * `wpa_cli -i wlan0`.  Connecting writes the network in and `save_config`s it
 * to /usr/data/wpa_supplicant.conf (update_config=1), so it persists.
 * Round-screen-aware: the top corners stay clear (only back chevron + centred
 * title); the on/off switch + scan button share one row in the wide centre
 * band.  The list itself carries state (connected ✓ row / "Wi-Fi is off" /
 * "Scanning" / "No networks") so there is no redundant status line.
 * NB: connecting to a DIFFERENT AP drops the laptop deploy link - recover over
 * serial if needed. */

#define WCLI "/usr/sbin/wpa_cli -i wlan0 "

static lv_obj_t *g_sw, *g_list;
static lv_timer_t *g_scan_timer;
static lv_timer_t *g_conn_timer;      /* non-NULL while a user connect is being polled */
static uint32_t    g_wifi_last_start = 0;   /* last wifi_up.sh launch (shared: toggle + keepalive) */
static int         g_sanitize_pending = 1;  /* sanitize saved nets once the supplicant is up (set on 1st run + each (re)launch) */
static char g_pending_ssid[64];   /* SSID awaiting a password from the keyboard */
static char g_cur_ssid[64];       /* currently-connected SSID (to mark the list) */
static lv_obj_t *g_info_list;     /* details screen */

static void start_scan(void);     /* fwd */

/* The refresh glyph INSIDE the "Scanning" message spins while a scan runs. It lives
 * in g_list, so it must be stopped before g_list is cleaned (else the anim references
 * a freed object). scan_stop() is called at the top of every g_list-clearing path. */
static lv_obj_t *g_scan_icon;
static void spin_anim_cb(void *o, int32_t v){ lv_obj_set_style_transform_rotation((lv_obj_t*)o, v, 0); }
static void scan_stop(void){
    if(g_scan_icon){ lv_anim_delete(g_scan_icon, spin_anim_cb); g_scan_icon = NULL; }
}

/* ---- tiny local copies of settings.c's header helpers ------------------- */

/* ---- shell helpers ------------------------------------------------------ */
static int run_cap(const char *cmd, char *out, int cap){
    out[0] = 0;
    FILE *p = popen(cmd, "r");
    if(!p){ fprintf(stderr,"wifi run_cap popen failed: %s (%s)\n", cmd, strerror(errno)); return 0; }
    int n = fread(out, 1, cap-1, p);
    if(n < 0) n = 0;
    out[n] = 0;
    pclose(p);
    return n;
}

static int wifi_status(char *ssid, int scap, char *ip, int icap){
    (void)scap; (void)icap;
    ssid[0] = 0; ip[0] = 0;
    char buf[2048];
    if(!run_cap(WCLI "status 2>/dev/null", buf, sizeof buf)) return 0;
    int connected = (strstr(buf, "wpa_state=COMPLETED") != NULL);
    char *l = buf;
    while(l && *l){
        if(!strncmp(l, "ssid=", 5)){ sscanf(l+5, "%63[^\n]", ssid); }
        else if(!strncmp(l, "ip_address=", 11)){ sscanf(l+11, "%31[^\n]", ip); }
        l = strchr(l, '\n'); if(l) l++;
    }
    return connected;
}

static int wifi_radio_on(void){
    char buf[256];
    run_cap("pidof wpa_supplicant 2>/dev/null", buf, sizeof buf);
    return buf[0] ? 1 : 0;
}

/* ---- persistence / auto-reconnect hardening ----------------------------- */
/* Make the running supplicant's config writable, keep EVERY saved network a candidate
 * for auto-join (not just the last-selected), and drop stale per-network channel pins
 * (a network locked to a frequency the AP has since left never associates). Run on each
 * supplicant bring-up and after a user connect. */
/* wpa_cli only responds once the control interface is up - pidof can be true well before
 * that, so a ping (PONG) is the real "ready" signal. */
static int wpa_ready(void){
    char buf[64];
    run_cap(WCLI "ping 2>/dev/null", buf, sizeof buf);
    return strstr(buf, "PONG") != NULL;
}
/* returns 1 if it actually ran (ctrl interface ready), 0 if skipped so the caller keeps
 * g_sanitize_pending set until a live, responsive supplicant can be sanitized. */
static int wifi_sanitize_savenets(void){
    if(!wpa_ready()) return 0;                            /* supplicant not (yet) accepting wpa_cli */
    system(WCLI "set update_config 1 >/dev/null 2>&1");   /* else save_config silently fails */
    system(WCLI "enable_network all >/dev/null 2>&1");    /* any known net can auto-join */
    char buf[4096];
    run_cap(WCLI "list_networks 2>/dev/null", buf, sizeof buf);
    char *l = strchr(buf, '\n'); if(l) l++;               /* skip header row */
    while(l && *l){
        int nid;
        if(sscanf(l, "%d", &nid) == 1){
            char c[96];
            snprintf(c, sizeof c, WCLI "set_network %d frequency 0 >/dev/null 2>&1", nid);
            system(c);                                     /* clear stale channel lock */
        }
        l = strchr(l, '\n'); if(l) l++;
    }
    system(WCLI "save_config >/dev/null 2>&1");
    return 1;
}

/* Seed diskOS's own Wi-Fi on/off intent from stock SYSCONFIG.WIFI_STATUS the FIRST time
 * only; thereafter diskOS owns "wifi_on" (the toggle persists every change), so the
 * keepalive can distinguish "supplicant died" from "user turned Wi-Fi off". */
void wifi_init_intent(void){
    if(cfg_get_int("wifi_on", -1) >= 0) return;           /* already owned */
    char buf[32];
    run_cap("sqlite3 /usr/data/fiio/db/sysconfig.db \"SELECT WIFI_STATUS FROM SYSCONFIG WHERE ID=1\" 2>/dev/null",
            buf, sizeof buf);
    /* only persist a VALID read (0/1); a failed/empty query leaves it unseeded so it
     * retries next boot AND the keepalive's default (on) applies meanwhile - never
     * silently latch Wi-Fi off because sqlite hiccuped. */
    if(buf[0]=='0' || buf[0]=='1') cfg_set_int("wifi_on", buf[0]-'0');
}

/* Keepalive: whenever Wi-Fi should be on but wpa_supplicant isn't running (died, never
 * started, radio glitch) (re)start it, so a known network returning in range auto-joins.
 * Sanitize saved nets once the supplicant is up after each (re)launch - driven by a pending
 * flag (set on first run + every launch we initiate, cleared after one run), so it's
 * deterministic and idempotent regardless of pid reuse or a transient pidof miss. Enforce a
 * deliberate OFF against a late in-flight wifi_up. Self-rate-limited: safe to call each loop. */
void wifi_supervise(void){
    static uint32_t last_check = 0;
    static int      first = 1;
    if(!first && lv_tick_elaps(last_check) < 5000) return;   /* check ~every 5s */
    first = 0; last_check = lv_tick_get();

    int up   = wifi_radio_on();
    int want = cfg_get_int("wifi_on", 1);

    /* re-enable all nets + clear freq locks + persist once the supplicant is up, unless a
     * user connect is mid-flight (conn_poll_cb sanitizes on its outcome). */
    if(up && g_sanitize_pending && !g_conn_timer && wifi_sanitize_savenets()) g_sanitize_pending = 0;

    if(!up && want){                        /* should be on but isn't -> (re)start */
        if(!g_wifi_last_start || lv_tick_elaps(g_wifi_last_start) > 30000){  /* 30s backoff */
            g_wifi_last_start = lv_tick_get();
            g_sanitize_pending = 1;         /* sanitize once this new instance is up */
            system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &");
        }
    } else if(up && !want){                 /* should be off but a late wifi_up won -> enforce off */
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
    }
}

/* centred grey placeholder shown in the list area for non-network states */
/* a single status message (Scanning / off / empty) - centered in the list area */
static void list_msg(const char *m){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *e = lv_label_create(g_list);
    lv_label_set_text(e, m);
    lv_obj_set_style_text_color(e, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(e, &lv_font_montserrat_14, 0);
}
/* "Scanning" + a spinning refresh glyph, centered in the list area */
static void list_msg_scanning(void){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *row = lv_obj_create(g_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, "Scanning");
    lv_obj_set_style_text_color(t, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, LV_SYMBOL_REFRESH);
    lv_obj_set_size(ic, 24, 24);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
    g_scan_icon = ic;
    lv_obj_set_style_transform_pivot_x(ic, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(ic, lv_pct(50), 0);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, ic);
    lv_anim_set_exec_cb(&a, spin_anim_cb);
    lv_anim_set_values(&a, 0, 3600); lv_anim_set_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* default gateway from /proc/net/route (no external tools). */
static int get_gateway(char *out, int cap){
    out[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if(!f) return 0;
    char line[256]; int got = 0;
    if(!fgets(line, sizeof line, f)){ fclose(f); return 0; }   /* header */
    while(fgets(line, sizeof line, f)){
        char iface[32]; unsigned long dest, gw;
        if(sscanf(line, "%31s %lx %lx", iface, &dest, &gw) == 3 && dest == 0 && gw != 0){
            snprintf(out, cap, "%lu.%lu.%lu.%lu",
                     gw & 0xFF, (gw>>8)&0xFF, (gw>>16)&0xFF, (gw>>24)&0xFF);
            got = 1; break;
        }
    }
    fclose(f);
    return got;
}

/* Build a shell-safe, wpa_cli-safe quoted token for a network value.
 * wpa_cli wants a quoted string  "<value>"  (with \ and " backslash-escaped);
 * the whole token is then wrapped in shell single-quotes (with any literal '
 * emitted as '\'') so an SSID/password containing ' " \ $ space etc. can never
 * break out of the command. Output e.g.:  '"my\"net"'  or  '"pa'\''ss"'        */
static void wpa_q(const char *v, char *out, int cap){
    int o = 0;
    #define WQ_PUT(ch) do{ if(o < cap-1) out[o++] = (char)(ch); }while(0)
    WQ_PUT('\'');                 /* open shell single-quote   */
    WQ_PUT('"');                  /* open wpa quoted-string    */
    for(; *v && o < cap-8; v++){
        char c = *v;
        if(c == '\\' || c == '"'){ WQ_PUT('\\'); WQ_PUT(c); }   /* wpa-escape */
        else if(c == '\''){ WQ_PUT('\''); WQ_PUT('\\'); WQ_PUT('\''); WQ_PUT('\''); } /* '\'' */
        else WQ_PUT(c);
    }
    WQ_PUT('"');                  /* close wpa quoted-string   */
    WQ_PUT('\'');                 /* close shell single-quote  */
    out[o] = 0;
    #undef WQ_PUT
}

/* ---- connect completion poll (honest feedback) -------------------------
 * A connect is async (association + DHCP take seconds). Instead of re-listing
 * immediately and implying success, poll wpa_state + IP for up to 15s and toast
 * the real outcome. */
static uint32_t    g_conn_start;
static char        g_conn_ssid[64];
static void conn_poll_cb(lv_timer_t *t){
    (void)t;
    char ss[64], ip[32];
    /* require COMPLETED + an IP + the associated SSID == the one we asked for, so a
     * stale old-AP "COMPLETED" (with the previous IP) during an AP switch can't be
     * mistaken for success. */
    if(wifi_status(ss, sizeof ss, ip, sizeof ip) && ip[0] && !strcmp(ss, g_conn_ssid)){
        lv_timer_del(g_conn_timer); g_conn_timer = NULL;
        snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", g_conn_ssid);
        if(wifi_sanitize_savenets()) g_sanitize_pending = 0;   /* re-enable all + persist (supplicant is up here) */
        ui_toast("Connected");
        start_scan();                        /* refresh list so the joined net gets its ✓ */
        return;
    }
    if(lv_tick_elaps(g_conn_start) > 15000){  /* gave it 15s to associate + DHCP */
        lv_timer_del(g_conn_timer); g_conn_timer = NULL;
        /* connect failed: re-enable other nets IF the supplicant is still up; if it died,
         * leave g_sanitize_pending set so supervise sanitizes the fresh instance when it's up. */
        if(wifi_sanitize_savenets()) g_sanitize_pending = 0;
        ui_toast("Couldn't connect");
        start_scan();
    }
}

/* ---- connect ------------------------------------------------------------ */
static void wifi_connect(const char *ssid, const char *key){
    char cmd[512], buf[2048];
    int id = -1;
    run_cap(WCLI "list_networks 2>/dev/null", buf, sizeof buf);
    char *l = strchr(buf, '\n'); if(l) l++;               /* skip the header row */
    size_t slen = strlen(ssid);
    while(l && *l){
        /* exact SSID match by literal-tab field split. sscanf("%d\t%63[^\t]") swallowed leading
         * whitespace in the SSID, so " Home" and "Home" compared equal - selecting one could
         * reconfigure the other saved profile. Split on the real tabs instead. */
        char *nl = strchr(l, '\n');
        char *t1 = strchr(l, '\t');                       /* end of id field */
        if(t1 && (!nl || t1 < nl)){
            char *sfld = t1 + 1;                          /* ssid field */
            char *t2 = strchr(sfld, '\t');                /* the SSID is always followed by a tab (bssid) */
            /* require a COMPLETE ssid field: a truncated capture ("17\tHom") must NOT match a shorter
             * prefix of a longer real SSID (e.g. select "Home" and reconfigure saved "HomeOffice"). */
            if(t2 && (!nl || t2 < nl) && (size_t)(t2 - sfld) == slen && memcmp(sfld, ssid, slen) == 0){
                id = atoi(l); break;
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    if(id < 0){
        char idbuf[32];
        run_cap(WCLI "add_network 2>/dev/null", idbuf, sizeof idbuf);
        char *end = idbuf;
        long nid = strtol(idbuf, &end, 10);
        if(end == idbuf || nid < 0){   /* empty / "FAIL\n" -> id would be 0 and clobber network 0 */
            fprintf(stderr, "wifi add_network failed: '%s'\n", idbuf);
            ui_toast("Couldn't add network");
            return;
        }
        id = (int)nid;
    }
    char qssid[160], qkey[300];
    wpa_q(ssid, qssid, sizeof qssid);
    /* run a wpa_cli step, log + yield its rc (gnu11 statement-expr) */
    #define WSTEP(c,step) ({ int _rc=system(c); if(_rc!=0) fprintf(stderr,"wifi connect: %s failed (rc=%d)\n",(step),_rc); _rc; })
    /* a failed set_network leaves the network misconfigured - abort BEFORE enable/select/
     * save/DHCP rather than connecting to a half-built profile. */
    snprintf(cmd, sizeof cmd, WCLI "set_network %d ssid %s >/dev/null 2>&1", id, qssid);
    if(WSTEP(cmd,"set ssid")){ ui_toast("Couldn't configure Wi-Fi"); return; }
    if(key && key[0]){
        wpa_q(key, qkey, sizeof qkey);
        /* set key_mgmt WPA-PSK BEFORE psk: a REUSED profile that was previously open still has
         * key_mgmt NONE, and setting only psk would leave it open-auth (connect fails w/ right pass). */
        snprintf(cmd, sizeof cmd, WCLI "set_network %d key_mgmt WPA-PSK >/dev/null 2>&1", id);
        if(WSTEP(cmd,"set key_mgmt")){ ui_toast("Couldn't configure Wi-Fi"); return; }
        snprintf(cmd, sizeof cmd, WCLI "set_network %d psk %s >/dev/null 2>&1", id, qkey);
        if(WSTEP(cmd,"set psk")){ ui_toast("Couldn't set Wi-Fi password"); return; }
    } else {
        snprintf(cmd, sizeof cmd, WCLI "set_network %d key_mgmt NONE >/dev/null 2>&1", id);
        if(WSTEP(cmd,"set open")){ ui_toast("Couldn't configure Wi-Fi"); return; }
    }
    snprintf(cmd, sizeof cmd, WCLI "enable_network %d >/dev/null 2>&1", id);  WSTEP(cmd,"enable_network");
    /* select_network is the real activation - if it fails, don't run DHCP against the
     * old/current association (which would misreport success via conn_poll). */
    snprintf(cmd, sizeof cmd, WCLI "select_network %d >/dev/null 2>&1", id);
    if(WSTEP(cmd,"select_network")){ ui_toast("Couldn't connect to Wi-Fi"); return; }
    WSTEP(WCLI "save_config >/dev/null 2>&1","save_config");   /* non-fatal: connection still works unsaved */
    system("/sbin/udhcpc -i wlan0 -n -q >/dev/null 2>&1 &");   /* backgrounded; outcome via conn_poll_cb */
    #undef WSTEP
    /* don't claim success yet - poll for the real outcome (assoc + DHCP). */
    snprintf(g_conn_ssid, sizeof g_conn_ssid, "%s", ssid);
    ui_toast("Connecting...");
    g_conn_start = lv_tick_get();
    if(g_conn_timer) lv_timer_del(g_conn_timer);
    g_conn_timer = lv_timer_create(conn_poll_cb, 1000, NULL);
}

static void psk_done(const char *text){
    if(text && text[0]) wifi_connect(g_pending_ssid, text);
}

/* ---- details screen (SCR_WIFI_INFO) ------------------------------------- */
static void info_row(const char *key, const char *val){
    lv_obj_t *r = lv_obj_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 40);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_50, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *k = lv_label_create(r);
    lv_label_set_text(k, key);
    lv_obj_set_pos(k, 12, 11);
    lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(k, lv_color_hex(0x8E8E93), 0);
    lv_obj_t *v = lv_label_create(r);
    lv_label_set_text(v, val && val[0] ? val : "-");
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(v, 110, 11); lv_obj_set_size(v, 128, 18);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(v, ui_font_cjk(14), 0);   /* "Network" value = SSID: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(v, lv_color_hex(0xFFFFFF), 0);
}
static void info_back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void wifi_info_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Network", info_back_cb);   /* shared header */
    g_info_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_info_list);
    lv_obj_set_pos(g_info_list, 55, 84); lv_obj_set_size(g_info_list, 250, 230);
    lv_obj_set_style_pad_bottom(g_info_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_info_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_info_list, 8, 0);
    lv_obj_set_flex_flow(g_info_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_info_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_info_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_info_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* A tappable destructive-action row appended to the details list (Forget). */
static void info_action_row(const char *label, lv_event_cb_t cb){
    lv_obj_t *r = lv_button_create(g_info_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 250, 44);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x2A1416), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x3A1C1E), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, label); lv_obj_center(t);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xFF453A), 0);
}
static char g_info_ssid[64];   /* the exact SSID shown on the details screen (retained for Forget) */
/* Remove EVERY saved network whose SSID EXACTLY equals `ssid`, then persist. 1 if any removed.
 * list_networks is TAB-separated (id\tssid\tbssid\tflags); we split on literal tabs and compare the
 * exact ssid field - NOT sscanf "%[^\t]" after a "\t", which would swallow a leading space so " Home"
 * and "Home" collide and Forget could delete the wrong network. */
static int wifi_remove_ssid(const char *ssid){
    if(!wpa_ready() || !ssid || !ssid[0]) return 0;
    char buf[4096]; run_cap(WCLI "list_networks 2>/dev/null", buf, sizeof buf);
    char *l = strchr(buf, '\n'); if(l) l++;               /* skip the header row */
    size_t slen = strlen(ssid);
    int removed = 0, failed = 0;
    while(l && *l){
        char *nl = strchr(l, '\n');
        char *t1 = strchr(l, '\t');                       /* end of id field */
        if(t1 && (!nl || t1 < nl)){
            char *sfld = t1 + 1;                           /* ssid field start */
            char *t2 = strchr(sfld, '\t');                 /* end of ssid field (always a tab: bssid follows) */
            /* require a COMPLETE ssid field so a truncated capture can't match a shorter prefix of a longer real SSID. */
            if(t2 && (!nl || t2 < nl) && (size_t)(t2 - sfld) == slen && memcmp(sfld, ssid, slen) == 0){
                int nid = atoi(l);
                /* wpa_cli prints OK/FAIL and exits 0 either way, so check its output, not the
                 * shell rc - otherwise a failed remove would still report "forgotten". */
                char cmd[80]; snprintf(cmd, sizeof cmd, WCLI "remove_network %d 2>/dev/null", nid);
                char out[64]; run_cap(cmd, out, sizeof out);
                if(strstr(out, "OK")) removed = 1; else failed = 1;   /* a duplicate-SSID profile we could not remove -> report failure */
            }
        }
        l = nl ? nl + 1 : NULL;
    }
    if(removed){
        char so[64]; run_cap(WCLI "save_config 2>/dev/null", so, sizeof so);
        if(!strstr(so, "OK")) failed = 1;   /* removed from the running config but not persisted (it returns on reboot) */
    }
    return removed && !failed;   /* success only when something was removed AND nothing failed */
}
static void wifi_forget_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    ui_toast(wifi_remove_ssid(g_info_ssid) ? "Network forgotten" : "Couldn't forget network");
    wifi_open();   /* back to the list + fresh scan */
}
void wifi_info_open(void){
    if(!g_info_list) return;
    lv_obj_clean(g_info_list);
    char ssid[64], ip[32], buf[2048], val[64], gw[32];
    wifi_status(ssid, sizeof ssid, ip, sizeof ip);
    snprintf(g_info_ssid, sizeof g_info_ssid, "%s", ssid);   /* retain for Forget (don't re-read on click) */
    info_row("Network", ssid);
    info_row("IP Address", ip);
    if(get_gateway(gw, sizeof gw)) info_row("Router", gw);
    run_cap(WCLI "signal_poll 2>/dev/null", buf, sizeof buf);
    char *p = strstr(buf, "RSSI=");
    if(p){ int r; if(sscanf(p+5, "%d", &r)==1){ snprintf(val,sizeof val,"%d dBm", r); info_row("Signal", val);} }
    run_cap(WCLI "status 2>/dev/null", buf, sizeof buf);
    p = strstr(buf, "key_mgmt=");
    if(p && sscanf(p+9, "%63[^\n]", val)==1){ info_row("Security", strstr(val,"NONE")?"Open":val); }   /* check sscanf: val was read uninitialized on no-match */
    p = strstr(buf, "\nfreq=");
    if(p){ int fr; if(sscanf(p+6,"%d",&fr)==1){ snprintf(val,sizeof val,"%d MHz", fr); info_row("Frequency", val);} }
    p = strstr(buf, "bssid=");
    if(p && sscanf(p+6, "%63[^\n]", val)==1){ info_row("BSSID", val); }   /* check sscanf: val was read uninitialized on no-match */
    info_action_row("Forget This Network", wifi_forget_cb);   /* C05: remove the saved network */
    screen_show(SCR_WIFI_INFO);
}

/* ---- scan list ---------------------------------------------------------- */
static void row_free_cb(lv_event_t *e){ free(lv_obj_get_user_data(lv_event_get_target(e))); }
static void net_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_CLICKED) return;
    lv_obj_t *row = lv_event_get_target(e);
    const char *ssid = (const char*)lv_obj_get_user_data(row);
    intptr_t flags = (intptr_t)lv_event_get_user_data(e);
    int secured   = flags & 1;
    int connected = flags & 2;
    if(!ssid || !ssid[0]) return;
    if(connected){
        wifi_info_open();                 /* tap the joined network -> details */
    } else if(secured){
        snprintf(g_pending_ssid, sizeof g_pending_ssid, "%s", ssid);
        char prompt[80]; snprintf(prompt, sizeof prompt, "Password for %s", ssid);
        kbinput_open_password(prompt, "", psk_done);   /* masked entry; prompt, not the bare SSID */
    } else {
        wifi_connect(ssid, NULL);
    }
}

/* signal-strength bars: RSSI(dBm) -> 1..4 filled bars, bottom-aligned. */
static void add_signal_bars(lv_obj_t *parent, int x, int y, int sig){
    int bars = sig >= -55 ? 4 : sig >= -67 ? 3 : sig >= -78 ? 2 : 1;
    static const int h[4] = {5, 9, 13, 17};
    for(int i=0;i<4;i++){
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 4, h[i]);
        lv_obj_set_pos(b, x + i*6, y + (17 - h[i]));
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i < bars ? 0xFFFFFF : 0x3A3A3C), 0);
    }
}

static void add_net_row(const char *ssid, int signal, int secured, int connected){
    lv_obj_t *r = lv_button_create(g_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 280, 46);
    lv_obj_set_style_radius(r, 8, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(connected ? 0x0A2A4A : 0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(r, connected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    char *dup = strdup(ssid);
    lv_obj_set_user_data(r, dup);
    intptr_t flags = (secured?1:0) | (connected?2:0);
    lv_obj_add_event_cb(r, net_cb, LV_EVENT_CLICKED, (void*)flags);
    lv_obj_add_event_cb(r, row_free_cb, LV_EVENT_DELETE, NULL);

    int tx = 12;
    if(connected){
        lv_obj_t *ck = lv_label_create(r);
        lv_label_set_text(ck, LV_SYMBOL_OK);
        lv_obj_set_pos(ck, 12, 15);
        lv_obj_set_style_text_font(ck, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ck, lv_color_hex(0x0A84FF), 0);
        tx = 34;
    }
    lv_obj_t *t = lv_label_create(r);
    lv_label_set_text(t, ssid);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, tx, 13); lv_obj_set_size(t, 202 - tx, 20);
    lv_obj_set_style_text_font(t, ui_font_cjk(16), 0);   /* SSIDs are user data: Cyrillic/CJK-capable (issue #3) */
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);

    if(secured){
        /* drawn padlock (no lock glyph exists in the fonts): a shackle loop with the body covering
         * its lower half, muted grey, in the clear lane left of the signal bars. */
        lv_obj_t *shk = lv_obj_create(r);            /* shackle: border-only circle, lower half hidden by body */
        lv_obj_remove_style_all(shk);
        lv_obj_clear_flag(shk, LV_OBJ_FLAG_CLICKABLE);   /* decorative: don't steal taps from the row */
        lv_obj_set_size(shk, 8, 8); lv_obj_set_pos(shk, 229, 15);
        lv_obj_set_style_radius(shk, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(shk, 2, 0);
        lv_obj_set_style_border_color(shk, lv_color_hex(0x8E8E93), 0);
        lv_obj_t *body = lv_obj_create(r);           /* body: rounded rect */
        lv_obj_remove_style_all(body);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(body, 10, 8); lv_obj_set_pos(body, 228, 20);
        lv_obj_set_style_radius(body, 2, 0);
        lv_obj_set_style_bg_color(body, lv_color_hex(0x8E8E93), 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    }
    add_signal_bars(r, 244, 14, signal);
}

/* parse `wpa_cli scan_results`: bssid \t freq \t signal \t flags \t ssid */
static void scan_fill(void){
    if(!g_list) return;
    scan_stop();
    lv_obj_clean(g_list);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);  /* rows top-aligned */
    char cs[64], cip[32];
    if(!(wifi_radio_on() && wifi_status(cs, sizeof cs, cip, sizeof cip))) cs[0]=0;
    snprintf(g_cur_ssid, sizeof g_cur_ssid, "%s", cs);

    char buf[8192];
    run_cap(WCLI "scan_results 2>/dev/null", buf, sizeof buf);
    char seen[40][64]; int nseen = 0;
    char *l = strchr(buf, '\n'); if(l) l++;   /* skip header */
    /* each line: bssid \t freq \t signal \t flags \t ssid  (ssid may be empty
     * for hidden networks, and may contain spaces - so split on tabs, never
     * scan the ssid with a width specifier). */
    while(l && *l){
        char *nl = strchr(l, '\n'); if(nl) *nl = 0;
        char *f[5] = {0,0,0,0,0}; int nf = 0; char *p = l;
        f[nf++] = p;
        while(nf < 5 && (p = strchr(p, '\t'))){ *p++ = 0; f[nf++] = p; }
        if(nf == 5 && f[4][0]){
            const char *ssid = f[4], *flags = f[3];
            int sig = atoi(f[2]);
            int dup = 0;
            for(int i=0;i<nseen;i++) if(!strcmp(seen[i], ssid)){ dup = 1; break; }
            if(!dup && nseen < 40){
                snprintf(seen[nseen++], 64, "%s", ssid);
                int secured = (strstr(flags, "WPA") || strstr(flags, "PSK") || strstr(flags, "WEP")) != 0;
                int connected = (g_cur_ssid[0] && !strcmp(ssid, g_cur_ssid));
                add_net_row(ssid, sig, secured, connected);
            }
        }
        if(!nl) break;
        l = nl + 1;
    }
    if(nseen == 0) list_msg("No networks found");
}

static void scan_timer_cb(lv_timer_t *t){
    (void)t;
    scan_fill();   /* scan_fill() calls scan_stop() before clearing the list */
    if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }
}
static void start_scan(void){
    if(!wifi_radio_on()){ list_msg("Wi-Fi is off"); return; }
    /* wpa_cli returns non-zero when it can't reach the control socket / issue the scan; surface
     * that honestly instead of letting scan_fill() report a false "No networks found". */
    if(system(WCLI "scan >/dev/null 2>&1") != 0){ list_msg("Couldn't scan Wi-Fi"); return; }
    list_msg_scanning();
    if(g_scan_timer) lv_timer_del(g_scan_timer);
    g_scan_timer = lv_timer_create(scan_timer_cb, 2500, NULL);
    lv_timer_set_repeat_count(g_scan_timer, 1);
}

/* ---- radio toggle ------------------------------------------------------- */
/* wifi_up.sh runs async, so wpa_supplicant isn't up the instant the switch flips.
 * Show "Turning on Wi-Fi..." and poll until the radio is actually up before
 * scanning (otherwise start_scan() would immediately report "Wi-Fi is off"). */
static lv_timer_t *g_radio_timer;
static uint32_t    g_radio_start;
static void radio_on_poll_cb(lv_timer_t *t){
    (void)t;
    if(wifi_radio_on()){
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        start_scan();
        return;
    }
    if(lv_tick_elaps(g_radio_start) > 8000){
        lv_timer_del(g_radio_timer); g_radio_timer = NULL;
        list_msg("Couldn't turn on Wi-Fi");
    }
}
static void sw_cb(lv_event_t *e){
    if(lv_event_get_code(e)!=LV_EVENT_VALUE_CHANGED) return;
    int on = lv_obj_has_state(g_sw, LV_STATE_CHECKED);
    if(on){
        cfg_set_int("wifi_on", 1);               /* persist intent -> keepalive keeps it up */
        system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &");
        g_wifi_last_start = lv_tick_get();       /* let the keepalive back off (no duplicate launch) */
        g_sanitize_pending = 1;                  /* sanitize this instance once it's up */
        list_msg("Turning on Wi-Fi...");
        g_radio_start = lv_tick_get();
        if(g_radio_timer) lv_timer_del(g_radio_timer);
        g_radio_timer = lv_timer_create(radio_on_poll_cb, 700, NULL);
    } else {
        cfg_set_int("wifi_on", 0);               /* set OFF before down so keepalive won't resurrect */
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }  /* cancel a pending scan, else scan_fill overwrites "Wi-Fi is off" with stale networks */
        if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }  /* cancel a pending connect poll, else it toasts/scans stale after off */
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
        list_msg("Wi-Fi is off");
    }
}

/* Quick Settings tile short-press: flip the radio + persist intent, no screen-specific UI.
 * Mirrors sw_cb's radio actions (keepalive enforces the intent). Returns the new state. */
int wifi_toggle(void){
    int on = !cfg_get_int("wifi_on", 1);
    cfg_set_int("wifi_on", on);
    if(on){
        system("/usr/bin/wifi_up.sh >/dev/null 2>&1 &");
        g_wifi_last_start = lv_tick_get();   /* let the keepalive back off (no duplicate launch) */
        g_sanitize_pending = 1;              /* sanitize this instance once it's up */
    } else {
        if(g_radio_timer){ lv_timer_del(g_radio_timer); g_radio_timer = NULL; }
        if(g_scan_timer){ lv_timer_del(g_scan_timer); g_scan_timer = NULL; }  /* cancel any pending radio/scan work on the open Wi-Fi screen */
        if(g_conn_timer){ lv_timer_del(g_conn_timer); g_conn_timer = NULL; }  /* + the connect poll */
        system("/usr/bin/wifi_down.sh >/dev/null 2>&1");
    }
    /* keep the Wi-Fi screen's switch in sync so it reflects reality when opened later */
    if(g_sw){ if(on) lv_obj_add_state(g_sw, LV_STATE_CHECKED); else lv_obj_clear_state(g_sw, LV_STATE_CHECKED); }
    return on;
}

static void rescan_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) start_scan(); }
static void back_cb(lv_event_t *e){ if(lv_event_get_code(e)==LV_EVENT_CLICKED) screen_back(); }

void wifi_create(lv_obj_t *root){
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header_cb(root, "Wi-Fi", back_cb);   /* shared header */

    /* control row: pill with [label + switch]; scan button OUTSIDE, to the
     * right of the switch (still inside the round screen). */
    lv_obj_t *trow = lv_obj_create(root);
    lv_obj_remove_style_all(trow);
    lv_obj_set_pos(trow, 50, 64); lv_obj_set_size(trow, 208, 48);
    lv_obj_set_style_radius(trow, 12, 0);
    lv_obj_set_style_bg_color(trow, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(trow, LV_OPA_70, 0);
    lv_obj_clear_flag(trow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(trow);
    lv_label_set_text(tl, "Wi-Fi");
    lv_obj_set_pos(tl, 16, 14);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(tl, lv_color_hex(0xFFFFFF), 0);

    g_sw = lv_switch_create(trow);
    lv_obj_set_size(g_sw, 46, 24);
    lv_obj_set_ext_click_area(g_sw, 10);
    lv_obj_align(g_sw, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_event_cb(g_sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rb = lv_button_create(root);
    lv_obj_remove_style_all(rb);
    lv_obj_set_pos(rb, 266, 72); lv_obj_set_size(rb, 36, 32);
    lv_obj_set_ext_click_area(rb, 8);
    lv_obj_set_style_radius(rb, 10, 0);
    lv_obj_set_style_bg_color(rb, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(rb, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(rb, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_add_event_cb(rb, rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rb);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(rl, lv_color_hex(0xC7C7CC), 0);
    lv_obj_center(rl);

    /* network list (carries connected/off/scanning state itself) */
    g_list = lv_obj_create(root);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_pos(g_list, 40, 126); lv_obj_set_size(g_list, 280, 192);
    lv_obj_set_style_pad_bottom(g_list, 44, 0);   /* last row scrolls clear of the round bottom bezel */
    lv_obj_set_style_bg_opa(g_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(g_list, 6, 0);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

/* called from settings when the Wi-Fi row is tapped */
void wifi_open(void){
    if(g_sw){
        if(wifi_radio_on()) lv_obj_add_state(g_sw, LV_STATE_CHECKED);
        else                lv_obj_clear_state(g_sw, LV_STATE_CHECKED);
    }
    screen_show(SCR_WIFI);
    start_scan();
}
