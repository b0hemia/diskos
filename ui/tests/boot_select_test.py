#!/usr/bin/env python3
"""The boot-selection record has three readers that must never disagree: main.c's boot_select_is_diskos(),
and the shipped shell helper payload/diskos-selected (used by S97 and the boot hook). This compiles the
REAL C reader and enforcement out of main.c and runs the REAL helper, under the host shell and, when
available, under the V2.40 firmware BusyBox ash via qemu with the firmware's own wc applet - because ash
`read` drops NUL bytes, which is exactly the kind of difference that makes readers disagree.

Also checks boot_select_enforce(): a non-diskos record must end in the stock exec or in _exit(127), never
fall through into diskOS, including when the stock exec fails.

Prints COMPLETE as its last line only if every check ran; the runner requires it."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile

app = Path(__file__).resolve().parents[1]
s = (app / 'main.c').read_text()
a = s.index('/* ---- boot select record (tests slice')
b = s.index('/* ---- end boot select record ---- */')

# the helper ships in the installer payload; dev tree and public tree sit at different depths
helper = next((p for p in (app.parents[1] / 'installer' / 'payload' / 'diskos-selected',
                           app.parent / 'payload' / 'diskos-selected') if p.is_file()), None)
if helper is None:
    raise SystemExit('FAIL cannot find payload/diskos-selected next to this tree')
# The stock firmware whose shell runs the helper: DISKOS_FW_BASE picks the version (default V2.40) and
# DISKOS_FW_ROOT its extracted rootfs. The BusyBox must be that version's pinned binary, so a run cannot
# quietly use another version's shell and report it as this one.
FW_BASE = os.environ.get('DISKOS_FW_BASE', '240')
FW_BUSYBOX_MD5 = {'209': 'e6b2bb328e1c08238878991c0aae062d', '228': 'c1af60e81cea22b040b957c00404de9e',
                  '240': '5d8a5e60ee7d40002fe70f43228596cb'}
if FW_BASE not in FW_BUSYBOX_MD5:
    raise SystemExit(f'FAIL unknown DISKOS_FW_BASE={FW_BASE!r}')
FW = Path(os.environ.get('DISKOS_FW_ROOT') or os.environ.get('DISKOS_V240_ROOT', '/tmp/vs6'))
QEMU = shutil.which('qemu-mipsel-static')
_fw_ok = bool(QEMU) and (FW / 'bin' / 'busybox').is_file() \
    and hashlib.md5((FW / 'bin' / 'busybox').read_bytes()).hexdigest() == FW_BUSYBOX_MD5[FW_BASE]
if os.environ.get('DISKOS_REQUIRE_FW') == '1' and not _fw_ok:
    raise SystemExit(f'FAIL firmware ash required (DISKOS_REQUIRE_FW=1) but qemu or the pinned V{FW_BASE} rootfs is missing at {FW}')

harness = r'''
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int g_reset = 0;
static void handoff_signal_reset(void){ g_reset = 1; }
static int fake_execv(const char *p, char *const v[]){
    printf("EXEC %s %s reset=%d\n", p, v[0], g_reset); fflush(stdout);
    return getenv("EXEC_FAILS") ? -1 : (_exit(0), 0);
}
#define execv(p,a) fake_execv((p),(a))
''' + s[a:b] + r'''
int main(int argc, char **argv){
    if(argc == 3 && !strcmp(argv[1], "read")){ printf("%d\n", boot_select_is_diskos(argv[2])); return 0; }
    boot_select_enforce(argv[2]);
    printf("FELL_THROUGH_TO_DISKOS\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='diskos-bootsel-') as t:
    t = Path(t)
    (t / 'h.c').write_text(harness)
    exe = t / 'h'
    subprocess.run([os.environ.get('CC', 'cc'), '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', str(t / 'h.c'), '-o', str(exe)], check=True)

    shells = {'host-sh': ['sh', str(helper)]}
    if _fw_ok:
        fwbin = t / 'fwbin'
        fwbin.mkdir()
        for applet in ('wc',):
            w = fwbin / applet
            w.write_text(f'#!/bin/sh\nexec {QEMU} -L {FW} {FW}/bin/busybox {applet} "$@"\n')
            w.chmod(0o755)
        shells[f'v{FW_BASE}-ash'] = [QEMU, '-L', str(FW), str(FW / 'bin' / 'busybox'), 'sh', str(helper)]
    else:
        fwbin = None
        print('NOTE firmware ash unavailable: comparing against the host shell only')

    def c_says(path):
        out = subprocess.run([str(exe), 'read', str(path)], capture_output=True, timeout=5, check=True).stdout
        if out not in (b'0\n', b'1\n'):
            raise SystemExit(f'FAIL unexpected harness output {out!r}')
        return out == b'1\n'

    def shell_says(name, path):
        env = dict(os.environ, DISKOS_BOOT_SELECT=str(path))
        if name == 'v240-ash':
            env['PATH'] = f'{fwbin}:{env["PATH"]}'
        r = subprocess.run(shells[name], env=env, timeout=10, capture_output=True)
        if r.returncode not in (0, 1):
            raise SystemExit(f'FAIL {name} helper exited {r.returncode}: {r.stderr!r}')
        return r.returncode == 0

    fixtures = {
        'exact "diskos\\n"': (b'diskos\n', True),
        'no newline': (b'diskos', False),
        'two newlines': (b'diskos\n\n', False),
        'stock': (b'stock\n', False),
        'empty': (b'', False),
        'trailing space': (b'diskos \n', False),
        'leading space': (b' diskos\n', False),
        'uppercase': (b'DISKOS\n', False),
        'second line': (b'diskos\nstock\n', False),
        'NUL inside (ash read drops NULs)': (b'dis\0kos\n', False),
        'NUL before newline': (b'diskos\0\n', False),
        'NUL replacing newline': (b'diskos\0', False),
        'CR LF': (b'diskos\r\n', False),
    }
    for i, (name, (content, want)) in enumerate(fixtures.items()):
        p = t / f'rec{i}'
        p.write_bytes(content)
        got = {'C': c_says(p)}
        for sh in shells:
            got[sh] = shell_says(sh, p)
        if any(v != want for v in got.values()):
            raise SystemExit(f'FAIL {name}: expected {want}, got {got}')
    print(f'PASS C reader and shell helper ({", ".join(shells)}) agree on {len(fixtures)} record contents')

    good = t / 'good'
    good.write_bytes(b'diskos\n')
    link = t / 'link'
    link.symlink_to(good)
    d = t / 'dir'
    d.mkdir()
    fifo = t / 'fifo'
    os.mkfifo(fifo)
    special = {'missing': t / 'nope', 'symlink to a valid record': link, 'directory': d, 'FIFO': fifo}
    for name, p in special.items():
        try:
            got = {'C': c_says(p)}
            for sh in shells:
                got[sh] = shell_says(sh, p)
        except subprocess.TimeoutExpired:
            raise SystemExit(f'FAIL a reader BLOCKED on the {name} fixture')
        if any(got.values()):
            raise SystemExit(f'FAIL {name} accepted: {got}')
    print('PASS missing, symlink, directory and FIFO records read as stock in every reader, none blocks')

    def enforce(path, exec_fails):
        env = dict(os.environ)
        if exec_fails:
            env['EXEC_FAILS'] = '1'
        return subprocess.run([str(exe), 'enforce', str(path)], env=env, capture_output=True, timeout=5)

    r = enforce(good, False)
    if r.returncode != 0 or r.stdout != b'FELL_THROUGH_TO_DISKOS\n':
        raise SystemExit(f'FAIL a valid record did not continue into diskOS: {r.returncode} {r.stdout!r}')
    stock = t / 'stockrec'
    stock.write_bytes(b'stock\n')
    for path in (stock, t / 'nope'):
        r = enforce(path, False)
        if r.returncode != 0 or r.stdout != b'EXEC /usr/bin/mq_ui mq_ui reset=1\n':
            raise SystemExit(f'FAIL {path.name}: did not hand off to stock with signals reset: {r.stdout!r}')
        r = enforce(path, True)
        if b'FELL_THROUGH' in r.stdout or r.returncode != 127:
            raise SystemExit(f'FAIL {path.name}: a failed stock exec fell through into diskOS: '
                             f'{r.returncode} {r.stdout!r}')
    print('PASS non-diskos records hand off to stock; a failed stock exec exits 127, never runs diskOS')

print('COMPLETE boot_select_test.py')
