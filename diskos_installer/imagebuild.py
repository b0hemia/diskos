"""Build a flashable diskOS image from the user's OWN stock firmware.

Faithful Python port of the proven mkdiskos.sh / extract_stock_rootfs.sh logic:
  extract_stock_rootfs(zip) -> stock rootfs.squashfs  (byte-exact from FiiO's zip)
  build_image(stock, mq_ui, variant) -> diskos_<variant>.bin (76021760 bytes)

We NEVER ship FiiO's rootfs; the user supplies their official firmware zip and we
build locally. squashfs pack/unpack delegates to the bundled mksquashfs/unsquashfs
(reference tools) - we do not reimplement squashfs.
"""

import contextlib
import os
import re
import subprocess
import sys
import zipfile

from . import bundle
from .reporter import CLIReporter

IMG_SIZE = 100663296         # diskOS image size: 768 NAND blocks (~96 MiB) - fits v2.40's 88 MB rootfs and covers v2.09/v2.28. The NAND writer (my_write5) MUST be built for this many blocks (NLOGBLOCKS=768); the flasher's writer-capacity check (dbg[6]) enforces it so a mismatched writer can't silently truncate. Written to the start of the mtd2 rootfs partition (128 MB); the RO squashfs need not fill it.

# Known-good stock rootfs.squashfs, verified out-of-band (NOT trusting the in-zip OTA manifest,
# which an attacker could modify consistently). A tested firmware whose extracted rootfs does not
# match its pin is refused: this rejects a modified/tampered/corrupt rootfs before it is patched
# and flashed. Map: MAIN_OS_VER -> (sha256, size_bytes). Add a version's pin only after hashing an
# authentic copy of that firmware.
PINNED_ROOTFS = {
    "228": ("0ffd877bca2c69ddff9ca70f4494da0d9e580c18d0f587e2c6d9921f2db82bd2", 72957952),
    "209": ("f1e3c69fb0e88b923c135558e01f4387a661f68839c8118e8ad490bdc9fc74e6", 75919360),
    "240": ("b479e159db5134325819b5f6e5a54388f3adefae373a4ee60680f02d5dcf0bb8", 88420352),
    # V2.57 pinned but NOT yet in TESTED_FW: a build on it needs DISKOS_ALLOW_UNTESTED_FW=1 and stays
    # unqualified until on-device init/artwork/BT/gain/USB-DAC testing passes (fits the 768-block image).
    "257": ("111e4dd7ee3d7ff91ba7e61181690be7ffd22bd1bbd13ad513f5f015ffb302ae", 80596992),
}
# Firmware versions diskOS has been flash-tested against. Others have DIFFERENT command-tag
# meanings, so diskOS built on them can send wrong commands and misbehave/reboot.
# v2.40's 88 MB rootfs needs IMG_SIZE=96 MB + the 768-block writer (see IMG_SIZE note).
TESTED_FW = {"209", "228", "240"}
# Firmware diskOS may be INSTALLED on in this release: the three stock versions it supports. Only their exact
# pinned images are accepted (validate_stock_rootfs(..., allow_override=False) on install/build), and there is no
# override. V2.57 is pinned for research but NOT supported. TESTED_FW governs validation of saved stock images
# for restore.
INSTALL_FW = {"209", "228", "240"}


def require_installable(mver):
    """Refuse to build or install diskOS on any firmware outside INSTALL_FW. No override."""
    if mver not in INSTALL_FW:
        raise BuildError(
            f"firmware MAIN_OS_VER={mver or '?'} is not supported for installing diskOS (supported: "
            f"{', '.join('V' + v[0] + '.' + v[1:] for v in sorted(INSTALL_FW))}). Restoring stock is still supported.",
            code="E225")
SQUASH_MAGIC = b"hsqs"


def validate_stock_rootfs(stock_squashfs, rep=None, allow_override=True):
    """Validate that `stock_squashfs` is a genuine, supported, known-good Snowsky Disc rootfs -
    a SHARED gate called BEFORE the image is saved as the recovery copy, BEFORE build, and BEFORE
    every restore-flash (so a wrong-device or crafted rootfs can never be saved or flashed on the
    strength of a size/magic preflight alone). Raises BuildError (E220 not-a-Disc-rootfs / E221
    untested version / E224 hash mismatch). Returns the MAIN_OS_VER string. Only extracts the tiny
    version.in - cheap enough to run on every path. DISKOS_ALLOW_UNTESTED_FW=1 relaxes E221/E224."""
    rep = rep or CLIReporter()
    import hashlib, tempfile, shutil
    unsq = bundle.native("unsquashfs")
    with open(stock_squashfs, "rb") as f:
        if f.read(4) != SQUASH_MAGIC:
            raise BuildError("not a squashfs image (bad magic) - not a Snowsky Disc rootfs", code="E220")
    tmp = tempfile.mkdtemp(prefix="diskos-vchk-")
    try:
        r = _run([unsq, "-d", os.path.join(tmp, "x"), "-f", stock_squashfs,
                  "etc/product_version/version.in"], capture_output=True, text=True)
        ver_in = os.path.join(tmp, "x", "etc/product_version/version.in")
        if not os.path.exists(ver_in):
            # The file we need was NOT extracted (e.g. unsquashfs without LZO, or not a readable
            # rootfs) - report that plainly rather than mislabeling it as a wrong-PRODUCT rootfs
            # below. Keyed on the file's presence, not on returncode: unsquashfs can exit nonzero
            # (status 2) for a metadata-setting warning while still writing the file correctly.
            raise BuildError(
                f"could not extract version.in from the stock rootfs using '{unsq}': "
                f"{(r.stderr or r.stdout or '').strip()[:300]}", code="E230",
                action="check the firmware image and that unsquashfs supports LZO "
                       "(install squashfs-tools with LZO support, or use the bundled tools)")
        prod = _grep1(ver_in, r"PRODUCT=([A-Za-z0-9_]+)")
        mver = _grep1(ver_in, r"MAIN_OS_VER=([0-9]+)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    # allow_override=False (install/build of this release): the pin is absolute - the override may only ever
    # relax validation of a saved stock image being RESTORED, never what diskOS gets built on or installed onto.
    override = allow_override and os.environ.get("DISKOS_ALLOW_UNTESTED_FW") == "1"
    if prod != "SNOWSKY_DISC":
        raise BuildError(f"not a Snowsky Disc rootfs (PRODUCT={prod!r})", code="E220")
    if mver not in TESTED_FW and not override:
        raise BuildError(
            f"firmware MAIN_OS_VER={mver or '?'} is not tested (supported: {', '.join(sorted(TESTED_FW))}). "
            "Other versions can have incompatible command meanings."
            + (" Re-run with DISKOS_ALLOW_UNTESTED_FW=1 at your own risk." if allow_override else ""),
            code="E221")
    pin = PINNED_ROOTFS.get(mver)
    if pin:
        exp_sha, exp_sz = pin
        got_sz = os.path.getsize(stock_squashfs)
        # The pin is over the EXACT extracted rootfs (exp_sz bytes). But the SAVED recovery copy is
        # those same bytes zero-PADDED to the partition size (_save_stock pads to IMG_SIZE), so this
        # gate is called with both the unpadded (install/build) and padded (restore) forms. Validate
        # the first exp_sz bytes against the pin and require every byte AFTER to be zero padding - so a
        # padded copy verifies identically to the original, while arbitrary appended data still fails.
        got_sha, tail_zero = None, False
        if exp_sz <= got_sz <= IMG_SIZE:
            with open(stock_squashfs, "rb") as f:
                h = hashlib.sha256(); remaining = exp_sz
                while remaining > 0:
                    chunk = f.read(min(1 << 20, remaining))
                    if not chunk:
                        break
                    h.update(chunk); remaining -= len(chunk)
                got_sha = h.hexdigest() if remaining == 0 else None
                tail_zero = True                       # anything past exp_sz must be pure zero padding
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    if chunk.strip(b"\x00"):
                        tail_zero = False; break
        if got_sha != exp_sha or not tail_zero:
            if override:
                rep.warning(f"stock rootfs hash {(got_sha or '?')[:12]}... != pinned V{mver} - proceeding (override set)")
            else:
                raise BuildError(
                    f"stock rootfs does not match the known-good V{mver} image (got "
                    f"{(got_sha or 'short/oversize')[:12]}..., expected {exp_sha[:12]}...). The firmware "
                    "may be modified, corrupt, or repackaged - re-download the official FiiO firmware."
                    + (" (Set DISKOS_ALLOW_UNTESTED_FW=1 at your own risk.)" if allow_override else ""),
                    code="E224")
        else:
            rep.log(f"stock rootfs matches the pinned known-good V{mver} image (sha256 verified)")
    elif mver in TESTED_FW:
        rep.warning(f"no pinned hash for V{mver} yet - rootfs authenticity is NOT verified against a pin")
    rep.log(f"stock rootfs OK: PRODUCT={prod} MAIN_OS_VER={mver or '?'}")
    return mver


from .errors import BuildError  # coded (E2xx); re-exported so imagebuild.BuildError still resolves


# --- safe zip extraction (reject traversal / symlink escape / bombs) ---------
def _safe_extract_member(zf, member, dest_root):
    name = member.filename
    if name.startswith("/") or os.path.isabs(name) or ".." in name.replace("\\", "/").split("/"):
        raise BuildError(f"unsafe path in zip: {name!r}", code="E202")
    target = os.path.realpath(os.path.join(dest_root, name))
    if not (target == os.path.realpath(dest_root) or
            target.startswith(os.path.realpath(dest_root) + os.sep)):
        raise BuildError(f"zip entry escapes extraction dir: {name!r}", code="E202")
    return target


# FiiO chunks the rootfs and wraps each chunk (and the manifest) in AES-256-CBC
# (openssl -pbkdf2) under the fixed key "fo123" (their reused OTA string -
# obfuscation, not protection). We decrypt + concatenate in index order. We only
# READ the image; no signature is involved.
_OTA_KEY = "fo123"


def _openssl_aes_decrypt(data, password):
    """Replicate `openssl enc -d -aes-256-cbc -pbkdf2 -k <password>`:
    'Salted__' + 8-byte salt header, PBKDF2-HMAC-SHA256 (10000 iters) -> 32B key +
    16B IV, AES-256-CBC, PKCS7 padding."""
    import hashlib
    from Crypto.Cipher import AES  # pycryptodome (bundled)
    if data[:8] != b"Salted__":
        raise BuildError("encrypted blob missing openssl 'Salted__' header (not a FiiO OTA chunk?)", code="E211")
    salt = data[8:16]
    ct = data[16:]
    if len(ct) == 0 or len(ct) % 16 != 0:
        raise BuildError("encrypted blob has bad length (truncated chunk?)", code="E211")
    keyiv = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 10000, 48)
    pt = AES.new(keyiv[:32], AES.MODE_CBC, keyiv[32:48]).decrypt(ct)
    pad = pt[-1] if pt else 0
    if pad < 1 or pad > 16 or pt[-pad:] != bytes([pad]) * pad:
        raise BuildError("bad PKCS7 padding after AES decrypt (wrong key or firmware?)", code="E211")
    return pt[:-pad]


def extract_stock_rootfs(fw_zip, out_squashfs, workdir, rep=None):
    """Pull the exact stock rootfs.squashfs out of FiiO's official update zip by
    decrypting + reassembling the main_os OTA chunks. Byte-exact; written only
    after it validates (manifest size + squashfs magic), so a failure never
    clobbers the output."""
    rep = rep or CLIReporter()
    rep.phase("Extracting stock firmware")
    if not zipfile.is_zipfile(fw_zip):
        raise BuildError(f"not a zip archive: {fw_zip}", code="E201")

    ex = os.path.join(workdir, "fw_unzip")
    import shutil
    if os.path.isdir(ex):                 # fresh every time - never mix two firmwares' chunks
        shutil.rmtree(ex, ignore_errors=True)
    os.makedirs(ex)
    total_uncompressed = 0
    with zipfile.ZipFile(fw_zip) as zf:
        infos = zf.infolist()
        if len(infos) > 20000:            # per-entry count bound (not just aggregate size)
            raise BuildError("zip has an implausible number of entries - refusing.", code="E202")
        for m in infos:
            total_uncompressed += m.file_size
            if total_uncompressed > 4 * (1 << 30):   # 4 GiB bomb guard
                raise BuildError("zip expands beyond 4 GiB - refusing (possible zip bomb)", code="E202")
        rep.status("Unpacking firmware zip")
        for i, m in enumerate(infos):
            tgt = _safe_extract_member(zf, m, ex)
            if m.is_dir():
                os.makedirs(tgt, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(tgt), exist_ok=True)
                with zf.open(m) as src, open(tgt, "wb") as dst:
                    while True:
                        chunk = src.read(1 << 20)
                        if not chunk:
                            break
                        dst.write(chunk)
            rep.progress(i + 1, len(infos))

    # locate the ONE main_os OTA manifest (refuse ambiguity)
    import glob
    mans = sorted(glob.glob(os.path.join(ex, "**", "main_os", "ota_v*", "ota_update.in.enc"),
                            recursive=True))
    if len(mans) == 0:
        raise BuildError("no main_os/ota_v*/ota_update.in.enc in this zip - not a Disc "
                         "main-OS firmware?", code="E210")
    if len(mans) > 1:
        raise BuildError(f"{len(mans)} main_os manifests in this zip - ambiguous, aborting.", code="E210")
    man_enc = mans[0]
    ota_dir = os.path.dirname(man_enc)
    rep.log(f"OTA dir: {os.path.relpath(ota_dir, ex)}")

    manifest = _openssl_aes_decrypt(open(man_enc, "rb").read(), _OTA_KEY).decode("utf-8", "ignore")
    img_name, img_size = _parse_rootfs_manifest(manifest)
    if not img_name:
        raise BuildError("no rootfs image in the OTA manifest.", code="E210")
    # img_name must be a bare basename (no path separators / traversal)
    if img_name != os.path.basename(img_name) or img_name in ("", ".", "..") or "/" in img_name or "\\" in img_name:
        raise BuildError(f"OTA manifest rootfs image name is not a safe basename: {img_name!r}", code="E210")
    if img_size is not None:
        if not img_size.isdigit() or not (0 < int(img_size) <= 256 * (1 << 20)):
            raise BuildError(f"OTA manifest img_size is implausible: {img_size!r}", code="E210")
    rep.log(f"rootfs image={img_name} expected_size={img_size or '?'}")

    # order the $img.NNNN.enc chunks by numeric index (skip ota_sha256_* etc.);
    # reject DUPLICATE indices and require a contiguous 0..N-1 sequence.
    by_idx = {}
    for f in glob.glob(os.path.join(ota_dir, glob.escape(img_name) + ".*.enc")):
        rest = os.path.basename(f)[len(img_name) + 1:]      # "NNNN.<hash>.enc"
        idx = rest.split(".", 1)[0]
        if not idx.isdigit():
            continue
        i = int(idx)
        if i in by_idx:
            raise BuildError(f"duplicate rootfs chunk index {i} in the OTA dir - refusing.", code="E212")
        by_idx[i] = f
    if not by_idx:
        raise BuildError(f"no {img_name}.NNNN.*.enc chunks found in the OTA dir.", code="E212")
    idxs = sorted(by_idx)
    if idxs != list(range(len(idxs))):
        raise BuildError(f"rootfs chunk indices are not a contiguous 0..{len(idxs)-1} sequence "
                         f"(got {idxs[:3]}...{idxs[-1]}) - missing chunk, refusing.", code="E212")
    chunks = [(i, by_idx[i]) for i in idxs]

    tmpout = os.path.join(workdir, "rootfs.assembled")
    rep.status("Decrypting + assembling rootfs")
    with open(tmpout, "wb") as out:
        for i, (_idx, f) in enumerate(chunks):
            out.write(_openssl_aes_decrypt(open(f, "rb").read(), _OTA_KEY))
            rep.progress(i + 1, len(chunks))

    got = os.path.getsize(tmpout)
    rep.log(f"assembled {len(chunks)} chunks ({got} bytes)")
    if img_size and got != int(img_size):
        raise BuildError(f"assembled size {got} != manifest {img_size} (missing/dup chunk?)", code="E213")
    with open(tmpout, "rb") as f:
        if f.read(4) != SQUASH_MAGIC:
            raise BuildError("assembled output is not a squashfs (bad magic) - wrong key/firmware.", code="E213")

    os.replace(tmpout, out_squashfs)
    rep.ok(f"stock rootfs extracted -> {out_squashfs} ({got} bytes)")
    return out_squashfs


def _parse_rootfs_manifest(text):
    """From the decrypted ota_update.in, return (img_name, img_size) for the block
    whose img_type=rootfs (mirrors the awk in extract_stock_rootfs.sh)."""
    in_rootfs = False
    name = size = None
    for line in text.splitlines():
        line = line.strip()
        if line == "img_type=rootfs" or line.endswith("=rootfs") and line.startswith("img_type"):
            in_rootfs = True
            continue
        if in_rootfs:
            if line.startswith("img_name="):
                name = line[len("img_name="):]
            elif line.startswith("img_size="):
                size = line[len("img_size="):]
            if name and size:
                break
    return name, size


def _copyfile(src, dst):
    with open(src, "rb") as s, open(dst, "wb") as d:
        while True:
            b = s.read(1 << 20)
            if not b:
                break
            d.write(b)


# --- fiio_init.sh boot-hook patch (python-native, single-match-or-refuse) ----
_OLD_IF = 'if [ "$COREDUMP_FLAG" == "1" ]; then'
_LAUNCH = _OLD_IF + "\n    /usr/data/mq_ui &"
_GUARD_PATH_LINE = "export PATH=/opt/diskos/bin:$PATH"
# S96 writes exactly "diskos\n" or "stock\n". Only a POSITIVE record launches diskOS: missing, partial or
# malformed (selector hung, crashed, timed out or never ran) means stock. The record is read ONLY through
# the shared helper (payload/diskos-selected), which rejects a non-regular file before opening it, so a FIFO
# at the path cannot block the boot hook (short of the path being swapped between its check and its read);
# if the helper itself is missing the test fails, which is stock.
_SELECTED_HELPER = "/opt/diskos/bin/diskos-selected"
_BOOTPROBE = "/opt/diskos/bin/diskos-bootprobe"
_SELECT_LINE = _SELECTED_HELPER
# (image path, payload name) of every file the boot's stock/diskOS decision runs through
_BOOT_DECISION_FILES = (
    ("etc/init.d/S96diskos_select", "S96diskos_select"),
    ("etc/init.d/S97diskos_install", "S97diskos_install"),
    (_SELECTED_HELPER.lstrip("/"), "diskos-selected"),
    (_BOOTPROBE.lstrip("/"), "diskos-bootprobe"),
)
_BLOCK = (
    "# diskOS SD guard: the stock player runs 'umount /tmp/sdcard; rm -rf /tmp/sdcard' without checking\n"
    "# the umount; on a busy (mounted) card that empties the card. /opt/diskos/bin/rm refuses card\n"
    "# operands. Exported HERE (before any launch) so the initial launch, BOTH watchdog respawn\n"
    "# branches and the stock fallback all inherit it.\n"
    + _GUARD_PATH_LINE + "\n"
    "# READINESS GATE: /tmp/.diskos_ready is written by S97 only when it COMPLETED and left a verified\n"
    "# override in place. It is on tmpfs, so it means 'this boot'. Requiring it means a first-boot hook\n"
    "# that hung, was killed, or could not quarantine a bad install falls back to STOCK - the decision no\n"
    "# longer depends on S97 having successfully deleted anything from NAND.\n"
    "# STOCK SELECTION: S96 sampled the boot preference and the Vol-Up override once, before any diskOS\n"
    "# code ran, and recorded the result. Only a positive 'diskos' record launches diskOS, so reaching the\n"
    "# stock firmware never depends on diskOS - or on the selector itself - completing.\n"
    "if " + _SELECT_LINE + " && [ -f /tmp/.diskos_ready ] && [ -f /usr/data/mq_ui ] && [ -f /usr/data/mq_player ]; then\n"
    "    # diskOS override: run our UI + the player from /usr/data (persists across\n"
    "    # rootfs flashes).  Falls back to the stock rootfs binaries if either is absent.\n"
    "    /usr/data/mq_ui &\n    sleep 2\n    /usr/data/mq_player &\n"
    # The stock hook has a SECOND launch site: when its coredump flag is set it runs the /usr/data
    # binaries directly. On a diskOS device those are ours, so that branch was a way to launch diskOS
    # with the readiness gate bypassed entirely. It keeps its original meaning (launch from /usr/data)
    # but only when readiness says so; otherwise it falls through to the bare names, which are stock.
    'elif ' + _SELECT_LINE + ' && [ -f /tmp/.diskos_ready ] && [ "$COREDUMP_FLAG" == "1" ]; then'
)


# The pristine stock fiio_init.sh the boot-hook patch was designed and tested against, hashed as RAW BYTES.
# It is byte-identical on V1.95, V2.09 (from the pin-verified stock image), V2.28, V2.40 and V2.57. Any
# other script is refused outright - there is no override for this one - because the patch's safety rests
# on the script's exact structure, and no text heuristic can establish shell control flow (the previous
# heuristic was bypassed three different ways). A new firmware with a different script needs its own
# reviewed entry here.
KNOWN_FIIO_INIT_SHA256 = {"ad8455ad0a3d9577260419a06ad2e0802c24bc6bb8e9ed4b64637b7bd968ccf2"}

# The only forms in which a patched script may mention the diskOS (/usr/data) binaries, after strip().
_USR_DATA_ALLOWED = (
    'PROCESS_MQ_UI="/usr/data/mq_ui"',
    'PROCESS_MQ_PLAYER="/usr/data/mq_player"',
    '/usr/data/mq_ui &',
    '/usr/data/mq_player &',
)


def _validate_boot_hook(fi):
    """Sanity-check a PATCHED fiio_init.sh (bytes or str). Raises BuildError (E232).

    This is NOT the safety proof and does not claim to establish shell control flow. The guarantee is:
    _patch_fiio_init only ever patches the one pinned stock script (KNOWN_FIIO_INIT_SHA256, raw bytes), and
    the output image must contain exactly the bytes that patch produced (checked by the caller). These
    checks catch gross corruption early and with a readable message: strict UTF-8, no carriage returns,
    exactly one patch, the SD-guard PATH export first, the generated block intact, and no mention of the
    /usr/data binaries outside the known forms or outside a gated condition.
    """
    def bad(msg):
        raise BuildError("repacked image: fiio_init.sh " + msg + " - do NOT flash", code="E232")
    if isinstance(fi, (bytes, bytearray)):
        try:
            fi = bytes(fi).decode("utf-8")
        except UnicodeDecodeError:
            bad("is not valid UTF-8")
    # A lone CR is invisible to line-splitting tools but not to the shell: it can fold a following
    # command into a comment. The known script has none, so any CR is corruption.
    if "\r" in fi:
        bad("contains a carriage return")
    if fi.count("diskOS override") != 1:
        bad("does not contain exactly one diskOS boot-hook patch")
    g = fi.find(_GUARD_PATH_LINE)
    if g < 0 or g > fi.find("diskOS override"):
        bad("lacks the SD-guard PATH export before the launch block")
    if fi.count(_BLOCK + "\n    /usr/data/mq_ui &") != 1:
        bad("does not contain the generated launch block followed by the branch it replaced")
    lines = fi.splitlines()
    for i, ln in enumerate(lines):
        st = ln.strip()
        if st.startswith("#") or ("/usr/data/mq_ui" not in st and "/usr/data/mq_player" not in st):
            continue
        if st in _USR_DATA_ALLOWED:
            continue
        if st.startswith("if " + _SELECT_LINE + " && [ -f /tmp/.diskos_ready ] && [ -f /usr/data/mq_ui ]"):
            continue
        bad(f"line {i + 1} mentions the /usr/data binaries in an unrecognised form: {st[:80]!r}")
    launches = 0
    for i, ln in enumerate(lines):
        st = ln.strip()
        if st.startswith("#") or not (st.startswith("/usr/data/mq_ui") or st.startswith("/usr/data/mq_player")):
            continue
        launches += 1
        cond = None
        for j in range(i - 1, -1, -1):
            pj = lines[j].strip()
            if pj.startswith("if ") or pj.startswith("elif "):
                cond = pj
                break
            if pj in ("else", "fi") or pj.startswith("else "):
                break          # the nearest enclosing test is not a positive condition
        if cond is None or _SELECT_LINE not in cond or "[ -f /tmp/.diskos_ready ]" not in cond:
            bad(f"line {i + 1} launches /usr/data binaries without the stock selection and readiness "
                f"gate in its own condition")
    if launches < 2:
        bad("has no gated /usr/data launch (unexpected structure)")


def _check_decision_file(path, payload_path, name):
    """One boot-decision file in the output image: a regular file (lstat - never a symlink), permission
    bits EXACTLY 0755 (S_IMODE: no setuid/setgid/sticky either), and byte-identical to the payload."""
    import stat as _st
    m = os.lstat(path).st_mode
    if not _st.S_ISREG(m) or _st.S_IMODE(m) != 0o755:
        raise BuildError(f"repacked image: {name} is not a regular 0755 file (mode {oct(m)}) - do NOT flash",
                         code="E232")
    with open(path, "rb") as a, open(payload_path, "rb") as b:
        if a.read() != b.read():
            raise BuildError(f"repacked image: {name} differs from the shipped payload - do NOT flash", code="E232")


def _check_boot_script_output(actual, expected):
    """The output-image check for the boot script, on RAW BYTES: sanity checks, then exact equality with
    the bytes the build wrote (which _patch_fiio_init only ever derives from the one pinned script)."""
    _validate_boot_hook(actual)
    if actual != expected:
        raise BuildError("repacked image: fiio_init.sh differs from the patched script the build wrote - "
                         "do NOT flash", code="E232")


def _patch_fiio_init(path, rep):
    """Patch the stock boot script. Works on RAW BYTES end to end, so no newline translation or decoding
    leniency can make the hashed, patched or written bytes differ from what is checked."""
    import hashlib
    with open(path, "rb") as f:
        raw = f.read()
    sha = hashlib.sha256(raw).hexdigest()
    if sha not in KNOWN_FIIO_INIT_SHA256:
        raise BuildError(f"fiio_init.sh (sha256 {sha[:12]}...) is not the stock boot script the diskOS boot "
                         "hook was built and tested against - refusing to patch this firmware.", code="E223")
    s = raw.decode("utf-8")       # the known script is valid UTF-8; strict, never lossy
    if "diskOS override" in s:    # cannot happen for the known hash; kept as a belt-and-braces refusal
        raise BuildError("fiio_init.sh already contains a diskOS marker - refusing to patch it again.", code="E223")
    n = s.count(_LAUNCH)
    if n != 1:
        raise BuildError(
            "boot-hook anchor (COREDUMP launch block) "
            f"{'not found' if n == 0 else 'ambiguous'} in fiio_init.sh - "
            "incompatible firmware boot structure; do not ship this base untested.", code="E223")
    i = s.index(_LAUNCH)
    s = s[:i] + _BLOCK + s[i + len(_OLD_IF):]
    with open(path, "wb") as f:
        f.write(s.encode("utf-8"))


def _install_sd_guards(rf):
    """Replace the stock rm entry, never its BusyBox target or an outside path."""
    import hashlib, stat
    from pathlib import Path
    bindir = os.path.join(rf, "bin")
    _assert_within_rf(rf, bindir)
    busybox = os.path.join(bindir, "busybox")
    _assert_within_rf(rf, busybox)
    if not stat.S_ISREG(os.lstat(busybox).st_mode):
        raise BuildError("stock /bin/busybox is not a regular file", code="E233")
    before = hashlib.sha256(Path(busybox).read_bytes()).hexdigest()
    dst = os.path.join(bindir, "rm")
    # Validate the parent first, then unlink the directory entry itself. In
    # particular an absolute /bin/busybox symlink must never resolve on the host.
    if os.path.lexists(dst):
        if stat.S_ISDIR(os.lstat(dst).st_mode):
            raise BuildError("stock /bin/rm is a directory", code="E233")
        os.unlink(dst)
    src = bundle.data("diskos-rmguard")
    _install(src, dst, 0o755, rf)
    _install(src, os.path.join(rf, "opt/diskos/bin/rm"), 0o755, rf)
    if hashlib.sha256(Path(busybox).read_bytes()).hexdigest() != before:
        raise BuildError("BusyBox changed while installing the SD guard", code="E233")
    return before


def _validate_sd_guards(rf, expected_busybox_sha=None):
    import hashlib, stat
    from pathlib import Path
    expected = Path(bundle.data("diskos-rmguard")).read_bytes()
    for rel in ("bin/rm", "opt/diskos/bin/rm"):
        path = os.path.join(rf, rel)
        _assert_within_rf(rf, path, code="E232")
        try:
            mode = os.lstat(path).st_mode
            if not stat.S_ISREG(mode) or not mode & 0o111:
                raise OSError("not an executable regular file")
            if Path(path).read_bytes() != expected:
                raise OSError("content mismatch")
        except OSError as exc:
            raise BuildError(f"repacked image: invalid SD guard {rel}: {exc} - do NOT flash", code="E232") from exc
    if expected_busybox_sha is not None:
        path = os.path.join(rf, "bin/busybox")
        _assert_within_rf(rf, path, code="E232")
        if (not stat.S_ISREG(os.lstat(path).st_mode)
                or hashlib.sha256(Path(path).read_bytes()).hexdigest() != expected_busybox_sha):
            raise BuildError("repacked image: BusyBox differs from stock - do NOT flash", code="E232")


# --- ELF sanity for the UI binary -------------------------------------------
def _validate_ui_elf(ui_path):
    if not os.path.exists(ui_path):
        raise BuildError(f"UI binary not found: {ui_path}", code="E222")
    with open(ui_path, "rb") as f:
        hdr = f.read(20)
    if hdr[:4] != b"\x7fELF":
        raise BuildError(f"'{ui_path}' is not an ELF", code="E222")
    if hdr[4:6] != b"\x01\x01":
        raise BuildError(f"'{ui_path}' is not ELF32 little-endian (EI_CLASS/DATA)", code="E222")
    if hdr[18:20] != b"\x08\x00":
        raise BuildError(f"'{ui_path}' e_machine is not MIPS-LE", code="E222")


def _run(cmd, **kw):
    # cmd[0] is the resolved tool path; native_env tailors the loader path to whether
    # that tool is bundled (gets vendor/lib) or a system copy (clean env).
    return subprocess.run(cmd, env=bundle.native_env(cmd[0]), **kw)


def check_squashfs_tools(mksq, unsq, rep=None):
    """Prove the resolved squashfs tools can actually PACK and EXTRACT with the exact
    stock LZO parameters, BEFORE the real build touches the firmware. A system mksquashfs
    built without LZO (or an otherwise broken tool) fails here with a clear, actionable
    message instead of a cryptic mid-build error. Applies to bundled and system tools
    alike. A failure is reported as an LZO CAPABILITY failure (it can also be caused by
    disk space, permissions, resource limits or missing libraries), never asserted as
    'built without LZO'. Raises BuildError E104 on any failure."""
    import tempfile, filecmp
    _script = bundle.native_build_script("squashfs")
    _fixhint = ("diskOS requires working LZO compression and extraction; install squashfs-tools "
                f"with LZO support, or repair/rebuild the bundled tools with {_script} from the "
                "diskOS source")

    def _fail(stage, path, detail):
        detail = (detail or "").strip()[:300]
        raise BuildError(
            f"squashfs LZO capability check failed during {stage} using '{path}'"
            + (f": {detail}" if detail else ""), code="E104", action=_fixhint)

    try:
        with tempfile.TemporaryDirectory(prefix="diskos-sqcheck-") as td:
            src = os.path.join(td, "src")
            os.makedirs(src)
            payload = os.path.join(src, "probe.bin")
            # compressible, multi-block content (> one 128 KiB block) so the LZO data path runs
            with open(payload, "wb") as f:
                f.write(b"diskos-lzo-probe-0123456789abcdef" * 8192)   # ~256 KiB
            sqfs = os.path.join(td, "probe.sqfs")
            ext = os.path.join(td, "ext")

            try:
                pack = _run([mksq, src, sqfs, "-comp", "lzo", "-b", "131072", "-no-xattrs",
                             "-all-root", "-noappend", "-processors", "1"],
                            capture_output=True, text=True, timeout=120)
            except (OSError, subprocess.TimeoutExpired) as e:
                _fail("pack", mksq, str(e))
            if pack.returncode != 0 or not os.path.exists(sqfs):
                _fail("pack", mksq, (pack.stderr or "") + (pack.stdout or ""))
            try:
                unp = _run([unsq, "-processors", "1", "-d", ext, sqfs],
                           capture_output=True, text=True, timeout=120)
            except (OSError, subprocess.TimeoutExpired) as e:
                _fail("extract", unsq, str(e))
            if unp.returncode != 0:
                _fail("extract", unsq, (unp.stderr or "") + (unp.stdout or ""))
            out = os.path.join(ext, "probe.bin")
            if not os.path.exists(out) or not filecmp.cmp(payload, out, shallow=False):
                raise BuildError(
                    "squashfs LZO capability check failed: pack/extract round-trip bytes differ",
                    code="E104", action=_fixhint)
    except OSError as e:
        # the scratch dir / payload write / compare / cleanup failed (e.g. ENOSPC, EACCES) -
        # translate to a coded, actionable error rather than an uncoded OSError.
        raise BuildError(
            f"squashfs LZO capability check could not run: {e}", code="E104",
            action="check free disk space and temp-dir permissions and retry; " + _fixhint)
    if rep:
        rep.log("squashfs LZO pack/extract check: OK")


# --- case-sensitive scratch space (macOS) ------------------------------------
# The stock Snowsky Disc rootfs is built on Linux and contains filenames that differ
# ONLY by case in the same directory (e.g. main_1/back_home.png and main_1/BACK_HOME.png) -
# fine on a case-sensitive filesystem, but macOS's default APFS volume is case-INsensitive
# (case-preserving), so the second unsquashfs write collides with the first ("already
# exists") and the extraction fails deterministically, every time, on stock Mac setups.
def _is_case_sensitive(dir_path):
    """Probe whether dir_path's filesystem folds case. Uses a PRIVATE, exclusively-created
    temp subdirectory so it can never clobber a real file, be fooled by an unrelated
    differently-cased file, or race a concurrent probe. Raises OSError on I/O or permission
    failure - callers must NOT treat that as 'case-sensitive'."""
    import tempfile, shutil
    os.makedirs(dir_path, exist_ok=True)
    probe = tempfile.mkdtemp(prefix=".diskos-cs-", dir=dir_path)
    try:
        with open(os.path.join(probe, "csprobe"), "w") as f:
            f.write("x")
        # case-insensitive fs: the uppercase alias resolves back to the file we just wrote.
        # Use os.stat (not os.path.exists, which swallows EIO etc. and would misreport a failing
        # filesystem as case-sensitive): only a real absence -> case-sensitive; any other error
        # propagates so the caller can fail closed (E234).
        try:
            os.stat(os.path.join(probe, "CSPROBE"))
            return False
        except FileNotFoundError:
            return True
    finally:
        shutil.rmtree(probe, ignore_errors=True)


def _hdiutil(args, what, timeout=180):
    """Run one hdiutil subcommand, mapping a missing tool, a timeout, or a nonzero exit to E234."""
    try:
        r = subprocess.run(["hdiutil", *args], capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        raise BuildError("hdiutil not found - a case-sensitive scratch volume is required to "
                         "build on a case-insensitive macOS filesystem.", code="E234")
    except subprocess.TimeoutExpired:
        raise BuildError(f"failed to {what}: hdiutil timed out after {timeout}s", code="E234")
    if r.returncode != 0:
        raise BuildError(f"failed to {what}: {(r.stderr or r.stdout).strip()[:400]}", code="E234")
    return r


def _scratch_detach(mnt, rep):
    """Detach the scratch volume at mnt and CONFIRM it is gone. Bounded retries, escalating to
    -force, so a transiently-busy volume never leaves a live mount the caller then 'succeeds'
    over. Warns (never silently accepts) if it truly cannot be detached. Returns True if unmounted."""
    import time
    for i in range(6):
        if not os.path.ismount(mnt):
            return True
        try:
            subprocess.run(["hdiutil", "detach", mnt] + (["-force"] if i >= 3 else []),
                           capture_output=True, text=True, timeout=60)
        except FileNotFoundError:
            break
        except subprocess.TimeoutExpired:
            pass   # count this as a failed attempt; the loop retries and then escalates to -force
        if not os.path.ismount(mnt):
            return True
        time.sleep(0.4 * (i + 1))
    if os.path.ismount(mnt):
        rep.warning(f"the case-sensitive scratch volume at {mnt} could not be detached; run "
                    "'hdiutil detach' on it before deleting the build directory.")
        return False
    return True


@contextlib.contextmanager
def _case_sensitive_extract_root(workdir, rep):
    """Yield a directory the case-colliding stock rootfs can be unsquashfs'd into safely.

    If workdir is already on a case-sensitive filesystem (Linux, or a case-sensitive macOS
    volume) yield it unchanged - a true no-op. Otherwise, on macOS only, create a FRESH
    case-sensitive APFS sparse image, mount it under workdir, VERIFY it, yield the mountpoint,
    and ALWAYS detach + delete it afterwards. Fails closed (E234) on any hdiutil error, a
    case-insensitive non-macOS host, or a scratch volume that does not verify - never an unsafe
    fallback extraction."""
    try:
        cs = _is_case_sensitive(workdir)
    except OSError as e:
        raise BuildError(f"could not probe the build filesystem at '{workdir}': {e}", code="E234")
    if cs:
        yield workdir
        return
    if sys.platform != "darwin":
        raise BuildError(
            f"'{workdir}' is on a case-insensitive filesystem, which cannot hold this rootfs "
            "(it has files that differ only by case). Rebuild on a case-sensitive filesystem.",
            code="E234")

    base = os.path.join(workdir, ".cs-scratch")
    img = base + ".sparseimage"
    mnt = base
    # Never let a planted symlink redirect create / attach / delete.
    for p in (img, mnt):
        if os.path.islink(p):
            raise BuildError(f"refusing a symlinked scratch path: {p}", code="E234")
    # A prior interrupted build can leave a stale mount/image. Detach a stale mount (verified) and
    # drop a stale image so we always build on a FRESH, verified volume. Fail closed if a stale
    # mount will not detach - do NOT reuse it or delete anything under it.
    if os.path.ismount(mnt) and not _scratch_detach(mnt, rep):
        raise BuildError(f"a previous scratch volume at {mnt} is still mounted; unmount it and retry.",
                         code="E234")
    if os.path.exists(img):
        try:
            os.remove(img)
        except OSError as e:
            raise BuildError(f"could not remove a stale scratch image {img}: {e}", code="E234")

    rep.log("build directory is on a case-insensitive filesystem - creating a case-sensitive "
            "APFS scratch volume for extraction")
    try:
        # create is INSIDE the cleanup scope, so a partial image from a failed/interrupted
        # create is removed by the finally below.
        _hdiutil(["create", "-size", "2g", "-type", "SPARSE", "-fs", "Case-sensitive APFS",
                  "-volname", "diskOS-build", base], "create the case-sensitive scratch volume")
        os.makedirs(mnt, exist_ok=True)
        _hdiutil(["attach", img, "-mountpoint", mnt, "-nobrowse", "-owners", "on"],
                 "mount the case-sensitive scratch volume")
        if not os.path.ismount(mnt):
            raise BuildError("the case-sensitive scratch volume did not mount as expected", code="E234")
        try:
            if not _is_case_sensitive(mnt):
                raise BuildError("the scratch volume is not case-sensitive as requested", code="E234")
        except OSError as e:
            raise BuildError(f"the scratch volume is not usable: {e}", code="E234")
        yield mnt
    finally:
        _scratch_detach(mnt, rep)
        # Remove the backing image only once the volume is actually detached.
        if not os.path.ismount(mnt):
            try:
                if os.path.exists(img):
                    os.remove(img)
            except OSError as e:
                rep.warning(f"could not remove scratch image {img}: {e}")
            try:
                if os.path.isdir(mnt):
                    os.rmdir(mnt)
            except OSError:
                pass


def _validate_squashfs_output(sq_path, unsq, expect_ui_sha, expect_ui_sz, extract_root, rep=None, expected_busybox_sha=None,
                              expected_fiio_init=None):
    """Validate a freshly-repacked squashfs by its CONTENT, not the repacker's exit status. Does a
    COMPLETE independent extraction (so silent corruption ANYWHERE fails, not just in two files),
    then verifies every boot-critical artefact: the boot-hook patch in fiio_init.sh, the executable
    first-boot installer hook, the manifest (agreeing with the embedded UI), and the embedded UI
    itself (exact sha256 + size + exec bit). Raises BuildError (E232) on any problem."""
    import hashlib, tempfile, shutil
    with open(sq_path, "rb") as f:
        if f.read(4) != b"hsqs":
            raise BuildError("repacked image is not a valid squashfs (bad superblock magic) - "
                             "the repacker produced a corrupt file", code="E232")
    # This is a FULL re-extraction of the same rootfs (same case-colliding filenames as the
    # original unpack), so it needs the same case-sensitive destination - never the system tmp
    # dir, which on macOS lives on the same case-insensitive volume as everything else.
    tmp = tempfile.mkdtemp(prefix="diskos-sqcheck-", dir=extract_root)
    try:
        dst = os.path.join(tmp, "x")
        # FULL extraction (no file subset): a corrupt inode / metadata block / file anywhere in the
        # bootable filesystem makes this fail, which two-file extraction would miss.
        r = _run([unsq, "-d", dst, "-f", sq_path], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"repacked image failed full extraction (unsquashfs rc={r.returncode}) - "
                             f"corrupt/truncated repack: {r.stderr.strip()[:200]}", code="E232")

        def _need(rel, what):
            p = os.path.join(dst, rel)
            _assert_within_rf(dst, p, code="E232")   # a crafted rootfs must not escape via a symlink
            if not os.path.exists(p):
                raise BuildError(f"repacked image is missing {what} ({rel}) - do NOT flash", code="E232")
            return p

        # boot hook present + patched
        with open(_need("usr/project/fiio_init.sh", "boot script"), "rb") as f:
            fi = f.read()                 # RAW bytes: no newline translation, no lossy decoding
        if expected_fiio_init is None:
            raise BuildError("internal: output validation needs the patched boot script bytes", code="E232")
        _check_boot_script_output(fi, expected_fiio_init)
        sel = _need("etc/init.d/S96diskos_select", "boot selector")
        if not (os.stat(sel).st_mode & 0o111):
            raise BuildError("repacked image: S96diskos_select is not executable", code="E232")
        # exact identity, not just existence: the boot-time scripts are what decide stock vs diskOS
        # every file the stock/diskOS decision runs through: exact bytes, a regular file (not a symlink),
        # and permission bits exactly 0755 - a non-executable helper would silently force every boot to stock
        for rel, name in _BOOT_DECISION_FILES:
            _check_decision_file(_need(rel, name), bundle.data(name), name)
        _validate_sd_guards(dst, expected_busybox_sha)
        # first-boot installer hook present + executable
        s97 = _need("etc/init.d/S97diskos_install", "first-boot installer hook")
        if not (os.stat(s97).st_mode & 0o111):
            raise BuildError("repacked image: S97diskos_install is not executable", code="E232")
        # embedded UI: exact identity + exec bit
        ui = _need("opt/diskos/mq_ui", "embedded UI")
        if os.path.getsize(ui) != expect_ui_sz:
            raise BuildError(f"repacked image: embedded UI size {os.path.getsize(ui)} != {expect_ui_sz}", code="E232")
        if hashlib.sha256(open(ui, "rb").read()).hexdigest() != expect_ui_sha:
            raise BuildError("repacked image: embedded UI hash mismatch (repack corrupted it) - do NOT flash", code="E232")
        if not (os.stat(ui).st_mode & 0o111):
            raise BuildError("repacked image: embedded UI is not executable", code="E232")
        # manifest present + agrees with the embedded UI (the on-device hook trusts it)
        man = _need("etc/diskos_manifest", "diskOS manifest")
        if (_grep1(man, r"SHA256=([0-9a-fA-F]+)") != expect_ui_sha
                or _grep1(man, r"SIZE=([0-9]+)") != str(expect_ui_sz)):
            raise BuildError("repacked image: manifest/UI mismatch - do NOT flash", code="E232")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if rep is not None:
        rep.log("output squashfs validated (full extraction + boot hook + S97 + manifest + UI hash/mode)")


def build_image(stock_squashfs, ui_binary, variant, out_bin, workdir, rep=None):
    """Build diskos_<variant>.bin from a stock rootfs + the diskOS UI."""
    rep = rep or CLIReporter()
    if variant not in ("public", "dev"):
        raise BuildError(f"variant must be 'public' or 'dev', got {variant!r}", code="E250")
    rep.phase(f"Building diskOS image ({variant})")

    unsq = bundle.native("unsquashfs")
    mksq = bundle.native("mksquashfs")
    # Prove the resolved squashfs tools can pack+extract with the stock LZO params BEFORE
    # unpacking the firmware - so a system tool without LZO fails cleanly, not mid-build.
    check_squashfs_tools(mksq, unsq, rep)
    with _case_sensitive_extract_root(workdir, rep) as extract_root:
        rf = os.path.join(extract_root, "rf")
        if os.path.isdir(rf):
            import shutil
            shutil.rmtree(rf)

        rep.status("[1/6] unpacking stock rootfs")
        r = _run([unsq, "-d", rf, stock_squashfs], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"unsquashfs failed: {r.stderr.strip()[:400]}", code="E230")

        rep.status("[2/6] validating base is a Snowsky Disc rootfs")
        require_installable(validate_stock_rootfs(stock_squashfs, rep, allow_override=False))   # exact pin, then policy

        rep.status("[3/6] validating the diskOS UI binary")
        _validate_ui_elf(ui_binary)

        rep.status("[4/6] patching fiio_init.sh + installing first-boot hook")
        fiio_path = os.path.join(rf, "usr/project/fiio_init.sh")
        _assert_within_rf(rf, fiio_path)          # a crafted rootfs must not redirect the in-place patch
        _patch_fiio_init(fiio_path, rep)
        with open(fiio_path, "rb") as f:
            patched_fiio_init = f.read()
        # Runs before S97 so the stock choice is made ahead of any diskOS installation work.
        _install(bundle.data("S96diskos_select"), os.path.join(rf, "etc/init.d/S96diskos_select"), 0o755, rf)
        _install(bundle.data("S97diskos_install"), os.path.join(rf, "etc/init.d/S97diskos_install"), 0o755, rf)
        # the one shell reader of the boot-selection record, used by S97 and by the boot hook
        _install(bundle.data("diskos-selected"), os.path.join(rf, _SELECTED_HELPER.lstrip("/")), 0o755, rf)
        # S96's bounded native probe: errno-aware preference lookup + pin read, under its own supervisor
        _install(bundle.data("diskos-bootprobe"), os.path.join(rf, _BOOTPROBE.lstrip("/")), 0o755, rf)
        # Guard the actual /bin entry too: default PATH and stock fallbacks must
        # remain protected even when the custom UI/NAND override is unavailable.
        busybox_sha = _install_sd_guards(rf)

        import hashlib
        ui_sha = hashlib.sha256(open(ui_binary, "rb").read()).hexdigest()
        ui_sz = os.path.getsize(ui_binary)
        import time
        manifest_path = os.path.join(rf, "etc/diskos_manifest")
        _assert_within_rf(rf, manifest_path)
        if os.path.islink(manifest_path):
            os.unlink(manifest_path)              # never follow a planted symlink at the manifest path
        with open(manifest_path, "w") as f:
            f.write(f"SHA256={ui_sha}\nSIZE={ui_sz}\nARCH=mips-le\nVARIANT={variant}\n"
                    f"BUILT={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")

        # Embed the UI INSIDE the rootfs at a fixed path the S97 hook installs from FIRST (SD is only a
        # fallback source). This makes first boot need no SD card: flash -> reboot -> diskOS. The hook
        # still verify_ui's this copy against the manifest above, so a corrupt flash can't run it.
        _install(ui_binary, os.path.join(rf, "opt/diskos/mq_ui"), 0o755, rf)

        # Debug-access tooling (BOTH variants): the diskos-debug helper + a static dropbear. mq_ui's
        # "Debug Mode" toggle drives these to start SSH (random per-enable password) and/or the USB
        # serial shell on demand. Shipping them in the public image too means a normal user can enable
        # debug access from the UI without needing the dev build.
        _install(bundle.data("dropbearmulti"), os.path.join(rf, "usr/project/dropbearmulti"), 0o755, rf)
        _install(bundle.data("diskos-debug.sh"), os.path.join(rf, "usr/project/diskos-debug.sh"), 0o755, rf)

        if variant == "dev":
            # Dev only: an ALWAYS-ON USB serial recovery shell (builds the gadget + attaches the one
            # diskos-debug shell at boot), so a dev build is reachable over USB even before the UI runs.
            _install(bundle.data("S99usbserial"), os.path.join(rf, "etc/init.d/S99usbserial"), 0o755, rf)

        rep.status("[5/6] repacking squashfs (stock params: lzo, -b 131072)")
        out_sq = os.path.join(workdir, "out.squashfs")
        if os.path.exists(out_sq):
            os.remove(out_sq)
        r = _run([mksq, rf, out_sq, "-comp", "lzo", "-b", "131072",
                  "-no-xattrs", "-all-root", "-noappend"], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"mksquashfs failed: {r.stderr.strip()[:400]}", code="E230")
        sqsz = os.path.getsize(out_sq)
        if sqsz > IMG_SIZE:
            raise BuildError(
                f"squashfs is {sqsz} > {IMG_SIZE} partition - refusing (truncating would "
                "make it unbootable). Trim content or use a smaller UI.", code="E231")

        # Trust the OUTPUT, not the repacker's exit code: confirm the superblock magic AND that the
        # embedded UI extracts byte-identical (an independent unsquashfs round-trip). Catches a
        # silently-corrupt/truncated repack - the exact failure mode a nonzero-exit-but-valid (or, worse,
        # zero-exit-but-corrupt) mksquashfs could hide - before it ever reaches the device.
        _validate_squashfs_output(out_sq, unsq, ui_sha, ui_sz, extract_root, rep, busybox_sha,
                                  expected_fiio_init=patched_fiio_init)

        rep.status("[6/6] finalizing image (pad to partition size)")
        _copyfile(out_sq, out_bin)
        with open(out_bin, "r+b") as f:      # pad to exact partition size
            f.truncate(IMG_SIZE)

        import hashlib as _h
        md5 = _h.md5(open(out_bin, "rb").read()).hexdigest()
        rep.ok(f"image built: {out_bin} ({os.path.getsize(out_bin)} bytes) md5={md5}")
        return out_bin


def _grep1(path, pattern):
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            m = re.search(pattern, f.read())
            return m.group(1) if m else None
    except OSError:
        return None


def _assert_within_rf(rf, dst, code="E233"):
    """Refuse a write target that, via a symlink AT the target or in any PARENT component, resolves
    OUTSIDE the extracted rootfs `rf`. Image building runs with the caller's privileges (often sudo,
    since mask-ROM USB needs it), so a crafted stock squashfs that ships e.g.
    `etc/init.d/S97diskos_install -> /etc/cron.d/x` could make a plain open() clobber a host file.
    realpath() resolves every symlink on the path, so an escape is caught here before any write."""
    rroot = os.path.realpath(rf)
    real = os.path.realpath(dst)
    if not (real == rroot or real.startswith(rroot + os.sep)):
        raise BuildError(
            f"refusing to write outside the rootfs: {dst!r} resolves to {real!r} via a symlink - "
            "the stock firmware may be crafted or corrupt. Re-download the official FiiO firmware.",
            code=code)


def _install(src, dst, mode, rf):
    _assert_within_rf(rf, dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.islink(dst):
        os.unlink(dst)          # replace a planted symlink with a real file - never follow it
    _copyfile(src, dst)
    os.chmod(dst, mode)
