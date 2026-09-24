"""Application service - the install / restore-stock / remove flows, independent
of any front-end. CLI and GUI both call these with a Reporter and a `confirm`
callback, so the two front-ends can never diverge in behaviour or safety checks.

`confirm(summary: dict) -> bool` is the destructive-action gate:
  - CLI: a y/N prompt.
  - GUI: the CONFIRMATION screen (acknowledge checkbox + explicit button),
    driven from the worker thread via a blocking Event.

Each flow returns a result dict; raises BuildError/FlashError on failure.
"""

import os
import time

from . import bundle, flasher, imagebuild, state


def _save_stock(stock_sq, rep):
    """Copy the user's bone-stock image into state (padded to the partition size)
    so restore-stock can always reflash exactly it. Returns its sha256."""
    rep.status("Saving your stock rootfs image (so diskOS can always be deactivated/reverted)")
    sz = os.path.getsize(stock_sq)
    if sz > imagebuild.IMG_SIZE:
        # NEVER truncate a larger image - a truncated file can still look flashable
        # (hsqs magic + right size) yet be a corrupt, unbootable stock.
        raise imagebuild.BuildError(
            f"stock rootfs is {sz} bytes > {imagebuild.IMG_SIZE} partition - refusing "
            "(truncating it would produce a corrupt 'stock' image).", code="E240")
    if sz < imagebuild.IMG_SIZE:
        padded = stock_sq + ".padded"
        imagebuild._copyfile(stock_sq, padded)
        with open(padded, "r+b") as f:
            f.truncate(imagebuild.IMG_SIZE)
        src = padded
    else:
        src = stock_sq
    digest = state.save_stock_image(src, progress=lambda r, t: rep.progress(r, t))
    rep.ok(f"bone-stock image saved (sha256={digest[:16]}...)")
    return digest


def _ensure_full_size(image_path, rep):
    """Repad a saved stock image that is SHORTER than IMG_SIZE up to IMG_SIZE with zero padding (the same
    convention _save_stock and validate_stock_rootfs use), so the 768-block writer covers the whole image.
    Fixes old 580-block/76 MB backups that would otherwise fail the exact-size preflight (E121). A backup
    LARGER than IMG_SIZE is real corruption - reject rather than truncate (truncation could hide it)."""
    sz = os.path.getsize(image_path)
    if sz == imagebuild.IMG_SIZE:
        return image_path
    if sz > imagebuild.IMG_SIZE:
        raise flasher.FlashError(
            f"saved stock image is {sz} bytes > {imagebuild.IMG_SIZE} - refusing (truncating it could "
            "hide a corrupt image)", code="E142",
            action="delete the saved image and rebuild it from FiiO's firmware .zip")
    padded = image_path + ".restorepad"   # next to the saved backup (an existing state dir)
    imagebuild._copyfile(image_path, padded)
    with open(padded, "r+b") as f:
        f.truncate(imagebuild.IMG_SIZE)   # zero-fills the tail to the partition size
    rep.log(f"repadded saved stock image {sz} -> {imagebuild.IMG_SIZE} bytes for the 768-block writer")
    return padded


def _save_state_soft(st, rep):
    """Persist state, but NEVER let a bookkeeping failure masquerade as a flash
    failure. Call only AFTER a verified flash."""
    try:
        state.save(st)
    except Exception as e:  # disk full, permissions, etc.
        rep.warning(f"flash succeeded, but saving local history failed: {e}")


def do_install(params, rep, confirm):
    """params: dict(firmware=?, stock=?, ui_binary=?, variant='public'|'dev').
    Extract -> save bone-stock -> build -> confirm -> flash. Returns result dict."""
    ui_bin = params.get("ui_binary") or bundle.data("mq_ui", required=False)
    if not ui_bin or not os.path.exists(ui_bin):
        raise imagebuild.BuildError("no diskOS UI binary (bundled 'mq_ui' missing).", code="E102",
                                   action="the install is incomplete; re-download the installer")
    variant = params.get("variant", "public")

    work = state.build_dir()
    st = state.load()
    st.update({"phase": "prepared", "variant": variant})
    state.save(st)

    # 1) obtain the stock rootfs + save it as the restore image
    stock_sq = os.path.join(work, "stock_rootfs.squashfs")
    if params.get("stock"):
        imagebuild._copyfile(params["stock"], stock_sq)
    elif params.get("firmware"):
        imagebuild.extract_stock_rootfs(params["firmware"], stock_sq, work, rep=rep)
    else:
        raise imagebuild.BuildError("need a firmware .zip or a stock rootfs.squashfs.", code="E140",
                                   action="pass your official FiiO firmware .zip")
    # Validate the stock image is a genuine, supported, known-good Disc rootfs BEFORE it overwrites
    # the saved recovery copy - so a wrong/corrupt image can't destroy a good recovery then abort.
    # The install policy (INSTALL_FW) is checked here too, for the same reason: refusing an unsupported
    # firmware must leave the saved recovery image untouched.
    imagebuild.require_installable(imagebuild.validate_stock_rootfs(stock_sq, rep, allow_override=False))
    _save_stock(stock_sq, rep)

    # 2) build the diskOS image
    out_bin = os.path.join(work, f"diskos_{variant}.bin")
    imagebuild.build_image(stock_sq, ui_bin, variant, out_bin, work, rep=rep)

    # 3) preflight + confirm (destructive gate), then flash
    flasher.preflight(out_bin, rep)
    summary = {
        "action": "install",
        "variant": variant,
        "image": out_bin,
        "duration": "~15 minutes",
        "consequence": "This rewrites the device root filesystem. Do not disconnect.",
    }
    if not confirm(summary):
        rep.warning("aborted before flashing (nothing written to the device).")
        return {"ok": False, "aborted": True}

    st.update({"phase": "flash-started"})
    _save_state_soft(st, rep)
    # From here, flash() either raises (real failure) or returns verified. A later
    # state-save error must NOT turn a verified flash into a reported failure.
    d = flasher.flash(out_bin, log_path=os.path.join(state.state_dir(), "last-flash.log"), rep=rep)
    st.update({"phase": "flash-verified", "installed": True,
               "installed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
    _save_state_soft(st, rep)
    rep.ok("diskOS flashed and verified. Power-cycle the device to boot it.")
    return {"ok": True, "debug": d}


def do_restore(params, rep, confirm):
    """Reflash the saved stock-rootfs image (deactivates diskOS; leaves /usr/data files). If no image
    is saved, a firmware .zip must be provided to rebuild it."""
    stock_bin, _ = state.stock_paths()
    if not state.have_stock_image():
        if not params.get("firmware"):
            raise flasher.FlashError(
                "no saved bone-stock image", code="E141",
                action="provide FiiO's firmware .zip so I can rebuild your stock rootfs image")
        work = state.build_dir()
        stock_sq = os.path.join(work, "stock_rootfs.squashfs")
        imagebuild.extract_stock_rootfs(params["firmware"], stock_sq, work, rep=rep)
        imagebuild.validate_stock_rootfs(stock_sq, rep)
        _save_stock(stock_sq, rep)

    # An OLD saved backup (from a prior installer that built 580-block/76 MB images) can be smaller than
    # IMG_SIZE; the 768-block writer needs the full partition-size image, so repad a short one before the
    # exact-size preflight (E121) would otherwise strand it. Then validate + preflight the image that is
    # ACTUALLY written.
    stock_bin = _ensure_full_size(stock_bin, rep)
    # Never restore-flash an unvalidated image: validate whatever is about to be written (the saved
    # copy or the freshly-extracted one) - product/version/known-good-hash, not just size+magic.
    imagebuild.validate_stock_rootfs(stock_bin, rep)
    flasher.preflight(stock_bin, rep)
    summary = {
        "action": "restore-stock",
        "image": stock_bin,
        "duration": "~15 minutes",
        "consequence": "This reflashes bone-stock and removes diskOS. Do not disconnect.",
    }
    if not confirm(summary):
        rep.warning("aborted (device unchanged).")
        return {"ok": False, "aborted": True}

    st = state.load()
    st.update({"phase": "restore-started"})
    _save_state_soft(st, rep)
    d = flasher.flash(stock_bin, log_path=os.path.join(state.state_dir(), "last-restore.log"), rep=rep)
    st.update({"phase": "restore-verified", "installed": False})
    _save_state_soft(st, rep)
    rep.ok("Stock system reflashed. Power-cycle to boot stock.")
    rep.warning("This deactivates diskOS but is not a byte-for-byte factory wipe: the diskOS "
                "files under /usr/data (a separate partition) remain, inert - they do nothing "
                "without the boot hook this reflash removed. (The UI embedded in the diskOS "
                "rootfs is gone, since this reflash overwrote that partition with stock.)")
    return {"ok": True, "debug": d}


def do_remove(params, rep, confirm):
    """Delete everything the tool created on this computer (its full uninstall
    footprint). Nothing was installed system-wide, so this is the whole cleanup."""
    if state.load().get("installed") and not params.get("force"):
        if not confirm({"action": "remove-tool",
                        "consequence": "diskOS still appears to be on the device. This only "
                                       "removes the installer + its saved files from THIS "
                                       "computer (run restore-stock first to clear the device)."}):
            return {"ok": False, "aborted": True}
    d, errors = state.wipe_all()
    if errors:
        rep.error(f"could not fully remove {len(errors)} item(s) under {d}:")
        for p, m in errors[:8]:
            rep.log(f"  {p}: {m}")
        return {"ok": False, "errors": errors, "removed": d}
    rep.ok(f"removed all tool state: {d}")
    return {"ok": True, "removed": d}
