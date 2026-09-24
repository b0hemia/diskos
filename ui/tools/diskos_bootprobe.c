/* diskos-bootprobe: the bounded, supervised hardware/NAND probe for the boot selector (S96diskos_select).
 *
 * Usage: diskos-bootprobe PREF DATA_MNT DEADLINE_MS [MEMDEV]
 * Prints exactly one line "<flag> <pinrc> <value>" and exits 0:
 *   flag  1 = PREF exists (lstat, any type), 0 = PREF absent (ENOENT only, with DATA_MNT a mountpoint),
 *         E = could not establish either (DATA_MNT not a mountpoint, or any other lstat error)
 *   pinrc 0 = the GPB pin register was read; value = 0x%08x. Anything else = not read.
 * or "T 1 -" when the deadline passed, and "X 1 -" on a supervisor failure. S96 maps everything except a
 * valid 0/1 flag with pinrc 0 to stock. Bad usage exits 2 with no output (also stock).
 *
 * WHY NATIVE: the shell versions could not both bound the probe and keep hold of its identity. Here:
 *  - the probe is a CHILD we never reap before signalling it, so its pid cannot be reused under us - an
 *    exited-but-unreaped child keeps its identity until waitpid. That needs SIGCHLD at its default: an
 *    ignored SIGCHLD survives exec and makes the kernel reap children automatically, so we reset it
 *    (checked) before forking;
 *  - it leads its own process group (set by both sides of the fork, so it holds before either proceeds);
 *  - PR_SET_PDEATHSIG kills it if this supervisor dies, so killing S96 or us cannot leave it behind;
 *  - it reports over a private pipe and has stdout/stderr on /dev/null, so a child stuck in the kernel
 *    cannot hold S96's command substitution open;
 *  - the deadline is CLOCK_MONOTONIC. It bounds how long we WAIT, and a single check made AFTER the whole
 *    result has been received decides whether it is ACCEPTED, so a result that arrives late (e.g. we were
 *    descheduled past the deadline) is never accepted;
 *  - every setup step the guarantees rest on is checked: if the child cannot set its death signal, its
 *    process group or its /dev/null redirection, it exits WITHOUT publishing (reported as "X"), so it never
 *    does hardware work while still holding the caller's stdout or outliving us.
 * A child stuck in uninterruptible NAND I/O cannot be killed until the I/O returns; we do not wait for it.
 *
 * The pin: x2000 pinctrl at 0x10010000, GPB PxPIN at +0x100, Vol-Up = bit 13 (active low). Read through a
 * 4 KiB read-only mapping of MEMDEV (/dev/mem), as main.c did (validated on-device 2026-08-13). MEMDEV is
 * an argument only so host tests can supply a file; S96 passes /dev/mem. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PINCTRL_BASE 0x10010000UL
#define GPB_PXPIN    0x100

static long long now_ms(void){       /* -1 if the clock cannot be read; callers treat that as failure */
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 1 if dir is a mount point (different device from its parent, or the root itself), 0 if not, -1 error */
static int is_mountpoint(const char *dir){
    struct stat a, b;
    char parent[4096];
    if(stat(dir, &a) != 0 || !S_ISDIR(a.st_mode)) return -1;
    if(snprintf(parent, sizeof parent, "%s/..", dir) >= (int)sizeof parent) return -1;
    if(stat(parent, &b) != 0) return -1;
    return (a.st_dev != b.st_dev || a.st_ino == b.st_ino) ? 1 : 0;
}

static void child(int wfd, pid_t supervisor, const char *pref, const char *mnt, const char *memdev){
    /* Every step below is a precondition of the supervisor's guarantees; if one fails, exit without
     * publishing (the supervisor reports "X") and without touching NAND or /dev/mem. */
    if(prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) _exit(10);       /* die with the supervisor ...        */
    if(getppid() != supervisor) _exit(11);                     /* ... which may already have died    */
    if(setpgid(0, 0) != 0 && getpgrp() != getpid()) _exit(12); /* our own group (parent may have set it) */
    int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    if(nul < 0) _exit(13);
    if(dup2(nul, 0) < 0 || dup2(nul, 1) < 0 || dup2(nul, 2) < 0) _exit(14);  /* never hold S96's stdout */
    if(nul > 2) close(nul);

    char flag = 'E';
    if(is_mountpoint(mnt) == 1){
        struct stat st;
        if(lstat(pref, &st) == 0) flag = '1';
        else if(errno == ENOENT) flag = '0';
    }

    int pinrc = 1;
    uint32_t gpb = 0;
    int fd = open(memdev, O_RDONLY | O_SYNC | O_CLOEXEC);
    if(fd >= 0){
        void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (off_t)PINCTRL_BASE);
        if(map != MAP_FAILED){
            gpb = *(volatile uint32_t *)((char *)map + GPB_PXPIN);
            pinrc = 0;
            munmap(map, 4096);
        }
        close(fd);
    }

    char line[64];
    int n = pinrc == 0 ? snprintf(line, sizeof line, "%c 0 0x%08X\n", flag, (unsigned)gpb)
                       : snprintf(line, sizeof line, "%c 1 -\n", flag);
    if(n > 0 && write(wfd, line, (size_t)n) != n) _exit(1);
    _exit(0);
}

int main(int argc, char **argv){
    if(argc < 4 || argc > 5) return 2;
    char *end = NULL;
    long deadline = strtol(argv[3], &end, 10);
    if(!end || *end || deadline <= 0 || deadline > 600000) return 2;
    const char *memdev = argc == 5 ? argv[4] : "/dev/mem";

    /* default SIGCHLD, so the child stays unreaped (and its pid ours) until we signal it */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    if(sigaction(SIGCHLD, &dfl, NULL) != 0){ puts("X 1 -"); return 0; }

    long long t0 = now_ms();
    if(t0 < 0){ puts("X 1 -"); return 0; }
    long long t_end = t0 + deadline;
    int p[2];
    if(pipe2(p, O_CLOEXEC) != 0){ puts("X 1 -"); return 0; }
    pid_t self = getpid();
    pid_t pid = fork();
    if(pid < 0){ puts("X 1 -"); return 0; }
    if(pid == 0){ close(p[0]); child(p[1], self, argv[1], argv[2], memdev); }
    close(p[1]);
    setpgid(pid, pid);                           /* also from this side: no window before the group exists */

    /* WAIT, bounded by the deadline. Once it has passed we still drain what is ALREADY in the pipe
     * (poll with 0) - accepting it or not is decided below, by one check, not here. */
    char buf[64];
    size_t got = 0;
    int done = 0, late = 0, failed = 0;
    while(!done){
        long long now = now_ms();
        if(now < 0){ failed = 1; break; }
        long long left = t_end - now;
        struct pollfd pf = { p[0], POLLIN, 0 };
        int r = poll(&pf, 1, left > 0 ? (int)left : 0);
        if(r < 0){ if(errno == EINTR) continue; break; }
        if(r == 0){ late = 1; break; }           /* nothing arrived within the deadline */
        ssize_t k = read(p[0], buf + got, sizeof buf - 1 - got);
        if(k < 0){ if(errno == EINTR) continue; break; }
        if(k == 0) done = 1;                     /* EOF: the child closed the pipe (normally by exiting) */
        else { got += (size_t)k; if(got >= sizeof buf - 1) break; }
    }
    /* ACCEPTANCE: the single deadline check, made after the whole result has been received. */
    {
        long long now = now_ms();
        if(now < 0) failed = 1;              /* cannot tell whether it was on time: never accept */
        else if(now >= t_end) late = 1;
    }

    /* The child is unreaped, so pid and pgid are still ours: signal both, whatever happened. */
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, WNOHANG);                 /* never block on a child stuck in the kernel */

    buf[got] = 0;
    if(failed){ puts("X 1 -"); return 0; }
    if(late){ puts("T 1 -"); return 0; }
    if(!done || got == 0 || buf[got - 1] != '\n' || memchr(buf, '\n', got) != buf + got - 1){
        puts("X 1 -");
        return 0;
    }
    fputs(buf, stdout);
    return 0;
}
