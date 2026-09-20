"""Mask-ROM USB flashing - Python wrapper around the PROVEN native binaries
(usbboot + the my_write5 DRAM NAND writer + the X2000 SPL). We do NOT reimplement
the flashing protocol; we orchestrate it, watch it, and interpret the result.

Faithful port of flash_diskos.sh, with the python helper snippets (poison blob,
debug-struct parse) folded in natively, plus honest progress and fail-closed
result interpretation.
"""

import os
import shutil
import signal
import struct
import subprocess
import tempfile

from . import bundle, platform_probe
from .reporter import CLIReporter
from .errors import FlashError, DEVICE_RESULT_CODES, RECOVERABLE

IMG_SIZE = 100663296
SQUASH_MAGIC = b"hsqs"

# usbboot waits FLASH_WAIT seconds for the writer, then reads the DONE marker back. The writer
# finishes well inside this on real hardware (validated repeatedly). A too-short wait shows as a
# not-DONE readback (retry with a longer wait) - it never corrupts the flash. A device with an
# unusually high bad-block count can raise it via DISKOS_FLASH_WAIT.
FLASH_WAIT = os.environ.get("DISKOS_FLASH_WAIT", "900")   # 15 min (was 5400/90 min)
_FLASH_WAIT_SECS = int(FLASH_WAIT) if FLASH_WAIT.isdigit() else 900

RESULT_NAMES = {code: name for code, (_fcode, name) in DEVICE_RESULT_CODES.items()}

FLASH_EXPECT_SECS = 15 * 60   # ~15 min expected flash duration
# Hard ceiling for the whole usbboot invocation: the writer --wait plus margin for the image
# download + result readback; then treat a still-running usbboot as a hung/reset device and
# terminate it (E303) rather than blocking forever.
FLASH_HARD_TIMEOUT_SECS = _FLASH_WAIT_SECS + 15 * 60   # writer wait + 15 min margin


def _probe_helpers():
    """Confirm every bundled native tool can actually execute (and load its libs)
    BEFORE the destructive gate. Catches a noexec temp mount / missing dylibs while
    the device is still untouched. Non-zero exit is fine; an OSError is not."""
    probes = [(bundle.native("usbboot"), ["--help"]),
              (bundle.native("mksquashfs"), ["-version"]),
              (bundle.native("unsquashfs"), ["-version"])]
    for path, args in probes:
        try:
            subprocess.run([path] + args, env=bundle.native_env(),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=15)
        except OSError as e:
            raise FlashError(
                f"bundled helper '{os.path.basename(path)}' cannot execute ({e})",
                code="E103",
                action="the tool's directory may be on a noexec mount, or a required library is "
                       "missing; move the installer to an exec-capable filesystem and retry")
        except subprocess.TimeoutExpired:
            pass   # it started (that's all we needed to prove)


def preflight(image_path, rep=None):
    """Validate the image and that exactly one device is in mask-ROM. Fail closed."""
    rep = rep or CLIReporter()
    if not os.path.exists(image_path):
        raise FlashError(f"image not found: {image_path}", code="E120")
    sz = os.path.getsize(image_path)
    if sz != IMG_SIZE:
        raise FlashError(f"wrong image size {sz} (must be {IMG_SIZE})", code="E121",
                         action="use a diskOS image built by this installer")
    with open(image_path, "rb") as f:
        if f.read(4) != SQUASH_MAGIC:
            raise FlashError("image is not a squashfs (bad magic) - not a diskOS/stock image", code="E122")

    _probe_helpers()   # every bundled tool must actually execute (catches noexec /tmp, missing libs)

    n = platform_probe.maskrom_count()
    if n == 0:
        raise FlashError(
            "no device in mask-ROM mode", code="E110",
            action="power the device OFF, hold Volume-Down, plug in USB (screen stays "
                   "black), then retry")
    if n > 1:
        raise FlashError(f"{n} devices in mask-ROM mode - need exactly 1", code="E111",
                         action="unplug the other Ingenic devices")
    if n < 0:
        # FAIL CLOSED: we could not prove exactly one device (no libusb backend or a
        # permission error). Never cross the destructive gate on an unproven count.
        raise FlashError(
            "cannot confirm exactly one device (USB enumeration failed - missing libusb "
            "backend or insufficient USB permissions)", code="E112",
            action="refusing to flash on an unproven device count; fix USB access and retry")


def _parse_debug(dbg_path):
    """Parse the 1KB little-endian debug readback (256 x uint32). Returns a dict.
    Mirrors the field offsets in flash_diskos.sh."""
    with open(dbg_path, "rb") as f:
        raw = f.read(1024)
    if len(raw) < 1024:
        raise FlashError(f"short debug readback ({len(raw)} bytes) - flash result UNKNOWN", code="E302",
                         action=RECOVERABLE)
    w = struct.unpack("<256I", raw)
    nbad = w[20]
    return {
        "magic": w[0],
        "start_block": w[5],
        "nlogblocks": w[6],   # writer's compiled logical-block capacity - MUST cover the whole image
        "done": w[9],
        "skipped": w[10],
        "result": w[16],
        "retried": w[17],
        "worst_retries": w[18],
        "bad_found": nbad,
        "bad_list": [w[40 + i] for i in range(min(nbad, 64))],
    }


def _writer_capacity(writer_path):
    """The NAND writer only programs its compiled NLOGBLOCKS logical blocks; a stale writer (the old
    580-block build) silently TRUNCATES the image and the device still reports success for the part it
    wrote. Read that capacity from the writer's `addiu $v0, $zero, NLOGBLOCKS` instruction (offset 0x40c)
    so a mismatch is refused BEFORE any NAND write - not only caught in the post-write readback (E311),
    by which point the device already holds a truncated, unbootable image. Returns the block count, or
    None if the writer's layout is unrecognised (caller fails closed)."""
    try:
        with open(writer_path, "rb") as f:
            f.seek(0x40c)
            instr = struct.unpack("<I", f.read(4))[0]
    except Exception:
        return None
    if (instr & 0xFFFF0000) != 0x24020000:   # not `addiu $v0,$zero,imm` -> writer build changed; don't trust the offset
        return None
    return instr & 0xFFFF


def flash(image_path, log_path=None, rep=None):
    """Flash `image_path` to the device via mask-ROM. Returns the parsed debug dict
    on SUCCESS; raises FlashError otherwise (fail-closed)."""
    rep = rep or CLIReporter()
    preflight(image_path, rep)
    image_path = os.path.abspath(image_path)

    usbboot = bundle.native("usbboot")
    writer = bundle.native("my_write5_dram.bin")
    spl = bundle.native("disc_spl_lpddr3.bin")

    # PRE-WRITE capacity gate: refuse a truncating writer before it touches the NAND (the post-write
    # E311 check is too late - the image is already partially programmed). This is the 580-vs-768 bug.
    need_blocks = IMG_SIZE // (128 * 1024)
    wcap = _writer_capacity(writer)
    if wcap is None:
        raise FlashError(
            "unrecognised NAND writer build - cannot verify its block capacity before flashing; "
            "refusing to risk a truncated image", code="E123",
            action="use a diskOS installer bundle built by this project")
    if wcap < need_blocks:
        raise FlashError(
            f"NAND writer covers only {wcap} x 128 KB blocks but the image needs {need_blocks} - "
            f"flashing WOULD TRUNCATE the image (nothing has been written). This is the 580-vs-768 "
            f"writer bug.", code="E124",
            action=f"reconcile my_write5 to NLOGBLOCKS>={need_blocks} and rebuild the bundle")
    rep.log(f"writer capacity (pre-write): {wcap} blocks (image needs {need_blocks})")

    tmp = tempfile.mkdtemp(prefix="diskos-flash-")
    poison = os.path.join(tmp, "poison.bin")
    dbg = os.path.join(tmp, "dbg.bin")
    with open(poison, "wb") as f:
        f.write(b"\xee" * 128)

    cmd = [
        usbboot, "-v", "--cpu", "x2000", "--stage1", spl, "--wait", "2",
        "--addr", "0xa0c00000", "--download", writer,
        "--addr", "0xa1000000", "--download", image_path,
        "--addr", "0xa0a00000", "--download", poison,
        "--start1", "0xa0c00030", "--wait", FLASH_WAIT,
        "--addr", "0xa0a00000", "--length", "0x400", "--upload", dbg,
    ]

    rep.phase("Flashing diskOS (mask-ROM) - ~15 minutes", destructive=True)
    rep.warning("Do NOT disconnect the device or let the host sleep during the flash.")

    logf = open(log_path, "w") if log_path else open(os.path.join(tmp, "flash.log"), "w")
    rep.indeterminate(True, note="flashing (scan → erase → program → verify → retry)",
                      expect_secs=FLASH_EXPECT_SECS)
    try:
        try:
            with _sleep_inhibited(rep):
                # own session so a GUI/parent crash can't SIGPIPE the flasher; output goes
                # to a file (never a pipe) so a closed reader can't deadlock or kill it.
                proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT,
                                        env=bundle.native_env(), start_new_session=True)

                def _kill_flasher():
                    # The flasher runs in its OWN session (start_new_session) so a parent SIGPIPE
                    # can't kill it mid-write - but that also means it SURVIVES us. Kill the whole
                    # process group so NAND writes actually stop, then reap without blocking.
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                    except OSError:
                        proc.kill()
                    try:
                        proc.wait(timeout=30)   # reap; don't block if stuck in D-state
                    except subprocess.TimeoutExpired:
                        pass

                try:
                    # A3: bound the wait. If the device resets/re-enumerates mid-flash,
                    # usbboot can block on a dead handle forever - never hang the app.
                    rc = proc.wait(timeout=FLASH_HARD_TIMEOUT_SECS)
                except subprocess.TimeoutExpired:
                    _kill_flasher()
                    raise FlashError(
                        f"flash timed out - the device stopped responding after "
                        f"{FLASH_HARD_TIMEOUT_SECS // 60} min (it most likely reset "
                        "mid-flash)", code="E303", action=RECOVERABLE)
                except BaseException:
                    # Ctrl-C, SIGTERM, a GUI/parent crash, or any error during the wait: WITHOUT
                    # this the flasher keeps writing NAND after we return, and a tester who believes
                    # flashing stopped may unplug mid-write and brick the device. Kill it, then
                    # propagate the original interruption/error.
                    _kill_flasher()
                    raise
        finally:
            rep.indeterminate(False)
            logf.close()

        rep.log(f"usbboot exit={rc}")
        if not os.path.exists(dbg):
            raise FlashError(
                "no debug readback produced - flash result UNKNOWN; assume FAILED",
                code="E301", action=RECOVERABLE)

        d = _parse_debug(dbg)
        ok = (d["magic"] == 0x4004E005 and d["done"] == 0x55555555
              and d["result"] == 0x600DF10C)
        # SAFETY: the writer only programs its compiled NLOGBLOCKS (dbg[6]) logical blocks. If that is
        # fewer than the image occupies, the TAIL is silently dropped - and squashfs keeps its
        # inode/directory/fragment tables at the tail, so a truncated image mounts-fails and won't
        # boot, WHILE the device still reports SUCCESS for the part it did write. Require the writer's
        # capacity to cover the whole image. (This is exactly the 580-vs-768 bug; this check catches it.)
        need_blocks = IMG_SIZE // (128 * 1024)
        cap_ok = d["nlogblocks"] == need_blocks
        rep.log(f"scan: bad-blocks-found={d['bad_found']} list={d['bad_list']}")
        rep.log(f"writer capacity: {d['nlogblocks']} blocks (image needs {need_blocks})")
        # B4: dbg[17]/[18] mean retried/worst only on SUCCESS; on the out-of-space /
        # block-write-fail aborts they hold the last phys/logical block; on the other
        # aborts they are 0 and must NOT be shown as "last block" (would be misleading).
        if ok:
            rep.log(f"write: skipped={d['skipped']} retried={d['retried']} "
                    f"worst={d['worst_retries']}")
        elif d["result"] in (0xDEAD0002, 0xDEAD0003):
            rep.log(f"write: skipped={d['skipped']} last-phys-block={d['retried']} "
                    f"last-logical-block={d['worst_retries']}")
        else:
            rep.log(f"write: skipped={d['skipped']}")
        fcode, name = DEVICE_RESULT_CODES.get(d["result"], ("F000", "UNKNOWN"))
        rep.log(f"result: 0x{d['result']:08X} [{fcode}] {name}")

        if not ok:
            raise FlashError(
                f"flash FAILED (device result [{fcode}] {name}, "
                f"magic=0x{d['magic']:08X} done=0x{d['done']:08X})",
                code="E310", action=RECOVERABLE)
        if not cap_ok:
            raise FlashError(
                f"writer/image size MISMATCH: the flashing tool programs {d['nlogblocks']} x 128 KB "
                f"blocks but this image needs {need_blocks}. The image was TRUNCATED - the device "
                f"reports success but will not boot. This is a build/tooling defect; do not ship or "
                f"trust this build.", code="E311", action=RECOVERABLE)
        rep.ok("flash verified OK")
        return d
    finally:
        # B3: never leak the per-flash tempdir (poison/dbg/flash.log), but keep the log
        # debuggable: if no external log_path was given, preserve the internal log to a
        # single stable file (overwritten each flash - bounded, not a growing leak).
        internal_log = os.path.join(tmp, "flash.log")
        if not log_path and os.path.exists(internal_log):
            try:
                # Unique, 0600, no symlink-follow: write THROUGH the mkstemp fd (never reopen the path,
                # which could follow a swapped-in symlink and truncate an arbitrary target). Path reported.
                fd, kept = tempfile.mkstemp(prefix="diskos-flash-", suffix=".log")
                with os.fdopen(fd, "wb") as out, open(internal_log, "rb") as src:
                    shutil.copyfileobj(src, out)
                rep.log(f"flash log saved to {kept}")
            except OSError:
                pass
        shutil.rmtree(tmp, ignore_errors=True)


class _sleep_inhibited:
    """Best-effort host sleep inhibitor for the duration of the flash. No-op if the
    platform tool isn't available; never blocks or fails the flash. If it CAN'T
    inhibit sleep, it warns (B5) - a suspend mid-flash would abort it."""

    def __init__(self, rep=None):
        self.rep = rep

    def __enter__(self):
        self.proc = None
        o, _ = platform_probe.host()
        try:
            if o == "macos":
                self.proc = subprocess.Popen(["caffeinate", "-dimsu"])
            elif o == "linux":
                import shutil
                if shutil.which("systemd-inhibit"):
                    # keep a long-lived inhibitor process alive; we kill it on exit
                    self.proc = subprocess.Popen(
                        ["systemd-inhibit", "--what=sleep:idle",
                         "--why=diskOS flash in progress", "sleep", "infinity"])
        except Exception:
            self.proc = None
        if self.proc is None and self.rep is not None:
            self.rep.warning(
                "could not auto-inhibit system sleep on this host - make sure your "
                "computer will NOT sleep/suspend for the next ~15 minutes (a suspend "
                "mid-flash aborts it; the device stays recoverable).")
        return self

    def __exit__(self, *exc):
        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=5)   # reap so no zombie / stray 'sleep infinity' lingers
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
        return False
