# PyInstaller spec - one self-contained diskos-installer executable.
# Bundles the native flashing tools (vendor/<os>-<arch>/) and the payload
# (mq_ui + on-device init scripts) as data, plus pyusb. The result needs no
# system Python and no system packages: "remove the tool" = delete this file.
import os

ROOT = os.path.abspath(os.path.join(os.getcwd()))   # run from installer/
# host tag for the vendor dir to bundle (build on each target OS)
import platform
_sys = platform.system().lower()
_os = {"darwin": "macos", "linux": "linux"}.get(_sys, _sys)
_arch = {"x86_64": "x86_64", "amd64": "x86_64", "arm64": "arm64",
         "aarch64": "arm64"}.get(platform.machine().lower(), platform.machine().lower())
TAG = f"{_os}-{_arch}"

# files under vendor/ that must NOT be bundled into the app (dev-only backups, notes)
def _skip_vendor(fn):
    return fn.endswith(".bak") or fn.endswith(".dynamic.bak") or fn in ("README.md",)

datas = []
vend = os.path.join(ROOT, "vendor", TAG)
if os.path.isdir(vend):
    for fn in os.listdir(vend):
        if _skip_vendor(fn) or os.path.isdir(os.path.join(vend, fn)):
            continue
        datas.append((os.path.join(vend, fn), f"vendor/{TAG}"))
    libdir = os.path.join(vend, "lib")
    if os.path.isdir(libdir):
        for fn in os.listdir(libdir):
            datas.append((os.path.join(libdir, fn), f"vendor/{TAG}/lib"))
payload = os.path.join(ROOT, "payload")
if os.path.isdir(payload):
    for fn in os.listdir(payload):
        datas.append((os.path.join(payload, fn), "payload"))

# FAIL the build (don't ship an unusable artifact) if any required native tool or
# payload for this host is missing.
_required_native = ["usbboot", "mksquashfs", "unsquashfs",
                    "my_write5_dram.bin", "disc_spl_lpddr3.bin"]
_required_payload = ["mq_ui", "S96diskos_select", "S97diskos_install", "S99usbserial", "diskos-debug.sh", "dropbearmulti", "diskos-rmguard", "diskos-selected", "diskos-bootprobe"]
_missing = [n for n in _required_native if not os.path.exists(os.path.join(vend, n))]
_missing += [f"payload/{n}" for n in _required_payload if not os.path.exists(os.path.join(payload, n))]
if _missing:
    raise SystemExit(f"REFUSING to build: missing bundled files for {TAG}: {_missing}. "
                     f"Populate vendor/{TAG}/ and payload/ first.")

# pycryptodome loads C submodules dynamically; collect them all so the frozen
# app can AES-decrypt the FiiO OTA chunks.
from PyInstaller.utils.hooks import collect_submodules
hidden = ["usb", "usb.core", "usb.backend.libusb1"] + collect_submodules("Crypto")

a = Analysis(
    [os.path.join(ROOT, "diskos_installer", "__main__.py")],
    pathex=[ROOT],
    binaries=[],
    datas=datas,
    hiddenimports=hidden,
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)
pyz = PYZ(a.pure)
# onefile: pass binaries + datas straight into EXE with no COLLECT step.
exe = EXE(
    pyz, a.scripts, a.binaries, a.datas, [],
    name="diskos-installer",
    debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
    console=True,
)
