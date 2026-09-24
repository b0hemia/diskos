#!/usr/bin/env python3
"""Exercise shared transport and pause feedback, with IPC failures and storage holds."""
from pathlib import Path
import os,subprocess,tempfile
app=Path(__file__).resolve().parents[1]
s=(app/'main.c').read_text();hint=s[s.index('static int      g_pp_hint'):s.index('/* set absolute volume')]
scan=s[s.index('void ui_rescan_library(void)'):s.index('/* Poll for scan completion')]
s=(app/'ui.c').read_text();action=s[s.index('int ui_transport_command('):s.index('static void transport_cb(')]
headers=r'''
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define LV_SYMBOL_PAUSE "pause"
#define LV_SYMBOL_PLAY "play"
static int g_playing,local=1,book,send_fail,seeks,commands,cancelled,disarmed,hints;
static int scan_busy,scan_fail,sd_allowed=1,scans;
static uint32_t now;
static long g_seek_target_ms,requested_seek;
static uint32_t g_seek_hold_until;
static char *btn_pp="";
static char toast[100],glyph[20],g_play_scope[40]="keep",g_play_pendscope[40]="keep";
typedef struct { char path[40]; long position_ms,duration_ms; } track_state_t;
static track_state_t state;
static uint32_t lv_tick_get(void){return now;}
static int ui_is_playing(void){return g_playing;}
static int ui_local_playback_allowed(void){return local;}
static void ui_toast(const char *s){snprintf(toast,sizeof toast,"%s",s);}
static void ui_defer_sleep(void){}
static void ipc_get_state(track_state_t *s){*s=state;}
static int mdb_is_book_path(const char *s){(void)s;return book;}
static int ipc_send_cmd(const char *s){(void)s;commands++;return send_fail?-1:0;}
static int ui_seek_to(long p){seeks++;requested_seek=p;return send_fail?-1:0;}
static void ui_book_user_seeked(long p){(void)p;hints++;}
static void ui_cancel_book_resume(void){cancelled++;}
static void ui_disarm_book_eoc(void){disarmed++;}
static void set_label_text_changed(const char *obj,const char *s){(void)obj;snprintf(glyph,sizeof glyph,"%s",s);}
static int scanner_active(void){return scan_busy;}
static int sd_io_allowed(void){return sd_allowed;}
static int scanner_start(void){scans++;return scan_fail?-1:0;}
int ui_pp_icon_playing(int real);
'''
tests=r'''
int main(void){
    g_playing=1; now=1000;
    assert(ui_transport_command("0201000C0000")==0);
    assert(!strcmp(glyph,"play") && ui_pp_icon_playing(1)==0);
    now=1100; assert(ui_transport_command("0201000C0000")==0);
    assert(!strcmp(glyph,"pause") && ui_pp_icon_playing(0)==1);
    now=1200; assert(ui_transport_command("0201000C0000")==0);
    assert(ui_pp_icon_playing(1)==0);
    now=3000; assert(ui_pp_icon_playing(0)==0); /* no bounce at the inference grace */
    now=3401; assert(ui_pp_icon_playing(1)==1); /* reconcile after hold */
    now=UINT32_MAX-100; ui_pp_tap_hint(); now=50;
    assert(ui_pp_icon_playing(1)==0); now=2300; assert(ui_pp_icon_playing(1)==1);
    puts("PASS instant feedback, rapid repeated taps, reconciliation and tick wrap");

    send_fail=1; g_pp_hint=-1; assert(ui_transport_command("0201000C0000")<0);
    assert(g_pp_hint==-1 && !strcmp(toast,"Player busy - try again"));
    int sent=commands; local=0;
    assert(ui_transport_command("0201000C0000")<0);
    assert(ui_transport_command("0201000C0001")<0);
    assert(commands==sent && seeks==0);
    puts("PASS no optimistic success on IPC failure and no commands during USB ownership");

    local=1; send_fail=0; book=1; state.position_ms=10000;state.duration_ms=20000;
    assert(ui_transport_command("0201000C0002")==0 && requested_seek==0);
    assert(ui_transport_command("0201000C0001")==0 && requested_seek==20000);
    assert(commands==sent && hints==2 && !cancelled && !disarmed);
    send_fail=1; assert(ui_transport_command("0201000C0001")<0 && hints==2);
    book=0; assert(ui_transport_command("0201000C0001")<0 && !cancelled && !disarmed);
    send_fail=0;assert(ui_transport_command("0201000C0001")==0 && cancelled==1 && disarmed==1);
    puts("PASS bounded book seeks, failed seek preserves resume, successful music skip clears it");

    local=0; ui_rescan_library(); assert(scans==0 && !strcmp(g_play_scope,"keep"));
    local=1;scan_busy=1;ui_rescan_library();assert(scans==0 && strstr(toast,"already running"));
    scan_busy=0;sd_allowed=0;ui_rescan_library();assert(scans==0 && strstr(toast,"not ready"));
    sd_allowed=1;scan_fail=1;ui_rescan_library();assert(scans==1 && !strcmp(g_play_scope,"keep") && strstr(toast,"Couldn't start"));
    scan_fail=0;ui_rescan_library();assert(scans==2 && !g_play_scope[0] && !g_play_pendscope[0] && !strcmp(toast,"Scanning library..."));
    puts("PASS honest scan admission, busy/error feedback and scope invalidation after start");
    puts("HARNESS COMPLETE");
}
'''
def run_harness(exe, timeout):
    """Run a compiled harness; its LAST stdout line must be HARNESS COMPLETE, so an early return 0 fails."""
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    if r.returncode != 0:
        raise SystemExit(f'FAIL harness exited {r.returncode}')
    lines = r.stdout.strip().splitlines()
    if not lines or lines[-1] != 'HARNESS COMPLETE':
        raise SystemExit('FAIL harness did not reach its HARNESS COMPLETE line')


with tempfile.TemporaryDirectory(prefix='diskos-transport-test-') as t:
    p=Path(t);(p/'test.c').write_text(headers+hint+action+scan+tests)
    subprocess.run([os.environ.get('CC','cc'),'-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    run_harness(p/'test', 10)

print('COMPLETE transport_test.py')   # last line: the runner requires it, so an early clean exit cannot pass
