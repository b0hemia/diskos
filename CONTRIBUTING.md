# Contributing to diskOS

Thanks for wanting to help. diskOS touches real hardware in a way that can brick a device, so a few
rules keep contributions safe and legally clean.

## Ground rules

1. **Hardware-affecting changes must be tested on real hardware.** Anything that changes the image
   builder, the boot hook, the NAND writer, the SPL, or device targeting has to be flashed and
   booted on an actual Snowsky Disc before it is declared working. Say which firmware version and
   which DRAM/NAND variant you tested on.
2. **Never weaken the fail-closed contract.** The first-boot hook must always fall back to the stock
   UI when it cannot verify the UI against the baked manifest. The flasher must fail closed rather
   than commit an unverified or wrong-device write.
3. **Add an error code for every new failure mode** (`E1xx` preflight, `E2xx` build, `E3xx` flash,
   `F1xx` device-writer) and document it in `README.md`.
4. **Keep the honest-limitations tone.** If something is blocked by hardware or a toolchain gap, say
   so plainly. Do not imply a feature works when it has not been verified.

## Licensing and provenance

- diskOS has two licenses by area: the on-device UI source in [`ui/`](ui/) is **GPL-3.0-or-later**;
  the installer and build tooling (everything else) is **MIT**. By contributing you agree your
  contribution is under the license that already governs the file(s) you change. Please sign off your
  commits (`git commit -s`, Developer Certificate of Origin).
- **Do not upload FiiO firmware, stock rootfs images, or generated `diskos_*.bin` images** to issues
  or PRs - they contain FiiO's software. The installer builds images locally from a user's own
  firmware; that is fine, redistributing them is not.
- If you touch how a GPL/LGPL component is bundled (usbboot, squashfs-tools, the SPL, libusb),
  keep its corresponding source and `NOTICE.md` in sync.
- Do not commit device-identifying data (serials, MAC addresses) or secrets.

## Filing issues

Include: firmware version + hash, DRAM/NAND variant if known, host OS, the exact error code, the
bad-block count if the flasher printed one, and redacted logs. **Do not** post serial numbers, MAC
addresses, firmware zips, stock rootfs, or generated images.

## Development

See [`agents/AGENTS.md`](agents/AGENTS.md) for a project brief (the hardware facts that are easy to
get wrong, the repo layout, and the device-safety rules) - useful whether you work by hand or with
an AI coding agent. Build the installer with `bash build/build.sh`.

For iterating on the on-device UI, see [`docs/DEV_WORKFLOW.md`](docs/DEV_WORKFLOW.md) (build, deploy over
SSH, hot-reload, and the watchdog trap to avoid) and the helper scripts in [`tools/`](tools/) for
deploying a build, capturing screenshots, and driving the touchscreen.
