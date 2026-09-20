"""diskOS installer CLI.

Commands:
  doctor         Report host, bundled tools, device presence, and saved state.
  install        Build a diskOS image from YOUR stock firmware and flash it.
  restore-stock  Deactivate diskOS: reflash your saved stock-rootfs image (leaves /usr/data files).
  remove         Delete everything this tool created (its full uninstall footprint).

Design: the whole tool is Python; the bricking-sensitive steps delegate to bundled
proven native binaries. Nothing is installed system-wide on Linux/macOS.
"""

import argparse
import fcntl
import os
import sys
import tempfile

from diskos_installer import (__version__, bundle, errors, flasher, imagebuild,
                              platform_probe, service, state, ui)
from diskos_installer.reporter import CLIReporter


# --- single-run lock so two installers can't touch the ONE physical device at once. It must be
# SHARED across all users on the host (a GUI as your user and a CLI under sudo must not both flash),
# so it lives at a FIXED /tmp path - NOT per-user. Advisory flock (auto-released on death -> no
# stale-lock hazard). Opened READ-ONLY so any user can take the flock on a 0644 file, and O_NOFOLLOW
# so a planted symlink can't redirect the open (we bail instead of following it). ---
class _Lock:
    def __init__(self):
        # A FIXED absolute path, not tempfile.gettempdir(): $TMPDIR differs between a normal run and a
        # sudo run (and per-user on macOS), which would hand them SEPARATE locks and defeat the whole
        # point. /tmp is world-accessible on every supported host (Linux/macOS); Windows is unsupported.
        self.path = "/tmp/diskos-installer.lock"
        self.fd = None

    def __enter__(self):
        import stat as _stat
        flags = os.O_CREAT | os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
        try:
            self.fd = os.open(self.path, flags, 0o644)
        except OSError as e:
            raise SystemExit(ui.red(
                f"could not open the run-lock at {self.path} ({e}). If it exists as a symlink, "
                "remove it and retry."))
        # A planted symlink is refused by O_NOFOLLOW above; also refuse a non-regular file.
        if not _stat.S_ISREG(os.fstat(self.fd).st_mode):
            os.close(self.fd); self.fd = None
            raise SystemExit(ui.red(f"run-lock {self.path} is not a regular file - refusing."))
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(self.fd)
            self.fd = None
            raise SystemExit(ui.red(
                "another diskOS installer is already running. Close it and retry "
                "(only one may touch the device at a time)."))
        return self

    def __exit__(self, *exc):
        try:
            if self.fd is not None:
                fcntl.flock(self.fd, fcntl.LOCK_UN)
                os.close(self.fd)
        except OSError:
            pass
        return False


def _set_phase(st, phase):
    st["phase"] = phase
    state.save(st)


def cmd_doctor(args):
    ui.step("diskOS installer - doctor")
    o, a = platform_probe.host()
    ui.info(f"version    : {__version__}")
    ui.info(f"host       : {o}-{a} ({'supported' if platform_probe.is_supported() else 'UNSUPPORTED'})")
    ui.info(f"state dir  : {state.state_dir()}")

    def _tool_ok(path, need_exec):
        try:
            return (os.path.isfile(path) and os.access(path, os.R_OK)
                    and (not need_exec or os.access(path, os.X_OK)))
        except OSError:
            return False

    ui.info("native tools and device files:")
    all_ok = True
    for name in ("usbboot", "mksquashfs", "unsquashfs", "my_write5_dram.bin", "disc_spl_lpddr3.bin"):
        p = bundle.native(name, required=False)
        need_exec = not name.endswith(".bin")
        ok = bool(p) and _tool_ok(p, need_exec)
        if ok:
            ui.ok(f"  {name}: {p} ({'bundled' if bundle.is_vendored(p) else 'system'})")
        elif p:
            ui.err(f"  {name}: BROKEN ({p}) - not a readable{'/executable' if need_exec else ''} file")
        else:
            ui.err(f"  {name}: MISSING")
        all_ok = all_ok and ok
    # squashfs tools may be bundled OR system copies - prove the actual pair can pack+extract LZO.
    mksq = bundle.native("mksquashfs", required=False)
    unsq = bundle.native("unsquashfs", required=False)
    if mksq and unsq:
        try:
            imagebuild.check_squashfs_tools(mksq, unsq)
            ui.ok("  squashfs LZO pack/extract check: OK")
        except Exception as e:
            ui.err(f"  squashfs LZO pack/extract check: FAILED - {e}")
            all_ok = False
    for name in ("mq_ui", "S97diskos_install", "S99usbserial", "diskos-debug.sh", "dropbearmulti"):
        p = bundle.data(name, required=False)
        (ui.ok if p else ui.err)(f"  {name}: {p or 'MISSING'}")
        all_ok = all_ok and bool(p)   # payload files are required to build a flashable image

    n = platform_probe.maskrom_count()
    if n < 0:
        ui.warn("device: cannot enumerate USB (pyusb/lsusb unavailable)")
    elif n == 0:
        ui.info("device: none in mask-ROM mode (normal unless you're about to flash)")
    else:
        (ui.ok if n == 1 else ui.warn)(f"device: {n} in mask-ROM mode")

    st = state.load()
    if st.get("installed"):
        ui.info(f"diskOS installed via this tool: yes (variant={st.get('variant')}, at {st.get('installed_at')})")
    else:
        ui.info("diskOS installed via this tool: no record")
    ui.info(f"saved bone-stock image (for restore): {'yes' if state.have_stock_image() else 'no'}")
    return 0 if all_ok else 1


def _cli_confirm(yes):
    """Build a service `confirm` callback for the CLI (prints the summary, then
    y/N - or auto-yes with --yes)."""
    def confirm(summary):
        ui.step("Ready - please confirm")
        for k in ("action", "variant", "image", "duration", "consequence"):
            if summary.get(k):
                ui.info(f"  {k}: {summary[k]}")
        if yes:
            return True
        return ui.confirm("Proceed?", default=False)
    return confirm


def cmd_install(args):
    if not platform_probe.is_supported():
        ui.err(f"[E101] unsupported host {platform_probe.host_tag()} - Linux/macOS only for now")
        return 2
    if not args.firmware and not args.stock:
        ui.err("[E140] need --firmware <FiiO official update .zip> (or --stock <rootfs.squashfs>).")
        return 2
    params = {"firmware": args.firmware, "stock": args.stock,
              "ui_binary": args.ui, "variant": args.variant}
    r = service.do_install(params, CLIReporter(), _cli_confirm(args.yes))
    if r.get("ok"):
        ui.info("Reboot the device and diskOS installs itself automatically on first boot.")
        ui.info("To deactivate diskOS later (reflash your saved stock rootfs):  diskos-installer restore-stock")
        return 0
    return 3 if r.get("aborted") else 1


def cmd_restore_stock(args):
    r = service.do_restore({"firmware": args.firmware}, CLIReporter(), _cli_confirm(args.yes))
    return 0 if r.get("ok") else (3 if r.get("aborted") else 1)


def cmd_remove(args):
    def confirm(summary):
        ui.warn(summary.get("consequence", ""))
        return args.yes or ui.confirm("Delete the installer's files anyway?", default=False)
    r = service.do_remove({"force": args.force}, CLIReporter(), confirm)
    if r.get("ok"):
        if getattr(sys, "frozen", False):
            ui.info(f"To finish: delete this executable ({sys.executable}).")
        else:
            ui.info(f"To finish: delete the installer folder ({bundle.resource_root()}).")
        ui.info("Nothing was installed system-wide, so there is nothing else to clean up.")
        return 0
    if r.get("errors"):
        return 1   # partial-removal errors already reported by the service
    ui.info("aborted; nothing removed.")
    return 3


def build_parser():
    p = argparse.ArgumentParser(prog="diskos-installer",
                                description="Standalone diskOS installer for the FiiO Snowsky Disc.")
    p.add_argument("--version", action="version", version=f"diskos-installer {__version__}")
    # no subcommand -> GUI (running ./diskos-installer with no arguments); subcommands = CLI
    sub = p.add_subparsers(dest="cmd", required=False)

    sub.add_parser("gui", help="launch the graphical installer (also the default with no arguments)")

    d = sub.add_parser("doctor", help="report host, bundled tools, device, and state")
    d.set_defaults(func=cmd_doctor)

    i = sub.add_parser("install", help="build from YOUR stock firmware and flash diskOS")
    i.add_argument("--firmware", help="FiiO official update .zip (your stock firmware)")
    i.add_argument("--stock", help="pre-extracted stock rootfs.squashfs (instead of --firmware)")
    i.add_argument("--ui", help="diskOS UI binary (default: bundled mq_ui)")
    i.add_argument("--variant", choices=["public", "dev"], default="public",
                   help="public (no always-on shell; Debug Mode enables SSH on demand) or dev (adds an ALWAYS-ON PASSWORDLESS ROOT SHELL over "
                        "USB - anyone with physical access gets root every boot; dev devices only)")
    i.add_argument("-y", "--yes", action="store_true", help="don't prompt before flashing")
    i.set_defaults(func=cmd_install)

    r = sub.add_parser("restore-stock", help="deactivate diskOS (reflash your saved stock rootfs)")
    r.add_argument("--firmware", help="FiiO update .zip (only needed if no stock image is saved)")
    r.add_argument("-y", "--yes", action="store_true", help="don't prompt before flashing")
    r.set_defaults(func=cmd_restore_stock)

    rm = sub.add_parser("remove", help="delete the installer and everything it created")
    rm.add_argument("-y", "--yes", action="store_true", help="don't prompt")
    rm.add_argument("--force", action="store_true", help="remove even if diskOS is still on the device")
    rm.set_defaults(func=cmd_remove)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    # no subcommand, or 'gui' -> launch the graphical installer (under the lock)
    if getattr(args, "cmd", None) in (None, "gui"):
        from diskos_installer import gui
        with _Lock():
            return gui.main()
    try:
        if args.cmd == "doctor":            # doctor is read-only; no lock needed
            return args.func(args)
        with _Lock():
            return args.func(args)
    except errors.DiskOSError as e:   # any coded installer error (Build/Flash/Preflight)
        ui.err(str(e))
        return 1
    except KeyboardInterrupt:
        ui.err("interrupted.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
