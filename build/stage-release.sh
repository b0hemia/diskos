#!/bin/bash
# stage-release.sh - assemble the release tarball(s) into build/release/ and generate SHA256SUMS.
#
# The release ships as SOURCE (run with your own Python via ./install.sh) plus the native flash
# tools for the target platform - NOT a bundled onefile binary. Each tarball is self-contained:
# extract it, run ./install.sh once, then ./diskos-installer.
#
# Usage:
#   build/stage-release.sh [TAG ...]      # default TAG: the host's (e.g. linux-x86_64)
# Build the native tools for each TAG first (Linux: build/build-*-static.sh; macOS: build-macos.sh).
set -euo pipefail
cd "$(dirname "$0")/.."                        # installer/
ROOT=$(pwd)
REL="$ROOT/build/release"
mkdir -p "$REL"

# default to the host platform tag if none given
if [ "$#" -eq 0 ]; then
  _os=$(uname -s | tr '[:upper:]' '[:lower:]'); case "$_os" in darwin) _os=macos;; esac
  _arch=$(uname -m); case "$_arch" in amd64) _arch=x86_64;; aarch64|arm64) _arch=arm64;; esac
  set -- "${_os}-${_arch}"
fi

# files/dirs that must NEVER go in a public tarball (secrets, FiiO-derived images, build scratch,
# internal docs). Mirrors .gitignore; kept here so staging works even outside a git checkout.
EXCLUer=(
  --exclude='./.git' --exclude='./.venv' --exclude='./build/venv' --exclude='./build/work'
  --exclude='./build/dist' --exclude='./build/release' --exclude='*/__pycache__/*'
  --exclude='__pycache__' --exclude='*.pyc' --exclude='*.pyo' --exclude='*.bak'
  --exclude='./signing' --exclude='*.pem' --exclude='*.key'
  --exclude='./diskos_dev*.bin' --exclude='./diskos_public*.bin' --exclude='*_recovery.bin'
  --exclude='*.squashfs' --exclude='*.sqfs' --exclude='./_retired_unlicensed'
  --exclude='./flash/qual_lpddr3.sh' --exclude='./flash/scan_check.sh'
  --exclude='./flash/my_write5_scan_dram.bin' --exclude='./flash/usbboot'
  --exclude='./mq_ui' --exclude='./S97diskos_install' --exclude='./S99usbserial'
  --exclude='./INSTALL.md' --exclude='./mkdiskos.sh' --exclude='./flash/flash_diskos.sh'
  --exclude='./flash/extract_stock_rootfs.sh'
  --exclude='./PUBLICATION_PLAN.md' --exclude='./BETA_CHECKLIST.md' --exclude='./RELEASE.md'
  --exclude='./RELEASE_READINESS.md' --exclude='./UPDATE2_DRAFT.md' --exclude='./UPDATE_ARCHITECTURE.md'
  --exclude='./INSTALL_v209_legacy.md' --exclude='./SHA256SUMS' --exclude='*.log' --exclude='*.tmp'
  --exclude='.DS_Store'
)

# native tools + payload that MUST be present for a TAG (fail rather than ship an unusable tarball)
REQ_VENDOR=(usbboot mksquashfs unsquashfs my_write5_dram.bin disc_spl_lpddr3.bin)
REQ_PAYLOAD=(mq_ui S96diskos_select S97diskos_install S99usbserial diskos-debug.sh dropbearmulti diskos-rmguard diskos-selected diskos-bootprobe)

made=()
for TAG in "$@"; do
  echo "== staging $TAG =="
  miss=()
  for f in "${REQ_VENDOR[@]}";  do [ -f "vendor/$TAG/$f" ] || miss+=("vendor/$TAG/$f"); done
  for f in "${REQ_PAYLOAD[@]}"; do [ -f "payload/$f" ]      || miss+=("payload/$f");      done
  if [ "${#miss[@]}" -gt 0 ]; then
    echo "  SKIP $TAG - missing: ${miss[*]}" >&2
    echo "  (build the native tools for $TAG first)" >&2
    continue
  fi

  # Stage into a temp dir named diskos-installer/, keeping ONLY this TAG's vendor dir, then tar it.
  stage=$(mktemp -d)
  dest="$stage/diskos-installer"
  mkdir -p "$dest"
  tar -c "${EXCLUer[@]}" --exclude='./vendor/*' -C "$ROOT" . | tar -x -C "$dest"
  mkdir -p "$dest/vendor/$TAG"
  cp -a "vendor/$TAG/." "$dest/vendor/$TAG/"
  rm -f "$dest/vendor/$TAG/"*.bak 2>/dev/null || true
  # macOS build helper that build/build-macos.sh calls - ship it so that path isn't broken
  [ -f vendor/setup-macos.sh ] && cp -a vendor/setup-macos.sh "$dest/vendor/setup-macos.sh"

  out="$REL/diskos-installer-$TAG.tar.gz"
  tar -czf "$out" -C "$stage" diskos-installer
  rm -rf "$stage"
  echo "  -> $out"
  made+=("diskos-installer-$TAG.tar.gz")
done

[ "${#made[@]}" -gt 0 ] || { echo "nothing staged." >&2; exit 1; }

echo "== generating SHA256SUMS =="
( cd "$REL" && sha256sum "${made[@]}" > SHA256SUMS ) && cat "$REL/SHA256SUMS"

NOTES="$REL/RELEASE_NOTES.md"
[ -f "$NOTES" ] || cat > "$NOTES" <<'EOF'
# diskOS installer - release

Installer for diskOS on the FiiO Snowsky Disc. Runs from source with your own Python (GUI + CLI). It
builds a diskOS image *from your own official FiiO firmware zip* (FiiO's rootfs is never
redistributed), flashes over mask-ROM with a bad-block-aware verify-every-block writer, and keeps a
checksum-verified stock image so **restore/remove is one action**.

## What's in the tarball
Source + the native flash tools for the platform. **No bundled Python** - you run it with your own.

## Setup
```
tar -xzf diskos-installer-<platform>.tar.gz
cd diskos-installer
./install.sh          # builds a local .venv, installs pyusb + pycryptodome
./diskos-installer doctor
```
Needs Python 3.8+. The GUI additionally needs Tk (`apt install python3-tk`); device detection needs
`libusb-1.0` (`apt install libusb-1.0-0`). `install.sh` checks for both and tells you what to install.

## Install / restore / remove
See the bundled `README.md`. Requires putting the device in mask-ROM (power off, hold Vol-Down, plug
USB). ~15 min; normally recoverable via mask-ROM, but not guaranteed.

## Honest status
Enthusiast flasher. Linux flashing is tested on real hardware; see the release notes for the test
status of each firmware version. macOS image builds work; macOS device flashing is unverified.
Not affiliated with FiiO.
EOF
echo "  -> $NOTES"
echo "Done. Attach $REL/* to the GitHub Release."
