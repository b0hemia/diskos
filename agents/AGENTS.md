# diskOS - working rules for AI coding agents

This file is written for an AI coding agent helping someone hack on **diskOS** - a custom
UI/firmware for the FiiO Snowsky Disc digital audio player (Ingenic X2000 SoC). Drop it in as your
agent's project-instruction file (the `AGENTS.md` convention), or paste it into the session, and
follow it. It encodes the discipline that keeps this project honest and the device un-bricked.

## The one rule that matters most: verify, do not guess

This device's hardware, firmware, audio path, and command surface are **non-obvious and easy
to hallucinate**. Before you assert anything about how the device behaves:

- **Check `docs/HARDWARE.md`** (live-probed hardware capability map) for chip part numbers,
  sample rates, dev nodes, partition layout, I2C addresses.
- **Read the actual binary / source** for what code does - never state it from training priors
  or from earlier in the session.
- If a claim is not in a doc and you cannot verify it against the device or a binary, **say it
  is unverified** rather than stating it confidently.

"Probably", "should be", "likely already" are hallucination flags. Resolve them by reading
first.

## Hardware facts that are easy to get wrong

- Wireless chip is **BCM43438 / AP6212 (2.4 GHz only, BT over UART)**. There is **no 5 GHz**.
  (Old notes saying "BCM4345C5" are wrong - those `.hcd` files are leftovers for other models.)
- **Four CS43131 DACs**, fully balanced (L+/L-/R+/R-), driven by a kernel driver via **ioctl**
  on `/dev/cs43131{,b,c,d}` - **not** via ALSA controls.
- **No physical LED** on the unit. `/sys/class/leds` is empty; `RGB_*` config fields are
  vestigial. Plan no LED features.
- **No GPU / VPU / hardware JPEG / DVFS / thermal sensors.** MSA SIMD is the only accel.
- The rootfs partition (`mtd2`) is a **read-only squashfs**, which is why the boot hook cannot
  be edited in place; persistent state lives in the writable `/usr/data` (`mtd7`).

See `docs/HARDWARE.md` for the full map and the reproducible probe commands.

## Repo layout

- `diskos_installer/` - the Python installer (GUI + CLI) that builds a diskOS image from the
  user's own stock firmware and flashes it over mask-ROM USB.
- `flash/` - the low-level flashing pieces: the mask-ROM writer source (`my_write5.c`), the
  stage-1 SPL, and helper scripts.
- `spl-src/` - GPL corresponding source for the stage-1 DRAM bring-up SPL (patch series over
  upstream U-Boot). See `SPL_SOURCE.md`.
- `src/usbboot/` - the Ingenic mask-ROM USB loader (third-party GPL; do not rewrite).
- `payload/` - the on-device first-boot hook and the diskOS UI binary (`mq_ui`).
- `docs/` - hardware map and other reference docs.
- `licenses/`, `NOTICE.md` - third-party attribution and license texts.

## Device-safety rules (read before touching the device)

- A flash **rewrites the read-only rootfs**. It is **normally recoverable** because the Ingenic
  mask-ROM USB mode lives in on-chip ROM and is reached by a button combo *before* any flashed
  code runs - so a bad flash can usually be re-flashed. This is the safety net, not an A/B slot, and
  recovery is **not guaranteed** on every unit or failure mode (match the README's wording).
- The first-boot install is **fail-closed**: if the UI cannot be verified against the baked
  manifest (SHA-256), the stock UI runs instead. Never weaken that contract.
- **You cannot install from the device's own "System updates" menu** - it checks an ECDSA
  signature against FiiO's public key, which a custom image will not have. Mask-ROM USB is the
  only way in.
- The mask-ROM USB link is **flaky** - it allows roughly one `usbboot` run per power-cycle and
  hangs early ~1 in 3. If a run hangs at the download-to-start transition, power-cycle back
  into mask-ROM and retry rather than fighting it.

## Working habits

- **Honest limitations.** When something is blocked by hardware (no 5 GHz, no GPU, DAC ioctls
  not yet reverse-engineered) or a toolchain gap, say so plainly instead of implying it works.
- **Test before claiming success.** A change to the image builder or the boot hook is not
  "done" until it is validated against a real build / a real device, not just a passing
  typecheck. The installer carries offline validation (independent squashfs extraction, hash
  round-trips) precisely so the expensive on-device flash only tests what it must.
- **Fail closed, log loudly.** Every error path in the installer has a code (`E1xx` preflight,
  `E2xx` build, `E3xx` flash, `F1xx` device-writer). If you add a failure mode, give it a code
  and document it in `README.md`.
- **Cite file:line** when you summarize what changed, so a human can verify it.

## Contributing

- Keep the flashed image = stock FiiO rootfs + the minimal diskOS patch. We do **not**
  redistribute FiiO's rootfs; the installer builds it from the user's own firmware zip.
- GPL components (usbboot, squashfs-tools, the SPL) ship with corresponding source. If you
  touch how they are bundled, keep the source and `NOTICE.md` in sync.
- New firmware versions must be **flash-tested on real hardware** before being declared
  supported - command-tag meanings differ across versions and a wrong command can misbehave.
