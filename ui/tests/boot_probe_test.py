#!/usr/bin/env python3
"""tools/diskos_bootprobe.c: the native supervisor S96 relies on for bounding and process identity.

Every supervision property is checked twice: on the real source, and on a copy with exactly that protection
removed, which must then FAIL the same observation - so each observation is shown able to detect the failure
it claims to cover.

Hangs are real: MEMDEV is a FIFO, so the probe child blocks inside open(2) until the test releases it. The
test OBSERVES each precondition instead of assuming it: the child blocked in that open (wchan
wait_for_partner), the supervisor actually stopped (state T), the child published and exited successfully
(a zombie with exit code 0 - observable precisely because the supervisor never reaps it before signalling).
No test-only code exists in the shipped binary.

Every supervisor and child is registered the moment it exists and cleaned up in `finally`. Children are held
by pidfd (opened, then confirmed to be the supervisor's child) and signalled through it, so cleanup never
signals a reused pid.

Host build only (x86-64); the shipped mipsel build of the same source is exercised by the S96 tests under qemu
and compared to a fresh build by the runner. Prints COMPLETE as its last line only if every check ran."""
from pathlib import Path
import errno
import os
import signal
import struct
import subprocess
import tempfile
import time

app = Path(__file__).resolve().parents[1]
SRC = (app / 'tools' / 'diskos_bootprobe.c').read_text()
RELEASED = 0xF6EFE127


def variant(*pairs):
    s = SRC
    for old, new in pairs:
        assert s.count(old) == 1, f'variant anchor not unique: {old!r}'
        s = s.replace(old, new)
    return s


def build(src, d, name):
    c = d / f'{name}.c'
    c.write_text(src)
    exe = d / name
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', str(c), '-o', str(exe)], check=True)
    return exe


def stat_fields(pid):
    """(state, ppid, starttime, exit_code) from /proc/<pid>/stat, or None if the pid is gone."""
    try:
        st = Path(f'/proc/{pid}/stat').read_text()
    except (FileNotFoundError, ProcessLookupError):
        return None
    f = st.rsplit(')', 1)[1].split()          # f[0] = field 3 (state)
    return f[0], int(f[1]), int(f[19]), int(f[49]) if len(f) >= 50 else None


def alive(pid):
    s = stat_fields(pid)
    return s is not None and s[0] != 'Z'


def wchan(pid):
    try:
        return Path(f'/proc/{pid}/wchan').read_text()
    except OSError:
        return ''


def children(ppid):
    out = []
    for d in Path('/proc').iterdir():
        if d.name.isdigit():
            s = stat_fields(int(d.name))
            if s and s[1] == ppid:
                out.append(int(d.name))
    return out


def wait_for(pred, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = pred()
        if v:
            return v
        time.sleep(0.01)
    return None


def fail(msg):
    raise SystemExit('FAIL ' + msg)


SUPERVISORS = []   # Popen objects
KIDS = []          # pidfds of probe children: they refer to THAT process for as long as we hold them


def cleanup():
    for p in SUPERVISORS:
        if p.poll() is None:
            try:
                p.send_signal(signal.SIGCONT)
                p.kill()
                p.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
    for fd in KIDS:                                   # signalled through the pidfd: never a reused pid
        try:
            signal.pidfd_send_signal(fd, signal.SIGKILL)
        except OSError:
            pass
        os.close(fd)


def release(fifo, timeout=5.0):
    """Open the FIFO's write end without ever blocking (ENXIO until the reader is there), then close it."""
    end = time.monotonic() + timeout
    while True:
        try:
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
            os.close(fd)
            return
        except OSError as e:
            if e.errno != errno.ENXIO or time.monotonic() > end:
                fail(f'could not release the blocked probe child: {e}')
            time.sleep(0.01)


try:
    with tempfile.TemporaryDirectory(prefix='diskos-bootprobe-test-') as t:
        t = Path(t)
        PDEATH = '    if(prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) _exit(10);       /* die with the supervisor ...        */\n'
        KILLS = '    kill(-pid, SIGKILL);\n    kill(pid, SIGKILL);\n'
        ACCEPT = '        else if(now >= t_end) late = 1;\n'
        SIGDFL = '    if(sigaction(SIGCHLD, &dfl, NULL) != 0){ puts("X 1 -"); return 0; }\n'
        real = build(SRC, t, 'real')
        no_pdeath = build(variant((PDEATH, '')), t, 'no_pdeath')
        # the explicit kill and PDEATHSIG back each other up (PDEATHSIG also fires when the supervisor exits
        # normally), so the explicit kill is isolated with PDEATHSIG removed as well
        no_kill_no_pdeath = build(variant((KILLS, ''), (PDEATH, '')), t, 'no_kill_no_pdeath')
        no_accept = build(variant((ACCEPT, '')), t, 'no_accept')          # ONLY the acceptance check removed
        no_sigdfl = build(variant((SIGDFL, '')), t, 'no_sigdfl')

        mem = t / 'mem'
        with open(mem, 'wb') as f:
            f.truncate(0x10011000)
            f.seek(0x10010100)
            f.write(struct.pack('<I', RELEASED))
        present = t / 'present'
        present.write_text('')
        afile = t / 'afile'
        afile.write_text('')

        # --- answers ----------------------------------------------------------------------------------
        def probe(exe, pref, mnt='/', deadline='2000', memdev=mem):
            return subprocess.run([str(exe), str(pref), str(mnt), deadline, str(memdev)],
                                  capture_output=True, text=True, timeout=30)
        cases = [
            ('present', dict(pref=present), '1 0 0xF6EFE127\n'),
            ('absent (ENOENT)', dict(pref=t / 'nope'), '0 0 0xF6EFE127\n'),
            ('lookup error ENOTDIR', dict(pref=afile / 'x'), 'E 0 0xF6EFE127\n'),
            ('data dir not a mountpoint', dict(pref=t / 'nope', mnt=t), 'E 0 0xF6EFE127\n'),
            ('pin unreadable', dict(pref=t / 'nope', memdev=t / 'no-mem'), '0 1 -\n'),
        ]
        for name, kw, want in cases:
            r = probe(real, **kw)
            if r.returncode != 0 or r.stdout != want:
                fail(f'{name}: rc={r.returncode} out={r.stdout!r} want={want!r}')
        for bad in (['x'], ['a', '/', '0', str(mem)], ['a', '/', 'abc', str(mem)], ['a', '/', '-5', str(mem)]):
            r = subprocess.run([str(real), *bad], capture_output=True, text=True, timeout=10)
            if r.returncode != 2 or r.stdout:
                fail(f'bad usage {bad} gave rc={r.returncode} out={r.stdout!r}')
        print('PASS present / ENOENT / other lstat error / unmounted / unreadable pin / bad usage answers')

        def hang(exe, deadline_ms, sigchld_ignored=False):
            """Start a supervisor whose child is BLOCKED in open(FIFO); return once that is observed."""
            fifo = t / f'fifo-{exe.name}-{time.monotonic_ns()}'
            os.mkfifo(fifo)
            pre = (lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN)) if sigchld_ignored else None
            p = subprocess.Popen([str(exe), str(t / 'nope'), '/', str(deadline_ms), str(fifo)],
                                 stdout=subprocess.PIPE, text=True, preexec_fn=pre)
            SUPERVISORS.append(p)
            kid = wait_for(lambda: children(p.pid))
            if not kid:
                fail(f'{exe.name}: probe child never appeared')
            kid = kid[0]
            # take a pidfd, then confirm it is still our supervisor's child: from here on the pidfd names
            # exactly this process, whatever happens to the numeric pid
            try:
                fd = os.pidfd_open(kid)
            except OSError as e:
                fail(f'{exe.name}: could not open a pidfd for the probe child: {e}')
            KIDS.append(fd)
            s = stat_fields(kid)
            if not s or s[1] != p.pid:
                fail(f'{exe.name}: pid {kid} is no longer the supervisor\'s child')
            if not wait_for(lambda: 'wait_for_partner' in wchan(kid)):
                fail(f'{exe.name}: probe child never blocked in the FIFO open (wchan={wchan(kid)!r})')
            return p, kid, fifo, time.monotonic()      # observed-blocked time: necessarily after the probe's t0

        # --- deadline + kill ------------------------------------------------------------------------------
        for exe in (real, no_pdeath):                # no_pdeath: the explicit kill alone must do it
            t0 = time.monotonic()
            p, kid, fifo, _ = hang(exe, 1000)
            out, _ = p.communicate(timeout=20)
            el = time.monotonic() - t0
            if out != 'T 1 -\n':
                fail(f'{exe.name} hung probe: supervisor said {out!r}, want timeout')
            if not (0.9 <= el < 5):
                fail(f'{exe.name} hung probe: supervisor returned after {el:.2f}s for a 1s deadline')
            if wait_for(lambda: not alive(kid), 3) is None:
                fail(f'{exe.name}: hung probe child survived the supervisor')
        p, kid, fifo, _ = hang(no_kill_no_pdeath, 1000)
        p.communicate(timeout=20)
        time.sleep(0.3)
        if not alive(kid):
            fail('positive control: with the kills (and PDEATHSIG) removed the blocked child should survive')
        print('PASS a blocked probe gives "T" within its deadline and is killed, by the explicit kill alone too '
              '(control: survives without it)')

        # --- supervisor death -----------------------------------------------------------------------------
        for exe, want_dies in ((real, True), (no_pdeath, False)):
            p, kid, fifo, _ = hang(exe, 60000)       # returns only once the child is blocked (past prctl)
            os.kill(p.pid, signal.SIGKILL)
            p.wait(timeout=10)
            died = wait_for(lambda: not alive(kid), 2) is not None
            if died != want_dies:
                fail(f'{exe.name}: after the supervisor was killed the child {"survived" if not died else "died"}')
        print('PASS killing the supervisor kills the probe child (control: survives without PR_SET_PDEATHSIG)')

        # --- late result, and SIGCHLD ---------------------------------------------------------------------
        def late(exe, resume_after_deadline, sigchld_ignored=False):
            """Stop the supervisor (observed), release the child, observe it publish and exit, then resume the
            supervisor before or after the deadline. Returns (supervisor output, child state seen)."""
            deadline_ms = 1500
            start = time.monotonic()                 # BEFORE launch: a lower bound for the probe's own t0
            p, kid, fifo, blocked = hang(exe, deadline_ms, sigchld_ignored)   # AFTER the probe's t0
            os.kill(p.pid, signal.SIGSTOP)
            if not wait_for(lambda: (stat_fields(p.pid) or ('?',))[0] in ('T', 't')):
                fail(f'{exe.name}: the supervisor did not stop')
            release(fifo)
            # published and exited: a zombie with exit code 0 - or, if it was auto-reaped, gone
            seen = wait_for(lambda: (lambda s: 'gone' if s is None else ('Z' if s[0] == 'Z' else None))(
                stat_fields(kid)), 5)
            if seen is None:
                fail(f'{exe.name}: the child did not exit while the supervisor was stopped')
            if seen == 'Z' and stat_fields(kid)[3] != 0:
                fail(f'{exe.name}: the child exited with {stat_fields(kid)[3]}, not a successful publication')
            # The probe's deadline lies in [start + 1.5s, blocked + 1.5s]. Resume strictly outside it:
            #  after:  blocked + deadline + 0.6  (>= the probe's deadline + 0.6, however slow its startup was)
            #  before: start + 0.6               (<= the probe's deadline - 0.9, however fast)
            resume_at = (blocked + deadline_ms / 1000 + 0.6) if resume_after_deadline else (start + 0.6)
            if not resume_after_deadline and time.monotonic() > resume_at:
                fail(f'{exe.name}: setup overran the pre-deadline resume point - control not meaningful')
            time.sleep(max(0.0, resume_at - time.monotonic()))
            os.kill(p.pid, signal.SIGCONT)
            out, _ = p.communicate(timeout=20)
            return out, seen

        out, seen = late(real, True)
        if seen != 'Z':
            fail('the published child was not retained as a zombie by the supervisor')
        if out != 'T 1 -\n':
            fail(f'a result read after the deadline was accepted: {out!r}')
        out, _ = late(real, False)
        if out != '0 1 -\n':
            fail(f'control: the same published result, read before the deadline, was not accepted: {out!r}')
        out, _ = late(no_accept, True)
        if out != '0 1 -\n':
            fail('positive control: without ONLY the post-read acceptance check, the late result should be accepted')
        print('PASS a published result read after the deadline is rejected by the single post-read check; the '
              'identical sequence before it is accepted (control: accepted late without that one check)')

        out, seen = late(real, True, sigchld_ignored=True)
        if seen != 'Z':
            fail('with SIGCHLD ignored by the caller, the child was auto-reaped - its pid is no longer ours')
        out, seen = late(no_sigdfl, True, sigchld_ignored=True)
        if seen != 'gone':
            fail('positive control: without the SIGCHLD reset an inherited SIG_IGN should auto-reap the child')
        print('PASS an inherited ignored SIGCHLD does not auto-reap the child (control: it does without the reset)')
finally:
    cleanup()

print('COMPLETE boot_probe_test.py')
