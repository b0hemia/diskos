#!/usr/bin/env python3
"""Compile the real launch gate and verify EVERY failure still reaches the stock player exec.

Owner constraint: diskOS may disable its own features, never the user's player or their route
back to the stock firmware. An earlier version held forever here; that is the bug this guards."""
from pathlib import Path
import os
import subprocess
import tempfile

app = Path(__file__).resolve().parents[1]
s = (app / 'main.c').read_text()
a = s.index('/* ---- launch outcome, published by the player launcher')
b = s.index('/* Start the SD-backed features', a)
source = s[a:b]
harness = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
static volatile sig_atomic_t g_bounded_pgid = 0;   /* real one lives with run_bounded in main.c */
static volatile sig_atomic_t g_bounded_pid = 0;    /* ditto: the direct child, killed with or without a group */
/* Observe the REAL handoff instead of replacing the program. */
static int fake_execv(const char *path, char *const argv[]);
#define execv(p,a) fake_execv((p),(a))
static int hang_guard=0;
static int fake_execv(const char *path, char *const argv[]){
    assert(!strcmp(path, "/usr/bin/mq_player"));
    assert(argv[0] && !strcmp(argv[0], "mq_player") && !argv[1]);
    /* a handed-off program must not inherit our timer, our blocked mask, or an ignored disposition */
    sigset_t cur; sigprocmask(SIG_BLOCK, NULL, &cur);
    assert(!sigismember(&cur, SIGALRM));
    struct sigaction sa; sigaction(SIGALRM, NULL, &sa);
    assert(sa.sa_handler == SIG_DFL);
    assert(alarm(0) == 0);
    puts("PLAYER_EXEC_REACHED"); fflush(stdout);
    _exit(0);
}
static int guard=1, exported=0, late_export=0, reset_rc=0, probes=0, resets=0;
static int fresh_without_card=0, transient=0;
static int player_config_absent_without_card(void){ return fresh_without_card; }
static int rmguard_ensure(void){ if(hang_guard) for(;;) pause(); return guard; }
static int sd_exported_to_host(void){ probes++; return exported || (late_export && probes>1); }
static int run_bounded(char *const argv[], int timeout){
    resets++;
    assert(timeout == 8000);
    assert(!strcmp(argv[0], "sh") && !strcmp(argv[1], "-c"));
    assert(strstr(argv[2], "UPDATE SYSCONFIG SET WORK_MODE=0"));
    assert(strstr(argv[2], "SELECT WORK_MODE FROM SYSCONFIG WHERE ID=1"));
    if(transient && resets==1) return -1;
    return reset_rc;
}
''' + source + r'''
int main(int argc, char **argv){
    if(argc > 1){
        if(!strcmp(argv[1], "guard")) guard=0;
        else if(!strcmp(argv[1], "host")) exported=1;
        else if(!strcmp(argv[1], "reset")) reset_rc=-1;
        else if(!strcmp(argv[1], "late")) late_export=1;
        else if(!strcmp(argv[1], "transient")) transient=1;
        else if(!strcmp(argv[1], "fresh")) fresh_without_card=1;
        else if(!strcmp(argv[1], "hang")) hang_guard=1;
        else if(!strcmp(argv[1], "blockedlog")) guard=0;   /* forces boot_note, whose log is a FIFO */
        player_launch_prepare();
        /* what the UI will read back is the contract between the two processes */
        assert(launch_status_get() == (guard && !exported && (reset_rc==0) && !late_export));
        /* Go through the REAL handoff. Printing the sentinel here instead is what let a broken
         * ordinary path pass: only the deadline cases ever reached exec_stock_player(). */
        exec_stock_player();
        assert(0 && "exec_stock_player must never return");
    }
    guard=0; assert(!rmguard_ensure() && resets==0);
    guard=1; exported=1; probes=0; assert(!player_local_mode_confirmed() && resets==0);
    exported=0; reset_rc=-1; probes=0; assert(!player_local_mode_confirmed() && resets==1);
    reset_rc=0; late_export=1; probes=0; assert(!player_local_mode_confirmed() && resets==2);
    late_export=0; probes=0; assert(player_local_mode_confirmed() && resets==3 && probes==2);
    puts("PASS actual host ownership, reset failure and late export are reported independently");
    /* A verdict only authorizes the player that EARNED it. tmpfs gives a boot lifetime, which is not
     * the same thing: the firmware watchdog can respawn the stock player without running our launcher. */
    {   FILE *f;
        pid_t dead = fork();            /* a pid that is certainly no longer that process */
        if(dead == 0) _exit(0);
        waitpid(dead, NULL, 0);
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"guard=1 local=1 pid=%ld\n",(long)getpid()); fclose(f);
        assert(launch_status_get()==1);
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"guard=10 local=10 pid=%ld\n",(long)getpid()); fclose(f);
        assert(launch_status_get()==-1);            /* a substring match would have accepted this */
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"guard=1 local=1 pid=%ld\n",(long)dead); fclose(f);
        assert(launch_status_get()==-1);            /* the player that earned it is gone */
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"nonsense\n"); fclose(f);
        assert(launch_status_get()==-1);
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"guard=1 local=1 pid=%ldjunk\n",(long)getpid()); fclose(f);
        assert(launch_status_get()==-1);            /* trailing junk must not be silently ignored */
        unlink(LAUNCH_STATUS_PATH);
        assert(launch_status_get()==-1);            /* absent is never "safe" */
        /* Once a verdict has been accepted, a DIFFERENT player must not inherit it - otherwise a
         * watchdog respawn that never ran our launcher would keep the card open. */
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"guard=1 local=1 pid=%ld\n",(long)getpid()); fclose(f);
        assert(launch_status_get()==1);             /* pins this generation */
        f=fopen(LAUNCH_STATUS_PATH,"w"); assert(f); fprintf(f,"guard=1 local=1 pid=%ld\n",(long)getppid()); fclose(f);
        assert(launch_status_get()==-1);            /* a different live process cannot adopt it */
        unlink(LAUNCH_STATUS_PATH);
        puts("PASS stale, malformed, junk-suffixed and foreign-pid verdicts are all rejected");
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='diskos-launcher-test-') as temp:
    root=Path(temp);c=root/'test.c';exe=root/'test';c.write_text(harness)
    status=root/'status'; blog=root/'boot.log'
    subprocess.run([os.environ.get('CC','cc'),'-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DLAUNCH_STATUS_PATH="%s"' % status, '-DBOOT_LOG_PATH="%s"' % blog, '-DLAUNCH_DEADLINE_S=2', '-DPLAYER_COMM_NAME="test"',
                    str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    # OWNER CONSTRAINT: diskOS must never deny the stock player. EVERY failure reaches the exec,
    # promptly, and records why - it must never hold, and must never depend on the deadline firing.
    for case,expect in [('guard','guard=0'),('host','local=0'),('reset','local=0'),('late','local=0')]:
        if status.exists(): status.unlink()
        out=subprocess.check_output([str(exe),case],timeout=10)
        assert out==b'PLAYER_EXEC_REACHED\n',(case,out)
        got=status.read_text()
        assert expect in got,(case,got)
    # a healthy boot, a transient database lock and a fresh first run all reach it too, fully confirmed
    for case in ['ok','transient','fresh']:
        if status.exists(): status.unlink()
        assert subprocess.check_output([str(exe),case],timeout=10)==b'PLAYER_EXEC_REACHED\n'
        assert status.read_text().startswith('guard=1 local=1'),(case,status.read_text())
    assert 'UNPROTECTED' in blog.read_text()

    # A hung preparation step must still reach the player: the deadline covers preparation itself.
    if status.exists(): status.unlink()
    assert subprocess.check_output([str(exe), 'hang'], timeout=20) == b'PLAYER_EXEC_REACHED\n'

    # ...and so must a BLOCKED diagnostic. boot_note() writes to NAND; if that write stalls, the old
    # code never reached the exec. Here the log is a FIFO with no reader, so the write blocks forever.
    blog.unlink(); os.mkfifo(blog)
    if status.exists(): status.unlink()
    assert subprocess.check_output([str(exe), 'blockedlog'], timeout=20) == b'PLAYER_EXEC_REACHED\n'
    print('PASS a hung preparation step and a blocked log both still reach the stock player')
    print('PASS every failure still reaches the stock player exec, with its reason published')

# Exercise the real first-run predicate on real disposable paths, including
# a dangling parent error (ENOTDIR must not count as an absent card).
a=s.index('static int player_config_absent_without_card(void){')
b=s.index('/* ---- launch outcome, published by the player launcher',a)
helper=s[a:b].replace('"/usr/data/fiio/db/sysconfig.db"','test_db').replace('"/sys/class/block/mmcblk0"','test_card')
with tempfile.TemporaryDirectory(prefix='diskos-first-run-test-') as temp:
    root=Path(temp);c=root/'test.c';exe=root/'test'
    c.write_text('#include <stdio.h>\n#include <sys/stat.h>\n#include <errno.h>\nstatic char *test_db,*test_card;\n'+helper+'\nint main(int argc,char **argv){ if(argc!=3)return 2;test_db=argv[1];test_card=argv[2];printf("%d\\n",player_config_absent_without_card());}\n')
    subprocess.run(['cc','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    db=root/'sysconfig.db';card=root/'card'
    def check(expected,card_path=card):
        assert subprocess.check_output([str(exe),str(db),str(card_path)])==f'{expected}\n'.encode()
    check(1)
    card.touch();check(0);card.unlink()
    db.touch();check(0);db.unlink()
    parent=root/'not-a-directory';parent.touch();check(0,parent/'card')
    print('PASS first-run initialization requires both absent config and confirmed absent card')

print('COMPLETE launcher_test.py')   # last line: the runner requires it, so an early clean exit cannot pass
