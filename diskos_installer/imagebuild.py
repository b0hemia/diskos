"""Build a flashable diskOS image from the user's OWN stock firmware.

Faithful Python port of the proven mkdiskos.sh / extract_stock_rootfs.sh logic:
  extract_stock_rootfs(zip) -> stock rootfs.squashfs  (byte-exact from FiiO's zip)
  build_image(stock, mq_ui, variant) -> diskos_<variant>.bin (76021760 bytes)

We NEVER ship FiiO's rootfs; the user supplies their official firmware zip and we
build locally. squashfs pack/unpack delegates to the bundled mksquashfs/unsquashfs
(reference tools) - we do not reimplement squashfs.
"""

import os
import re
import subprocess
import sys
import zipfile

from . import bundle
from .reporter import CLIReporter

IMG_SIZE = 76021760          # diskOS image size: 580 NAND blocks (~72.5 MiB); written to the start of the mtd2 rootfs partition (RO squashfs need not fill the 128 MB partition)

# Known-good stock rootfs.squashfs, verified out-of-band (NOT trusting the in-zip OTA manifest,
# which an attacker could modify consistently). A tested firmware whose extracted rootfs does not
# match its pin is refused: this rejects a modified/tampered/corrupt rootfs before it is patched
# and flashed. Map: MAIN_OS_VER -> (sha256, size_bytes). Add a version's pin only after hashing an
# authentic copy of that firmware.
PINNED_ROOTFS = {
    "228": ("0ffd877bca2c69ddff9ca70f4494da0d9e580c18d0f587e2c6d9921f2db82bd2", 72957952),
    "209": ("f1e3c69fb0e88b923c135558e01f4387a661f68839c8118e8ad490bdc9fc74e6", 75919360),
}
# Firmware versions diskOS has been flash-tested against. Others have DIFFERENT command-tag
# meanings, so diskOS built on them can send wrong commands and misbehave/reboot.
TESTED_FW = {"209", "228"}
SQUASH_MAGIC = b"hsqs"


def validate_stock_rootfs(stock_squashfs, rep=None):
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
        _run([unsq, "-d", os.path.join(tmp, "x"), "-f", stock_squashfs,
              "etc/product_version/version.in"], capture_output=True, text=True)
        ver_in = os.path.join(tmp, "x", "etc/product_version/version.in")
        prod = _grep1(ver_in, r"PRODUCT=([A-Za-z0-9_]+)")
        mver = _grep1(ver_in, r"MAIN_OS_VER=([0-9]+)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    override = os.environ.get("DISKOS_ALLOW_UNTESTED_FW") == "1"
    if prod != "SNOWSKY_DISC":
        raise BuildError(f"not a Snowsky Disc rootfs (PRODUCT={prod!r})", code="E220")
    if mver not in TESTED_FW and not override:
        raise BuildError(
            f"firmware MAIN_OS_VER={mver or '?'} is not tested (supported: {', '.join(sorted(TESTED_FW))}). "
            "Other versions can have incompatible command meanings. Re-run with "
            "DISKOS_ALLOW_UNTESTED_FW=1 at your own risk.", code="E221")
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
                    "may be modified, corrupt, or repackaged - re-download the official FiiO firmware. "
                    "(Set DISKOS_ALLOW_UNTESTED_FW=1 at your own risk.)", code="E224")
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
                         f"(got {idxs[:3]}…{idxs[-1]}) - missing chunk, refusing.", code="E212")
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
_BLOCK = (
    "if [ -f /usr/data/mq_ui ] && [ -f /usr/data/mq_player ]; then\n"
    "    # diskOS override: run our UI + the player from /usr/data (persists across\n"
    "    # rootfs flashes).  Falls back to the stock rootfs binaries if either is absent.\n"
    "    /usr/data/mq_ui &\n    sleep 2\n    /usr/data/mq_player &\n"
    'elif [ "$COREDUMP_FLAG" == "1" ]; then'
)


def _patch_fiio_init(path, rep):
    with open(path, encoding="utf-8", errors="surrogateescape") as f:
        s = f.read()
    # We always start from freshly-extracted OFFICIAL stock, so the file must be
    # UNpatched: refuse anything already containing our marker (corrupt/re-used tree)
    # rather than trusting it.
    if "diskOS override" in s:
        raise BuildError("fiio_init.sh already contains a diskOS marker - refusing to "
                         "patch a non-pristine rootfs. Re-extract from official firmware.", code="E223")
    n = s.count(_LAUNCH)
    if n != 1:
        raise BuildError(
            "boot-hook anchor (COREDUMP launch block) "
            f"{'not found' if n == 0 else 'ambiguous'} in fiio_init.sh - "
            "incompatible firmware boot structure; do not ship this base untested.", code="E223")
    i = s.index(_LAUNCH)
    s = s[:i] + _BLOCK + s[i + len(_OLD_IF):]
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(s)


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
    return subprocess.run(cmd, env=bundle.native_env(), **kw)


# --- case-sensitive scratch space (macOS) ------------------------------------
# The stock Snowsky Disc rootfs is built on Linux and contains filenames that differ
# ONLY by case in the same directory (e.g. main_1/back_home.png and main_1/BACK_HOME.png) -
# fine on a case-sensitive filesystem, but macOS's default APFS volume is case-INsensitive
# (case-preserving), so the second unsquashfs write collides with the first ("already
# exists") and the extraction fails deterministically, every time, on stock Mac setups.
def _is_case_sensitive(dir_path):
    """Probe whether dir_path's filesystem folds case, by writing one name and checking
    whether a differently-cased name is then visible."""
    os.makedirs(dir_path, exist_ok=True)
    lo = os.path.join(dir_path, ".diskos-cs-probe")
    hi = os.path.join(dir_path, ".DISKOS-CS-PROBE")
    try:
        with open(lo, "w"):
            pass
        return not os.path.exists(hi)
    finally:
        try:
            os.remove(lo)
        except OSError:
            pass


def _macos_case_sensitive_scratch(workdir, rep):
    """Create (or reuse) a small case-sensitive APFS sparse disk image under `workdir` and
    return its mount point. Sized generously (sparse - actual disk usage is only what's
    written) since the fully-unpacked rootfs runs to a couple hundred MB uncompressed."""
    mnt = os.path.join(workdir, ".cs-scratch")
    img_base = os.path.join(workdir, ".cs-scratch")
    img = img_base + ".sparseimage"
    if os.path.ismount(mnt):
        return mnt
    os.makedirs(mnt, exist_ok=True)
    if not os.path.exists(img):
        rep.log("build directory is on a case-insensitive filesystem - creating a "
                 "case-sensitive APFS scratch volume for extraction")
        r = subprocess.run(
            ["hdiutil", "create", "-size", "2g", "-type", "SPARSE", "-fs", "Case-sensitive APFS",
             "-volname", "diskOS-build", img_base],
            capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(
                f"failed to create a case-sensitive scratch volume: {r.stderr.strip()[:400]}",
                code="E234")
    r = subprocess.run(["hdiutil", "attach", img, "-mountpoint", mnt, "-nobrowse", "-quiet"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise BuildError(
            f"failed to mount the case-sensitive scratch volume: {r.stderr.strip()[:400]}",
            code="E234")
    return mnt


def _detach_case_sensitive_scratch(workdir):
    mnt = os.path.join(workdir, ".cs-scratch")
    if os.path.ismount(mnt):
        subprocess.run(["hdiutil", "detach", mnt, "-quiet"], capture_output=True, text=True)


def _ensure_case_sensitive_root(workdir, rep):
    """Return a directory to extract the (case-sensitive-only-safe) rootfs into: `workdir`
    itself if its filesystem is already case-sensitive, otherwise a case-sensitive scratch
    volume mounted under it (macOS only - other supported hosts are case-sensitive already)."""
    if _is_case_sensitive(workdir):
        return workdir
    if sys.platform != "darwin":
        raise BuildError(
            f"'{workdir}' is on a case-insensitive filesystem, which cannot hold this rootfs "
            "(it has files differing only by case) - rebuild on a case-sensitive filesystem.",
            code="E234")
    return _macos_case_sensitive_scratch(workdir, rep)


def _validate_squashfs_output(sq_path, unsq, expect_ui_sha, expect_ui_sz, extract_root, rep=None):
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
            if not os.path.exists(p):
                raise BuildError(f"repacked image is missing {what} ({rel}) - do NOT flash", code="E232")
            return p

        # boot hook present + patched
        with open(_need("usr/project/fiio_init.sh", "boot script"), encoding="utf-8", errors="ignore") as f:
            if "diskOS override" not in f.read():
                raise BuildError("repacked image: fiio_init.sh lacks the diskOS boot-hook patch", code="E232")
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
    extract_root = _ensure_case_sensitive_root(workdir, rep)
    try:
        rf = os.path.join(extract_root, "rf")
        if os.path.isdir(rf):
            import shutil
            shutil.rmtree(rf)

        rep.status("[1/6] unpacking stock rootfs")
        r = _run([unsq, "-d", rf, stock_squashfs], capture_output=True, text=True)
        if r.returncode != 0:
            raise BuildError(f"unsquashfs failed: {r.stderr.strip()[:400]}", code="E230")

        rep.status("[2/6] validating base is a Snowsky Disc rootfs")
        validate_stock_rootfs(stock_squashfs, rep)   # product / tested-version / known-good-hash gate

        rep.status("[3/6] validating the diskOS UI binary")
        _validate_ui_elf(ui_binary)

        rep.status("[4/6] patching fiio_init.sh + installing first-boot hook")
        fiio_path = os.path.join(rf, "usr/project/fiio_init.sh")
        _assert_within_rf(rf, fiio_path)          # a crafted rootfs must not redirect the in-place patch
        _patch_fiio_init(fiio_path, rep)
        _install(bundle.data("S97diskos_install"), os.path.join(rf, "etc/init.d/S97diskos_install"), 0o755, rf)

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
        _validate_squashfs_output(out_sq, unsq, ui_sha, ui_sz, extract_root, rep)

        rep.status("[6/6] finalizing image (pad to partition size)")
        _copyfile(out_sq, out_bin)
        with open(out_bin, "r+b") as f:      # pad to exact partition size
            f.truncate(IMG_SIZE)

        import hashlib as _h
        md5 = _h.md5(open(out_bin, "rb").read()).hexdigest()
        rep.ok(f"image built: {out_bin} ({os.path.getsize(out_bin)} bytes) md5={md5}")
        return out_bin
    finally:
        if extract_root != workdir:
            _detach_case_sensitive_scratch(workdir)


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
