"""Exercise firmware guard installation and real rm behavior on disposable files."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
from diskos_installer import imagebuild


class ImageGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='diskos-image-guard-')
        self.root = Path(self.tmp.name)
        self.rf = self.root / 'rf'
        (self.rf / 'bin').mkdir(parents=True)
        self.bb = self.rf / 'bin/busybox'
        self.bb.write_bytes(b'busybox-must-remain-byte-identical')
        self.rm = self.rf / 'bin/rm'

    def tearDown(self):
        self.tmp.cleanup()

    def install(self):
        before = self.bb.read_bytes()
        digest = imagebuild._install_sd_guards(str(self.rf))
        self.assertEqual(self.bb.read_bytes(), before)
        self.assertEqual(digest, hashlib.sha256(before).hexdigest())
        self.assertFalse(self.rm.is_symlink())
        imagebuild._validate_sd_guards(str(self.rf), digest)
        return digest

    def test_unknown_boot_script_is_refused_even_with_the_override(self):
        """Only the pinned stock script is ever patched: the patch's safety rests on its exact structure,
        and no heuristic can establish shell control flow for an unknown one."""
        script = self.root / 'fiio_init.sh'
        script.write_text('#!/bin/sh\nif [ "$COREDUMP_FLAG" == "1" ]; then\n    /usr/data/mq_ui &\n    sleep 2\n    /usr/data/mq_player &\nelse\n    mq_ui &\n    sleep 2\n    mq_player &\nfi\n')
        before = script.read_bytes()
        for env in ({}, {'DISKOS_ALLOW_UNTESTED_FW': '1'}):
            with self.subTest(env=env), mock.patch.dict(os.environ, env):
                with self.assertRaises(imagebuild.BuildError):
                    imagebuild._patch_fiio_init(str(script), None)
        self.assertEqual(script.read_bytes(), before, 'a refused script must be left untouched')

    def test_stock_symlink_targets_never_overwritten(self):
        for target in ('busybox', '/bin/busybox'):
            with self.subTest(target=target):
                self.rm.symlink_to(target)
                self.install()
                self.rm.unlink()

    def test_hardlinked_rm_never_truncates_busybox(self):
        os.link(self.bb, self.rm)
        self.install()

    def test_outside_final_symlink_is_unlinked_not_followed(self):
        outside = self.root / 'host-sentinel'
        outside.write_bytes(b'untouched')
        self.rm.symlink_to(outside)
        self.install()
        self.assertEqual(outside.read_bytes(), b'untouched')

    def test_parent_symlink_escape_rejected(self):
        self.bb.unlink()
        (self.rf / 'bin').rmdir()
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'busybox').write_bytes(b'untouched')
        (self.rf / 'bin').symlink_to(outside)
        with self.assertRaises(imagebuild.BuildError):
            imagebuild._install_sd_guards(str(self.rf))
        self.assertEqual((outside / 'busybox').read_bytes(), b'untouched')

    def test_output_rejects_missing_changed_nonexec_and_symlink_guards(self):
        digest = self.install()
        payload = self.rm.read_bytes()
        for rel in ('bin/rm', 'opt/diskos/bin/rm'):
            target = self.rf / rel
            for kind in ('missing', 'changed', 'nonexec', 'symlink'):
                with self.subTest(rel=rel, kind=kind):
                    target.unlink()
                    if kind == 'changed': target.write_bytes(payload + b'altered')
                    elif kind == 'nonexec': target.write_bytes(payload); target.chmod(0o644)
                    elif kind == 'symlink': target.symlink_to(self.bb)
                    with self.assertRaises(imagebuild.BuildError):
                        imagebuild._validate_sd_guards(str(self.rf), digest)
                    if target.exists() or target.is_symlink(): target.unlink()
                    target.write_bytes(payload); target.chmod(0o755)
        self.bb.write_bytes(b'corrupt-busybox')
        with self.assertRaises(imagebuild.BuildError):
            imagebuild._validate_sd_guards(str(self.rf), digest)


class ShellGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='diskos-shell-guard-')
        self.root = Path(self.tmp.name)
        self.mount = self.root / 'sdcard'
        self.mount.mkdir()
        self.music = self.mount / 'Music'
        self.music.mkdir()
        self.track = self.music / 'track.mp3'
        self.track.write_bytes(b'precious-music')
        self.alias = self.root / 'alias'
        self.alias.symlink_to(self.mount)
        bb = shutil.which('busybox')
        if not bb: self.skipTest('BusyBox required to execute the deployment shell primitives')
        payload = Path(imagebuild.bundle.data('diskos-rmguard')).read_text()
        payload = payload.replace('/tmp/sdcard', str(self.mount))
        payload = payload.replace('/|/tmp|/tmp/|', '/|' + str(self.root) + '|' + str(self.root) + '/|')
        payload = payload.replace('/usr/data/diskos_rmguard.log', str(self.root / 'no-log-dir/log'))
        payload = payload.replace('/bin/busybox', bb)
        self.guard = self.root / 'guard'
        self.guard.write_text(payload)
        self.guard.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def guarded(self, operand, cwd=None):
        result = subprocess.run([str(self.guard), '-rf', '--', operand], cwd=cwd,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.track.read_bytes(), b'precious-music')

    def test_failed_unmount_cleanup_keeps_music(self):
        # Deliberately failed unmount followed by the actual shell wrapper; no mount privileges needed.
        subprocess.run(['/bin/sh', '-c', 'false; "$1" -rf "$2"', 'test', str(self.guard), str(self.mount)],
                       check=True, capture_output=True, timeout=5)
        self.assertEqual(self.track.read_bytes(), b'precious-music')

    def test_aliases_subpaths_and_relative_paths_keep_music(self):
        operands = [str(self.mount), str(self.mount)+'/', str(self.mount)+'//',
                    str(self.root)+'//sdcard', str(self.root)+'/./sdcard',
                    str(self.music), str(self.track), str(self.alias)+'/Music',
                    'sdcard', './sdcard/Music', 'alias/Music/track.mp3', '.']
        for operand in operands:
            with self.subTest(operand=operand): self.guarded(operand, self.root)
        self.guarded('.', self.mount)
        self.guarded('track.mp3', self.music)

    def test_mixed_operands_do_not_partially_delete(self):
        other = self.root / 'ordinary'
        other.write_bytes(b'keep-this-too')
        subprocess.run([str(self.guard), '-rf', str(other), str(self.music)],
                       check=True, capture_output=True, timeout=5)
        self.assertTrue(other.exists())
        self.assertTrue(self.track.exists())

    def test_normal_cleanup_and_empty_mountpoint_work(self):
        other = self.root / 'sdcard-other'
        other.mkdir(); (other / 'file').write_bytes(b'ordinary')
        subprocess.run([str(self.guard), '-rf', str(other)], check=True)
        self.assertFalse(other.exists())
        self.track.unlink(); self.music.rmdir()
        subprocess.run([str(self.guard), '-rf', str(self.mount)], check=True, capture_output=True)
        self.assertFalse(self.mount.exists())



def _diskos_selected(temp):
    """S97 does nothing unless S96 recorded "diskos": point it at a private record that says so."""
    rec = Path(temp) / 'boot_select'
    rec.write_text('diskos\n')
    helper = Path(__file__).resolve().parents[1] / 'payload' / 'diskos-selected'
    return dict(os.environ, DISKOS_BOOT_SELECT=str(rec), DISKOS_SELECTED=str(helper))


def _alive(pid):
    """True if pid exists and is not a zombie (a zombie answers kill -0 but runs nothing). Only a missing
    /proc entry counts as gone; any other error reading it propagates instead of passing as death."""
    try:
        stat = Path(f'/proc/{pid}/stat').read_text()
    except (FileNotFoundError, ProcessLookupError):
        return False
    return stat.rsplit(')', 1)[1].split()[0] != 'Z'


def _supervisor_wrapper(text):
    """Lift the REAL supervisor out of the shipped hook, with only its deadline shortened.

    Keyed on explicit markers in the script: if someone edits the supervisor, these tests keep
    testing the edited version rather than silently slicing the wrong region.
    """
    import re
    begin = text.index('# >>> SUPERVISOR BEGIN')
    end = text.index('# <<< SUPERVISOR END')
    return re.sub(r'\b180\b', '2', text[begin:end])


class InstallHookSupervisorTests(unittest.TestCase):
    """The boot runs S97 synchronously, so a hung worker must not hold the boot.

    Exercises the REAL supervisor wrapper out of the shipped script, with a worker body
    replaced by a long sleep and the bound shortened, so the test itself stays quick.
    """

    def test_supervisor_returns_promptly_and_worker_is_killed(self):
        import re
        import time
        src = Path(__file__).resolve().parents[1] / 'payload' / 'S97diskos_install'
        text = src.read_text()
        wrapper = _supervisor_wrapper(text)
        with tempfile.TemporaryDirectory(prefix='diskos-s97-sup-') as temp:
            script = Path(temp) / 'S97test'
            started = Path(temp) / 'worker-started'
            marker = Path(temp) / 'worker-finished'
            script.write_text('#!/bin/sh\n' + wrapper +
                              'echo $$ > "%s"\nsleep 60\ntouch "%s"\n' % (started, marker))
            script.chmod(0o755)
            start = time.monotonic()
            try:
                rc = subprocess.run([str(script), 'start'], env=_diskos_selected(temp), timeout=30).returncode
                elapsed = time.monotonic() - start
                # without this witness, a worker that never started would pass every assertion below
                self.assertTrue(started.exists(), 'the worker never started - the test would pass vacuously')
                worker = int(started.read_text())
                time.sleep(0.5)
                self.assertFalse(_alive(worker), 'the hung worker is still running after the supervisor returned')
                self.assertFalse(marker.exists(), 'the hung worker should have been killed, not completed')
            finally:
                try:
                    os.kill(int(started.read_text()), 9)   # failure path: never leave the worker behind
                except (OSError, ValueError):
                    pass
        self.assertEqual(rc, 0, 'the boot must never wait on our verdict')
        self.assertLess(elapsed, 10, 'supervisor did not enforce its bound')

    def test_supervisor_kills_the_workers_descendants(self):
        """Killing only the worker leaves its children running against the card.

        A surviving copy or mount command can still be working after stock has started,
        which is the condition the whole guard exists to avoid.
        """
        import re
        import time
        src = Path(__file__).resolve().parents[1] / 'payload' / 'S97diskos_install'
        text = src.read_text()
        wrapper = _supervisor_wrapper(text)
        for positive_control in (False, True):
            with self.subTest(positive_control=positive_control):
                self.assertEqual(self._descendant_survives(wrapper, positive_control), positive_control,
                                 'positive control: without the group kill the descendant MUST survive'
                                 if positive_control else 'a descendant of the killed worker kept running')

    def _descendant_survives(self, wrapper, disable_group_kill):
        import time
        if disable_group_kill:
            needle = 'kill -KILL -"$_wpg" 2>/dev/null'
            self.assertEqual(wrapper.count(needle), 1)
            wrapper = wrapper.replace(needle, ':')
        with tempfile.TemporaryDirectory(prefix='diskos-s97-desc-') as temp:
            child = Path(temp) / 'descendant.pid'
            worker = Path(temp) / 'worker.pid'
            script = Path(temp) / 'S97test'
            # the descendant records its pid, then becomes a long sleep with that same pid; the worker
            # records its own pid and becomes a long sleep too, so cleanup can find both
            script.write_text('#!/bin/sh\n' + wrapper +
                              '/bin/sh -c \'echo $$ > "%s"; exec sleep 300\' &\n'
                              'echo $$ > "%s"\nexec sleep 60\n' % (child, worker))
            script.chmod(0o755)
            try:
                subprocess.run([str(script), 'start'], env=_diskos_selected(temp), timeout=30, check=True)
                self.assertTrue(child.exists(), 'the descendant never started - the test would pass vacuously')
                pid = int(child.read_text())
                time.sleep(0.5)
                return _alive(pid)
            finally:
                # read the witnesses HERE, so a timeout or failed assertion above still finds them
                for w in (child, worker):
                    try:
                        os.kill(int(w.read_text()), 9)
                    except (OSError, ValueError):
                        pass


class ReadinessGateTests(unittest.TestCase):
    """A worker that never completes must not leave the override launchable."""

    def test_marker_is_written_only_through_disarm(self):
        text = (Path(__file__).resolve().parents[1] / 'payload' / 'S97diskos_install').read_text()
        self.assertIn('ready() { override_on && : > /tmp/.diskos_ready', text)
        self.assertIn('disarm() { ready;', text)
        # the marker must live on tmpfs, so that it can only ever mean "this boot"
        self.assertNotIn('/usr/data/.diskos_ready', text)


_STOCK_FIIO_INIT = Path(os.environ.get(
    'DISKOS_STOCK_FIIO_INIT',
    Path(__file__).resolve().parents[2] / 'analysis/firmware-audit-2026-09-05/stock-v240/usr/project/fiio_init.sh'))


@unittest.skipUnless(_STOCK_FIIO_INIT.is_file(), 'needs a pristine stock fiio_init.sh (not shipped in this repo)')
class BootHookValidatorOnRealFirmwareTests(unittest.TestCase):
    """The output validator, run on the REAL stock boot script after the real patch.

    The pristine script is byte-identical on V1.95/V2.28/V2.40 (md5 e6f12b8a...); no pristine V2.09 copy was
    available to check. The previous validator rejected this exact script (it demanded the selection on
    every line mentioning COREDUMP_FLAG, including the plain assignment) and never tied the primary launch
    to its own condition."""

    def patched(self):
        with tempfile.TemporaryDirectory() as t:
            f = Path(t) / 'fiio_init.sh'
            shutil.copy(_STOCK_FIIO_INIT, f)
            imagebuild._patch_fiio_init(str(f), None)
            return f.read_bytes()

    def test_real_patched_script_is_accepted(self):
        out = self.patched()
        imagebuild._check_boot_script_output(out, out)
        subprocess.run(['/bin/sh', '-n', '-c', out.decode()], check=True)

    def test_patch_is_deterministic_and_keeps_the_guard_export_first(self):
        out = self.patched()
        self.assertEqual(out, self.patched())
        t = out.decode()
        self.assertLess(t.index(imagebuild._GUARD_PATH_LINE), t.index('/usr/data/mq_ui &'))
        self.assertLess(t.index(imagebuild._GUARD_PATH_LINE), t.index('    mq_ui &'))

    def test_pristine_base_is_the_pinned_one_and_others_are_refused(self):
        import hashlib
        self.assertIn(hashlib.sha256(_STOCK_FIIO_INIT.read_bytes()).hexdigest(), imagebuild.KNOWN_FIIO_INIT_SHA256)
        for name, data in (('extra line', _STOCK_FIIO_INIT.read_bytes() + b'\n# one extra line\n'),
                           ('CRLF line endings', _STOCK_FIIO_INIT.read_bytes().replace(b'\n', b'\r\n'))):
            with self.subTest(base=name), tempfile.TemporaryDirectory() as t:
                f = Path(t) / 'fiio_init.sh'
                f.write_bytes(data)
                with self.assertRaises(imagebuild.BuildError):
                    imagebuild._patch_fiio_init(str(f), None)

    def test_every_output_mutation_is_rejected(self):
        """Every mutation of the patched output fails the output check - including the ones the old
        line heuristic accepted, and the single-CR one that text-mode reading normalised away."""
        out = self.patched()
        t = out.decode()
        sel = imagebuild._SELECT_LINE
        tail = 'sleep 3\nwhile true'
        variants = {
            'unpatched stock': _STOCK_FIIO_INIT.read_bytes(),
            'primary launch loses the selection': t.replace('if ' + sel + ' && ', 'if ', 1),
            'coredump branch loses the selection': t.replace('elif ' + sel + ' && ', 'elif ', 1),
            'PATH export removed': t.replace(imagebuild._GUARD_PATH_LINE + '\n', '', 1),
            'extra ungated launch': t.replace(tail, '/usr/data/mq_ui &\n' + tail, 1),
            'exec launch': t.replace(tail, 'exec /usr/data/mq_ui\n' + tail, 1),
            'one-line if launch': t.replace(tail, 'if true; then /usr/data/mq_ui &\nfi\n' + tail, 1),
            'gate made vacuous with || true': t.replace('[ -f /usr/data/mq_player ]; then',
                                                        '[ -f /usr/data/mq_player ] || true; then', 1),
            'launch through $PROCESS_MQ_UI': t.replace(tail, '$PROCESS_MQ_UI &\n' + tail, 1),
            'gate words hidden in a comment': t.replace(tail, 'if false; then :; fi # ' + sel +
                                                        ' [ -f /tmp/.diskos_ready ]\n/usr/data/mq_ui &\n' + tail, 1),
            'single CR folds the gated if into a comment': t.replace('completing.\nif ' + sel,
                                                                     'completing.\rif ' + sel, 1),
        }
        for name, v in variants.items():
            with self.subTest(variant=name):
                data = v if isinstance(v, bytes) else v.encode()
                self.assertNotEqual(data, out, 'mutation did not apply - fixture drifted')
                with self.assertRaises(imagebuild.BuildError):
                    imagebuild._check_boot_script_output(data, out)
        with self.subTest(variant='invalid UTF-8'):
            with self.assertRaises(imagebuild.BuildError):
                imagebuild._check_boot_script_output(out + b'\xff\xfe', out)

    def test_cr_and_invalid_utf8_are_caught_by_the_sanity_check_alone(self):
        """Independent of the exact comparison: these must fail even if the expected bytes were wrong."""
        out = self.patched()
        for bad in (out.replace(b'completing.\nif ', b'completing.\rif ', 1), out + b'\xff'):
            with self.assertRaises(imagebuild.BuildError):
                imagebuild._validate_boot_hook(bad)


class DecisionFileCheckTests(unittest.TestCase):
    """_check_decision_file: the per-file output check for S96/S97/diskos-selected/diskos-bootprobe."""

    def test_only_an_exact_regular_0755_copy_passes(self):
        import stat
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            ref = t / 'payload'
            ref.write_bytes(b'#!/bin/sh\necho ok\n')
            good = t / 'good'
            good.write_bytes(ref.read_bytes())
            good.chmod(0o755)
            imagebuild._check_decision_file(str(good), str(ref), 'good')
            bad = {}
            for name, mode in (('setuid 4755', 0o4755), ('setgid 2755', 0o2755), ('sticky 1755', 0o1755),
                               ('0644', 0o644), ('0775', 0o775)):
                p = t / name.replace(' ', '_')
                p.write_bytes(ref.read_bytes())
                os.chmod(p, mode)
                self.assertEqual(stat.S_IMODE(os.lstat(p).st_mode), mode, f'could not set {name} here')
                bad[name] = p
            link = t / 'symlink'
            link.symlink_to(good)
            bad['symlink to a good copy'] = link
            differ = t / 'differ'
            differ.write_bytes(b'#!/bin/sh\necho no\n')
            differ.chmod(0o755)
            bad['different bytes'] = differ
            for name, p in bad.items():
                with self.subTest(case=name):
                    with self.assertRaises(imagebuild.BuildError):
                        imagebuild._check_decision_file(str(p), str(ref), name)

    def test_all_boot_decision_files_are_listed_and_shipped(self):
        names = {n for _, n in imagebuild._BOOT_DECISION_FILES}
        self.assertEqual(names, {'S96diskos_select', 'S97diskos_install', 'diskos-selected', 'diskos-bootprobe'})
        for n in names:
            p = Path(__file__).resolve().parents[1] / 'payload' / n
            self.assertTrue(p.is_file(), n)
            self.assertTrue(os.access(p, os.X_OK), n)



def _userns_ok():
    try:
        return subprocess.run(['unshare', '-Urm', 'true'], capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


@unittest.skipUnless(_STOCK_FIIO_INIT.is_file(), 'needs a pristine stock fiio_init.sh (not shipped in this repo)')
@unittest.skipUnless(_userns_ok(), 'needs unprivileged user+mount namespaces (unshare -Urm)')
class BootHookExecutionTests(unittest.TestCase):
    """EXECUTES the launch block of the real patched boot script and records which UI/player it starts.

    Runs in a private user+mount namespace with a fresh tmpfs on /tmp and on /opt, so the block's real
    absolute paths are used: /tmp/.diskos_boot_select, /tmp/.diskos_ready and the real shipped helper at
    /opt/diskos/bin/diskos-selected. Deliberate differences from the device, stated rather than hidden:
    /usr/data is rewritten to a temp directory (mounting over /usr would hide the host shell), and it runs
    under the HOST BusyBox ash (`busybox sh`), not the firmware's - the host dash rejects the stock
    script's `[ a == b ]`, which could make the coredump comparison fail for the wrong reason; qemu is not
    used because -L would redirect /tmp to the firmware tree. Launches are witnesses: the /usr/data binaries
    log DISKOS_*, the bare names log STOCK_*. This is the launch block only, not the watchdog loop."""

    def setUp(self):
        if not shutil.which('busybox'):
            self.fail('host busybox required for firmware-compatible [ == ] semantics')

    def launch_block(self):
        with tempfile.TemporaryDirectory() as t:
            f = Path(t) / 'fiio_init.sh'
            shutil.copy(_STOCK_FIIO_INIT, f)
            imagebuild._patch_fiio_init(str(f), None)
            text = f.read_text()
        a = text.index(imagebuild._GUARD_PATH_LINE)
        b = text.index('\nfi\n', a) + len('\nfi\n')
        return text[a:b]

    def boot(self, record, ready=True, helper=True, coredump='0', data_files=('mq_ui', 'mq_player'), block=None):
        with tempfile.TemporaryDirectory(prefix='diskos-hookexec-', dir='/var/tmp') as t:   # /tmp is replaced inside the namespace
            t = Path(t)
            data = t / 'usrdata'
            data.mkdir()
            log = t / 'launches'
            stockbin = t / 'stockbin'
            stockbin.mkdir()
            for name in ('mq_ui', 'mq_player'):
                w = stockbin / name
                w.write_text(f'#!/bin/sh\necho STOCK_{name} >> {log}\n')
                w.chmod(0o755)
                if name in data_files:
                    d = data / name
                    d.write_text(f'#!/bin/sh\necho DISKOS_{name} >> {log}\n')
                    d.chmod(0o755)
            block = (block or self.launch_block()).replace('/usr/data/', str(data) + '/')
            self.assertNotIn('/usr/data/', block)
            (t / 'block.sh').write_text('sleep() { :; }\nCOREDUMP_FLAG=' + coredump + '\n' + block + 'wait\n')
            setup = ['mount -t tmpfs none /tmp', 'mount -t tmpfs none /opt']
            if helper:
                helper_src = Path(__file__).resolve().parents[1] / 'payload' / 'diskos-selected'
                setup += ['mkdir -p /opt/diskos/bin', f'cp {helper_src} /opt/diskos/bin/diskos-selected',
                          'chmod 755 /opt/diskos/bin/diskos-selected']
            if record == 'FIFO':
                setup.append('mkfifo /tmp/.diskos_boot_select')
            elif record is not None:
                setup.append(f"printf '%s' '{record}' > /tmp/.diskos_boot_select")
            if ready:
                setup.append(': > /tmp/.diskos_ready')
            inner = ' && '.join(setup) + f' && PATH={stockbin}:$PATH busybox sh {t / "block.sh"}'
            r = subprocess.run(['unshare', '-Urm', 'sh', '-c', inner], capture_output=True, text=True, timeout=20)
            self.assertEqual(r.returncode, 0, r.stderr)
            return log.read_text().split() if log.exists() else []

    def test_only_a_positive_record_with_readiness_launches_diskos(self):
        diskos = ['DISKOS_mq_ui', 'DISKOS_mq_player']
        stock = ['STOCK_mq_ui', 'STOCK_mq_player']
        cases = [
            ('diskos + ready', dict(record='diskos\n'), diskos),
            ('diskos, not ready', dict(record='diskos\n', ready=False), stock),
            ('stock + ready', dict(record='stock\n'), stock),
            ('no record + ready', dict(record=None), stock),
            ('record without newline', dict(record='diskos'), stock),
            ('FIFO record (must not block)', dict(record='FIFO'), stock),
            ('helper missing', dict(record='diskos\n', helper=False), stock),
            ('coredump flag, record stock', dict(record='stock\n', coredump='1'), stock),
            ('coredump flag, not ready', dict(record='diskos\n', ready=False, coredump='1'), stock),
            ('coredump flag, diskos + ready', dict(record='diskos\n', coredump='1'), diskos),
            # mq_player absent from /usr/data makes the PRIMARY condition false, so these reach the elif:
            ('COREDUMP BRANCH taken: diskos + ready', dict(record='diskos\n', coredump='1', data_files=('mq_ui',)),
             ['DISKOS_mq_ui']),
            ('coredump branch: record stock', dict(record='stock\n', coredump='1', data_files=('mq_ui',)), stock),
            ('coredump branch: not ready', dict(record='diskos\n', ready=False, coredump='1', data_files=('mq_ui',)),
             stock),
            ('coredump branch: flag 0 falls to stock', dict(record='diskos\n', coredump='0', data_files=('mq_ui',)),
             stock),
        ]
        for name, kw, want in cases:
            with self.subTest(case=name):
                self.assertEqual(sorted(self.boot(**kw)), sorted(want))

    def test_coredump_branch_gate_is_what_keeps_stock(self):
        """Negative control: with the coredump branch's selection/readiness removed, the stock and not-ready
        coredump cases above launch diskOS - so those cases really exercise that branch's gate."""
        blk = self.launch_block()
        old = 'elif ' + imagebuild._SELECT_LINE + ' && [ -f /tmp/.diskos_ready ] && '
        self.assertEqual(blk.count(old), 1)
        ungated = blk.replace(old, 'elif ')
        for kw in (dict(record='stock\n'), dict(record='diskos\n', ready=False)):
            with self.subTest(**kw):
                got = self.boot(coredump='1', data_files=('mq_ui',), block=ungated, **kw)
                self.assertIn('DISKOS_mq_ui', got)

if __name__ == '__main__':
    unittest.main()
