<p align="center">
  <img src="docs/assets/diskos-hero.png" alt="diskOS running on a round digital audio player" width="100%">
</p>

<h1 align="center">diskOS</h1>

<p align="center">
  A purpose-built player interface for the FiiO Snowsky Disc.<br>
  Designed around the round screen, the music library, and the way the hardware wants to be used.
</p>

<p align="center">
  <a href="#install"><img alt="Platform: Linux x86-64" src="https://img.shields.io/badge/platform-Linux%20x86__64-3D424B?style=flat-square"></a>
  <a href="#whats-proven-vs-beta"><img alt="Status: beta" src="https://img.shields.io/badge/status-beta-B99AC8?style=flat-square"></a>
  <a href="LICENSE"><img alt="Installer license: MIT" src="https://img.shields.io/badge/installer-MIT-D77868?style=flat-square"></a>
  <a href="https://ko-fi.com/b0hemia"><img alt="Support diskOS on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support%20diskOS-FF5E5B?style=flat-square&logo=ko-fi&logoColor=white"></a>
</p>

<p align="center">
  <a href="#see-diskos">Screenshots</a> ·
  <a href="#install">Install</a> ·
  <a href="#restore-stock--recover">Restore</a> ·
  <a href="#known-issues">Known issues</a> ·
  <a href="#documentation">Docs</a> ·
  <a href="#contributing">Contribute</a>
</p>

> [!CAUTION]
> **diskOS is an unsupported beta and installation rewrites the Disc's main root filesystem.**
> Power loss, host sleep, a bad cable, a defect, an unsupported device, or an interrupted flash can
> make the player unbootable, lose data, require hardware recovery, or void its warranty. Recovery
> worked on tested units, but is **not guaranteed**. Back up your data and keep the saved stock image
> on separate storage before installing.

diskOS is a custom player UI/firmware for the **FiiO Snowsky Disc** digital audio player
(Ingenic X2000). It replaces the stock interface and uses a source-run installer to build a diskOS
image locally from **your own official FiiO firmware**, then flashes it over the chip's mask-ROM USB
mode. No FiiO root filesystem is distributed by this project.

The installer and build tooling in this repository are open source under the MIT license. The
on-device UI source lives in [`ui/`](ui/) and is licensed separately under **GPL-3.0-or-later** (its built
artifact, `payload/mq_ui`, is what the installer bakes into the image); see [License](#license) for
the boundary.

## At a glance

| | |
|---|---|
| **Device** | FiiO Snowsky Disc |
| **Project status** | Beta; field testing is still limited |
| **Released host** | Linux x86-64 |
| **macOS** | Build path exists; artifact and device flash are unverified |
| **Flash-tested stock firmware** | V2.09, V2.28, and V2.40 |
| **Typical flash time** | about 15 minutes, including verification |
| **Return to stock** | Saved-image restore, temporary stock boot, or stock UI as default |

Not affiliated with or endorsed by FiiO, Snowsky, or Ingenic. No warranty or support is promised.

## See diskOS

<table>
  <tr>
    <td align="center"><img src="docs/assets/home.png" alt="diskOS home screen" width="240"><br><sub>Home</sub></td>
    <td align="center"><img src="docs/assets/now-playing.png" alt="diskOS now playing screen" width="240"><br><sub>Now playing</sub></td>
    <td align="center"><img src="docs/assets/library.png" alt="diskOS music library" width="240"><br><sub>Library</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/assets/alphabet.png" alt="diskOS alphabet navigation" width="240"><br><sub>Fast navigation</sub></td>
    <td align="center"><img src="docs/assets/apps.png" alt="diskOS apps screen" width="240"><br><sub>Apps</sub></td>
    <td align="center"><img src="docs/assets/eq.png" alt="diskOS custom equalizer" width="240"><br><sub>Custom EQ</sub></td>
  </tr>
</table>

<p align="center">
  <img src="docs/assets/boot-animation.gif" alt="Premium diskOS boot animation concept" width="320">
  <br><sub>Premium boot concept: clockwise from the lower-left, matching the real seek-ring geometry.</sub>
</p>

### Built around the Disc

| For listening | For tinkering |
|---|---|
| Circular UI designed for the round display | Graphical and command-line installer |
| Bezel scrolling and alphabet navigation | Image built locally from your stock firmware |
| M3U playlist import | Bad-block-aware writer with block verification |
| Dynamic colors derived from album art | Opt-in SSH debug mode, off by default |
| Local playback as the well-tested path | Hardware map and reverse-engineering notes |
| Weather and Last.fm integrations *(experimental)* | Stock UI fallback and restore path |

## Install

> [!IMPORTANT]
> Read [Requirements](#requirements), [What's proven vs. beta](#whats-proven-vs-beta), and
> [Known issues](#known-issues) before connecting the player. Do not run the installer as root.

### First-time setup

The installer runs with your own **Python 3.8+**. The setup script creates a local virtual
environment and installs two Python dependencies into it; nothing is installed system-wide.

```bash
./install.sh
```

The setup check will tell you if either optional system component is missing:

- **Tk / tkinter:** needed only by the graphical installer. Debian/Ubuntu:
  `sudo apt install python3-tk`
- **libusb-1.0:** used to detect the device in mask-ROM mode. Debian/Ubuntu:
  `sudo apt install libusb-1.0-0`

After setup, run commands through `./diskos-installer`; it selects the local environment for you.

> [!NOTE]
> A source checkout does not include the large host-native flash tools. Build them once using the
> scripts in [For developers](#for-developers). A prepared release bundle places them under
> `vendor/<os>-<arch>/`.

### Graphical installer

1. Run `./diskos-installer gui`.
2. Choose **Install diskOS**, select your official FiiO firmware `.zip`, then select a variant:
   - **Public** *(recommended):* no always-on root shell. SSH can still be enabled temporarily
     from Debug Mode in the UI.
   - **Dev:** adds a passwordless USB-serial root shell on every boot. Use this only on a dedicated
     development device you control.
3. Power the Disc off. Hold **Volume Down** and plug in USB to enter mask-ROM mode. The screen stays
   black; that is expected.
4. Select **Install**, acknowledge the warning, and begin. Do not disconnect the cable or let the
   host sleep during the roughly 15-minute flash.
5. After verification succeeds, power-cycle the device. diskOS is embedded in the flashed image and
   installs on first boot; no microSD installation step is needed.

### Command line

```bash
./install.sh
./diskos-installer doctor
./diskos-installer install --firmware SNOWSKY_DISC_update_*.zip --variant public
```

Useful recovery and cleanup commands:

```bash
./diskos-installer restore-stock
./diskos-installer remove
```

## Requirements

- A **FiiO Snowsky Disc** with supported stock firmware.
- The matching official FiiO firmware `.zip`. You supply this file; the installer decrypts and
  extracts its root filesystem locally.
- A reliable USB cable and about 15 uninterrupted minutes.
- **Python 3.8+** and the dependencies installed by `./install.sh`.
- USB access to mask-ROM device `a108:eaef`.
- **Linux x86-64** for the released path. macOS support remains unverified.

Do **not** run the installer with `sudo`. Its saved recovery image and state belong under your user
account. On Linux, install the included udev rule once instead:

```bash
sudo cp udev/70-diskos-maskrom.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The build, save, and flash then run as one unprivileged process.

### Host support

- **Linux x86-64:** released and tested end to end.
- **macOS, Apple Silicon and Intel:** source is macOS-aware, but there is no verified release
  artifact yet. Build on a Mac with `build/build-macos.sh` after installing libusb with Homebrew.
  Run it from a **case-sensitive volume.** macOS is case-insensitive by default, and the stock
  firmware contains files that differ only in case, so extracting it on a case-insensitive
  filesystem can silently corrupt the built image. Point `DISKOS_INSTALLER_HOME` at a
  case-sensitive volume (create one in Disk Utility, or a case-sensitive APFS sparse image),
  since the build and extraction use that workspace. Treat the entire path as unverified until a
  real-device flash succeeds.

## Restore stock & recover

`restore-stock` reflashes the checksum-verified stock root filesystem reconstructed and saved during
installation. It deactivates diskOS, but it is not a factory wipe; inactive files under `/usr/data`
remain until you remove them.

You can also switch without reflashing:

- **Persistently:** Settings → System → Default UI → Stock
- **Once:** hold **Volume Up** during power-on

If a flash fails, the root filesystem can be left partially written. In tested cases, the device
could be returned to mask-ROM mode by powering off, holding **Volume Down**, and reconnecting USB,
then reflashing diskOS or the saved stock image. Mask-ROM lives in on-chip ROM and is entered before
flashed code runs, but recovery is still **not guaranteed** for every unit or failure.

## What's proven vs. beta

| Status | Area | Current evidence |
|---|---|---|
| ✅ | Flash mechanism and image build | Used on real hardware; writer skips factory bad blocks and verifies every block |
| ✅ | Firmware extraction | Reproduces the stock root filesystem byte for byte from FiiO's archive |
| ✅ | Linux x86-64 | Builds and flashes end to end |
| ⚠️ | macOS | Build recipe exists; artifact and real-device flash are unverified |
| ⚠️ | Firmware coverage | V2.09, V2.28, and V2.40 are flash-tested; other versions are refused by default |

The Disc uses **Winbond W63AH6NKB LPDDR3**, the DRAM initialized by its stock bootloader. If a future
hardware revision uses different DRAM, the writer is designed to fail at memory initialization and
leave the device mask-ROM-recoverable instead of continuing. Wide field testing is still in progress.

## Known issues

- **Firmware coverage:** only V2.09, V2.28, and V2.40 are currently flash-tested. Other command maps can
  differ and are refused by default.
- **Long flash:** a complete write and verification takes about 15 minutes. Most of the time is
  a conservative fixed wait; a faster writer is planned.
- **No on-device update path:** updating diskOS currently requires another flash.
- **microSD after cold boot:** the card mounts a few seconds after startup. If the library initially
  appears empty, reinsert the card once.
- **Lightly tested modes:** USB DAC, Bluetooth receiver, and USB storage have less coverage than
  local playback.
- **USB-serial debug:** the dev variant's CDC-ACM serial shell can be unreliable. Prefer temporary
  SSH over Wi-Fi.
- **Last.fm:** scrobbling is experimental and has not completed a live end-to-end verification.
  Setup transfers your API key over your local network in plaintext HTTP. It is off by default; read
  [`docs/PRIVACY.md`](docs/PRIVACY.md) first.

Found another issue? Open a GitHub issue and include the device's **firmware version** and the exact
on-screen **error code**.

## How it works

```mermaid
flowchart LR
    A[Official FiiO firmware ZIP] --> B[Local extraction and validation]
    C[diskOS UI payload] --> D[Build verified rootfs image]
    B --> D
    D --> E[Ingenic mask-ROM USB]
    E --> F[Bad-block-aware write and verify]
    F --> G[diskOS first boot]
    F -. saved image .-> H[Restore stock]
```

diskOS adds a small hook to the stock `fiio_init.sh` that launches `mq_ui` instead of the stock UI.
The stock root filesystem is read-only squashfs, so enabling the hook requires rewriting partition
`mtd2`. On first boot, the embedded UI is copied to writable storage and checked against a baked
SHA-256 manifest. If validation fails, the stock UI launches instead.

The device's normal update menu checks a signature this project cannot create, so installation uses
mask-ROM USB. See [`docs/HARDWARE.md`](docs/HARDWARE.md) for the partition map and hardware details.

## Documentation

| Document | What it covers |
|---|---|
| [`docs/DESIGN_SYSTEM.md`](docs/DESIGN_SYSTEM.md) | Round-screen tokens, edge-ring geometry, components, accessibility, and boot motion |
| [`docs/PREVIEW_UI_BUILD.md`](docs/PREVIEW_UI_BUILD.md) | Preview an `mq_ui` build on the Disc live over Wi-Fi, no reflash (reverts on reboot) |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Live-probed SoC, four CS43131 DACs, display planes, power, wireless, USB, and hardware gaps |
| [`docs/COMMAND_MAP.md`](docs/COMMAND_MAP.md) | Roughly 221 `mq_player` IPC tags, reply frames, and MCU/SPI commands |
| [`docs/RE_CATALOGUE.md`](docs/RE_CATALOGUE.md) | Reverse-engineering catalogue for `mq_player` and `mq_ui` |
| [`docs/PRIVACY.md`](docs/PRIVACY.md) | Network behavior of the installer and on-device UI |
| [`build/README-vendor.md`](build/README-vendor.md) | Building and packaging portable native flash tools |
| [`SPL_SOURCE.md`](SPL_SOURCE.md) | GPL source and build recipe for the stage-1 DRAM bring-up loader |
| [`DEPENDENCY_INVENTORY.md`](DEPENDENCY_INVENTORY.md) | Optional one-file bundle dependencies and obligations |
| [`NOTICE.md`](NOTICE.md) / [`licenses/`](licenses/) | Third-party component and license mapping |
| [`SECURITY.md`](SECURITY.md) | Private vulnerability reporting |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Safe contribution workflow |
| [`agents/`](agents/) | Project brief for diskOS development |

## For developers

A prepared release bundle includes native host tools under `vendor/<os>-<arch>/`. A fresh source
checkout does not include those large binaries, so build them once:

```bash
bash build/build-usbboot-static.sh
bash build/build-squashfs-static.sh
bash build/build-macos.sh              # run on macOS
```

You can optionally create a self-contained binary. This is not the normal release format and
bundles many system libraries; review [`licenses/THIRD_PARTY_BUNDLED.md`](licenses/THIRD_PARTY_BUNDLED.md)
before redistributing it.

```bash
bash build/build.sh
```

### Building the UI

The on-device UI source is in [`ui/`](ui/), licensed **GPL-3.0-or-later** - fork it, theme it, make
it yours. LVGL 9.2.2 is vendored in `ui/lvgl/` (lightly customized), so all you need is a static
`mipsel-linux-musl` toolchain; see [`ui/README.md`](ui/README.md) for the full recipe.

```bash
cd ui && make    # toolchain as mipsel-linux-musl-gcc; or: make CROSS=/path/to/mipsel-linux-musl-
```

## Debug Mode

<details>
<summary><strong>Optional SSH access and security notes</strong></summary>

Debug Mode is **off by default**. Open **Settings → System → Debug Mode → Enable Debug** to show an
SSH command and a newly generated password. The password rotates each time Debug Mode is enabled.

While enabled, the password is stored in plaintext at `/usr/data/sshd/current_pw` with mode `0600`
so the UI can display it again after a restart. Disabling Debug Mode removes the file. A reboot can
leave a stale copy, but the SSH overlay is inactive until Debug Mode is enabled again.

Debug Mode uses Dropbear 2022.83, which predates the CVE-2023-48795 Terrapin Strict-KEX mitigation.
Use it only for short sessions on a trusted network and turn it off when finished.

</details>

## Error codes

<details>
<summary><strong>Installer and device error reference</strong></summary>

Quote the complete code in bug reports. A stopped flash can leave the root filesystem partially
written even when the writer fails closed.

| Code | Meaning |
|---|---|
| **E1xx** | Environment or preflight; nothing was written |
| E101-E103 | Unsupported host, missing component, or tool cannot run |
| E110-E112 | Mask-ROM device detection or permission problem |
| E120-E142 | Image, firmware ZIP, saved stock image, or state-directory problem |
| **E2xx** | Firmware extraction and image build |
| E201-E224 | Unsafe archive, OTA manifest, decrypt, rootfs, version, payload, or hash problem |
| E230-E250 | Squashfs build, size, validation, symlink, partition, or variant problem |
| **E3xx** | Host-side flashing |
| E301 / E302 | Missing or truncated result; outcome unknown |
| E303 | Flash timed out |
| E310 | Device reported a verification failure |
| **F1xx** | Device writer aborted; return to mask-ROM and reflash or restore |

Process exit codes are `0` success, `1` error, `2` usage or preflight refusal, `3` cancelled, and
`130` interrupted.

</details>

## Contributing

Bug reports, hardware findings, documentation fixes, and carefully scoped patches are welcome.
Read [`CONTRIBUTING.md`](CONTRIBUTING.md) before changing flashing code or security-sensitive paths.
Report vulnerabilities privately according to [`SECURITY.md`](SECURITY.md).

## Support the project

diskOS is a solo open-source hobby project. Tips are optional, but they help keep testing,
reverse-engineering, documentation, and release work moving.

<p align="center">
  <a href="https://ko-fi.com/b0hemia">
    <img alt="Support diskOS on Ko-fi" src="https://img.shields.io/badge/Support%20diskOS%20on%20Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white">
  </a>
</p>

## License

- Original installer code, scripts, and documentation are **MIT** licensed; see [`LICENSE`](LICENSE).
- Third-party components keep their own licenses. GPL/LGPL corresponding source, notices, and
  relinking information are included in [`corresponding-source/`](corresponding-source/),
  [`spl-src/`](spl-src/), [`SPL_SOURCE.md`](SPL_SOURCE.md), [`NOTICE.md`](NOTICE.md), and
  [`licenses/`](licenses/).
- The on-device UI **source** is in [`ui/`](ui/), licensed **GPL-3.0-or-later**
  (see [`ui/COPYING`](ui/COPYING)) - deliberately copyleft so forks stay open. Its built artifact
  ships at `payload/mq_ui`. The UI's bundled components (LVGL, SQLite, fonts) keep their own
  licenses; see [`ui/README.md`](ui/README.md).

> [!WARNING]
> Do not redistribute generated `diskos_*.bin` images. They contain FiiO's root filesystem. Build
> them locally from firmware you obtained from FiiO and share the installer, not the resulting image.
