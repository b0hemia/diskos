"""Locate the bundled native binaries and data files.

Layout (in the source tree AND inside the PyInstaller bundle):
    <root>/vendor/<os>-<arch>/   usbboot, mksquashfs, unsquashfs,
                                 my_write5_dram.bin, disc_spl_lpddr3.bin,
                                 lib/ (bundled .so/.dylib for usbboot)
    <root>/payload/              mq_ui, S97diskos_install, S99usbserial,
                                 diskos_manifest templates, etc.

When frozen by PyInstaller, data is unpacked under sys._MEIPASS; in the source
tree it sits next to this package. `resource_root()` resolves both.

A source checkout does NOT include the large native tools (they are .gitignored);
only the small device .bin blobs are tracked. For the STANDARD squashfs tools
(mksquashfs/unsquashfs, which are ordinary squashfs-tools binaries), when no
vendored copy is present we fall back to a system copy on PATH so a plain checkout
works without building. usbboot (a custom Ingenic mask-ROM uploader with no system
equivalent) and the device .bin blobs never fall back."""

import os
import shutil
import stat
import sys

from . import platform_probe

# Standard host tools that also ship as an ordinary system package (squashfs-tools).
# ONLY these may fall back to a PATH copy when the vendored binary is absent.
_SYSTEM_FALLBACK = frozenset({"mksquashfs", "unsquashfs"})


def resource_root():
    """Directory that contains vendor/ and payload/.

    - Frozen (PyInstaller): sys._MEIPASS.
    - Source tree: the installer/ dir (parent of this package)."""
    if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
        return sys._MEIPASS
    # this file is installer/diskos_installer/bundle.py -> installer/
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def vendor_dir():
    return os.path.join(resource_root(), "vendor", platform_probe.host_tag())


def payload_dir():
    return os.path.join(resource_root(), "payload")


def is_vendored(path):
    """True if `path` lives inside this host's vendor/ dir (a bundled tool), as
    opposed to a system copy resolved from PATH."""
    if not path:
        return False
    try:
        vd = os.path.abspath(vendor_dir())
        return os.path.commonpath([os.path.abspath(path), vd]) == vd
    except ValueError:
        return False   # different drive/root (Windows) -> not vendored


def native_build_script(kind="squashfs"):
    """Host-appropriate script that (re)builds the native tools, for error guidance."""
    o, _ = platform_probe.host()
    if o == "macos":
        return "build/build-macos.sh"
    return "build/build-usbboot-static.sh" if kind == "usbboot" else "build/build-squashfs-static.sh"


def _ensure_exec(path):
    try:
        st = os.stat(path)
        os.chmod(path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass
    return path


def _which_abs(name):
    """Absolute path to `name` on PATH, ignoring empty and relative PATH entries
    (a relative dir on PATH would be an injection risk for a flash-critical tool).
    Returns None unless it resolves to an absolute, executable regular file."""
    raw = os.environ.get("PATH", os.defpath)
    safe = os.pathsep.join(d for d in raw.split(os.pathsep) if d and os.path.isabs(d))
    p = shutil.which(name, path=safe)
    if not p:
        return None
    p = os.path.abspath(p)
    return p if os.path.isfile(p) and os.access(p, os.X_OK) else None


def native(name, required=True):
    """Absolute path to a native tool for this host.

    A vendored copy under vendor/<tag>/ always wins. For the standard squashfs tools
    ONLY, when no vendored copy is present (a source checkout) we fall back to a system
    copy on PATH so the installer works without the large bundled binaries.

    A vendored copy that EXISTS but is broken (bad perms, wrong arch, missing libs) is
    never masked by the fallback: it is returned as-is and fails its later execution /
    capability check loudly. Only a genuinely ABSENT vendored entry triggers the fallback
    - a dangling symlink counts as a broken install (lexists), not as absent.

    Raises PreflightError (E102) if required and unresolved. System copies are returned
    as an absolute path and are NOT chmod'd (we never modify a system binary)."""
    p = os.path.join(vendor_dir(), name)
    try:
        os.lstat(p)                          # present (even as a dangling/broken symlink) -> vendored
        return _ensure_exec(p)               # dangling symlink still returned -> fails loudly downstream
    except FileNotFoundError:
        pass                                 # genuinely absent -> may fall back below
    except OSError:
        # the vendored path exists but is inaccessible (e.g. an unsearchable vendor dir): do NOT
        # mask a broken install with a system copy - return it so it fails loudly downstream.
        return _ensure_exec(p)
    if name in _SYSTEM_FALLBACK:
        sysp = _which_abs(name)
        if sysp:
            return sysp                       # system tool: absolute, reused as-is, not chmod'd
    if not required:
        return None
    from .errors import PreflightError
    tag = platform_probe.host_tag()
    if name in _SYSTEM_FALLBACK:
        raise PreflightError(
            f"tool '{name}' not found in vendor/{tag} or PATH", code="E102",
            action="install squashfs-tools with LZO support (Debian/Ubuntu: "
                   "sudo apt install squashfs-tools), or build the bundled tools with "
                   f"{native_build_script('squashfs')} from the diskOS source")
    if name == "usbboot":
        raise PreflightError(
            f"bundled tool 'usbboot' not found for {tag}", code="E102",
            action=f"build the custom Ingenic uploader with {native_build_script('usbboot')} "
                   "from the diskOS source (it has no system-package equivalent)")
    raise PreflightError(
        f"bundled device file '{name}' not found for {tag}", code="E102",
        action="restore this file from the matching diskOS source or release; "
               "see build/README-vendor.md")


def data(name, required=True):
    """Absolute path to a bundled data/payload file."""
    p = os.path.join(payload_dir(), name)
    if os.path.exists(p):
        return p
    if required:
        from .errors import PreflightError
        raise PreflightError(f"bundled payload '{name}' not found", code="E102",
                             action="the install is incomplete - re-download the installer")
    return None


def native_env(tool_path=None):
    """Environment for running a native tool.

    For a BUNDLED tool, put its private vendor/<tag>/lib on the loader path so a bundled
    libusb is found without touching the system. For a SYSTEM tool resolved from PATH,
    return a clean environment: never point a system binary at our vendored libs, and undo
    any PyInstaller loader override (restore *_ORIG) so it loads its own system libraries.

    `tool_path` is the resolved binary path; None means "bundled" (back-compat)."""
    env = dict(os.environ)
    o, _ = platform_probe.host()
    var = "DYLD_LIBRARY_PATH" if o == "macos" else "LD_LIBRARY_PATH"
    if tool_path is None or is_vendored(tool_path):
        libdir = os.path.join(vendor_dir(), "lib")
        if os.path.isdir(libdir):
            env[var] = libdir + (os.pathsep + env[var] if env.get(var) else "")
        return env
    # System tool: strip our / PyInstaller loader overrides so it uses its own libs.
    for v in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        orig = env.pop(v + "_ORIG", None)    # PyInstaller stashes the pre-launch value here
        if orig is not None:
            env[v] = orig
        elif getattr(sys, "frozen", False):
            env.pop(v, None)                 # frozen w/o _ORIG -> don't leak _MEIPASS to a system tool
    return env
