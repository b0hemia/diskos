# Changelog

All notable changes to diskOS are documented here.

Entries follow the Keep a Changelog format, with Added, Changed, and Fixed categories where applicable.
diskOS remains beta software; version numbers do not imply broad hardware or feature validation.

## [1.1.3] - 2026-09-24

A safety release. The stock FiiO player, which diskOS runs alongside, tries to unmount the microSD
card and then deletes everything at the card's mount point, without checking that the unmount
worked. If the card is still mounted, this can delete its contents. The flaw is confirmed in the
stock player on V2.09, V2.28, and V2.40. It has not been proven to be the cause of the two reported
card wipes. Every diskOS user should update; updating requires the complete installer and a new flash.

### Added

- A delete guard in the flashed image, at `/bin/rm` and first on the stock player's `PATH` (so the
  stock fallback and watchdog launches are covered too). It refuses `rm` on the card or anything on
  it; the card's folder can only be removed when it is empty and unmounted. It blocks this specific
  deletion path, not every possible way to lose data.
- A boot selector that runs before diskOS starts and decides once per boot between diskOS and stock.
  Any missing or invalid result boots stock.
- Hold Volume Up for about three seconds to force-close an external app. A message says if this is
  unavailable.
- Extra punctuation in the 14, 16, 18, and 20 px interface fonts, and eight more CJK characters in
  the fallback font. This is not full CJK support.

### Changed

- USB Storage now waits for diskOS to finish reading and writing the card, confirms the card really
  was handed over and really came back, and only then resumes artwork caching. A pending handoff
  survives a UI restart.
- USB Storage is refused during a library scan, and whenever diskOS cannot confirm that the card is
  protected or who owns it. If a handoff is left uncertain, diskOS keeps card access off and asks for
  a reboot.
- Library scans, browsing, artwork, lyrics, book details, playlists, and playback commands wait for
  these card checks. If protection is unavailable, diskOS keeps its card features off and says so;
  the stock player still starts.
- On V2.40, cold-boot card handling tries the direct mount before sending an insert event.
- First-boot setup uses only the UI built into the flashed image; the SD-card fallback copy is gone.
- Restart is temporarily removed from Settings until it can wait for card writes to finish.
- Bluetooth starts off when there is no saved diskOS preference. Saved preferences are kept.
  Turning it on from Quick Settings now waits for it to become ready.
- Home, Now Playing, and Quick Settings share the same transport controls. The play/pause icon
  responds at once. Quick Settings skips 15 s back and 30 s forward in audiobooks.
- Quick Settings allows the transport row only with five or fewer tiles enabled.
- Settings values sit below their labels, controls use the current accent colour, and search fits
  the round screen better.
- Debug Mode's description now says SSH over Wi-Fi; the USB serial shell is for developer builds.
- The installer accepts only the unmodified official V2.09, V2.28, and V2.40 firmware for install
  and build. The untested-firmware override no longer applies to them. V2.57 is refused.
- Release packaging requires the boot selector, its probe and helper, and the delete guard, and the
  image check verifies all of them and the patched boot script.

### Fixed

- Stopping card access now cancels a running scan and all artwork work. A cancelled scan keeps the
  previous library.
- Artwork cache files are flushed to the card before they are used (where the card supports it);
  artwork and playlist exports also try to flush their folder.
- Player start-up and the hand-off to stock have firmer time limits, clean up their helper
  processes, and reset signals correctly.
- Default UI's boot flag is written by a time-limited helper that reports failure.
- Bluetooth status and audio-route checks are time-limited: the check is killed along with anything
  it started, and cleanup never waits without a limit. Bluetooth shutdown no longer waits on the
  Bluetooth service. This closes one way the screen could freeze, not every possible one.
- Scan messages now tell apart refused, running, failed, and finished scans, with a count of skipped
  files. Help hints no longer replace an active message.
- Failed playback, seek, and playlist-export requests now report failure.
- The mode picker keeps showing "switching" until a mode change finishes, and clears the checkmark
  if it fails.
- The boot log keeps earlier start-up messages until it reaches its size limit.

## [1.1.2] - 2026-09-20

### Fixed

- Install now works from a plain source checkout that does not include the bundled squashfs tools:
  the installer uses a system `mksquashfs`/`unsquashfs` from `PATH` (for example from the
  `squashfs-tools` package) when the vendored copies are absent. `usbboot` and the device files
  remain bundle-only. Previously a source checkout failed with "bundled tool 'unsquashfs' not found"
  even when squashfs-tools was installed.
- Added a squashfs LZO capability check that runs before the build and in `doctor`, so a squashfs
  tool built without LZO support fails with a clear, actionable message instead of failing partway
  through the build.

### Changed

- `doctor` now labels each native tool as bundled or system, flags broken entries, and reports the
  squashfs LZO check result. Tool-not-found guidance points at the correct per-platform build step
  instead of advising a re-download.

## [1.1.1] - 2026-09-20

### Fixed

- Fixed rootfs extraction on the default case-insensitive macOS filesystem, resolving E230
  `unsquashfs ... already exists` failures from filenames that differ only in case.
  The installer automatically creates, mounts, and verifies a case-sensitive APFS scratch image
  using `hdiutil`, uses it for extraction and validation, then detaches and removes it.
  Cleanup failures are reported.
- Kept the existing build path unchanged on Linux with a case-sensitive filesystem and on
  case-sensitive macOS volumes. Validated on real macOS with `back_home.png` and `BACK_HOME.png`
  coexisting after extraction, and on Linux. macOS device flashing remains unverified end to end.

### Acknowledgements

- Pierre Nel ([@pierrenel](https://github.com/pierrenel)) contributed the macOS
  case-insensitive-filesystem fix ([PR #2](https://github.com/b0hemia/diskos/pull/2)).
- [eudj1n](https://github.com/eudj1n) reported the V2.40 image-size issue
  ([#1](https://github.com/b0hemia/diskos/issues/1)) with a byte-exact stock rootfs reproduction,
  reported the Cyrillic rendering issue ([#3](https://github.com/b0hemia/diskos/issues/3)),
  and built a QEMU preview harness.

## [1.1.0] - 2026-09-20

### Added

- V2.40 firmware support, flash-tested alongside V2.09 and V2.28 on Linux x86-64.
- Album cover flow with horizontal swipe navigation, pre-baked cover sprites, and reflections.
  Dynamic album lists and a bounded sprite cache accommodate libraries with thousands of albums.
- Dedicated Books view for single-file `.m4b` audiobooks with saved listening positions, plus
  chapter navigation from Now Playing (jump to a chapter, current chapter highlighted, with titles
  and durations).
- File/folder browsing on the microSD card, with playback of selected library tracks.
  Playback uses the all-songs queue, not a folder-only queue.
- Bundled Noto Sans glyphs for Latin-extended, Greek, and Cyrillic library text.
- On-device UI source under GPL-3.0-or-later, with vendored LVGL and build instructions.
  Installer, build tooling, and documentation remain MIT licensed.

### Changed

- Increased image and writer capacity from 580 to 768 NAND blocks (96 MiB) to fit V2.40's
  roughly 88 MB stock root filesystem, without changing the partition layout.
- Shortened the conservative flash wait; flashing and verification now take about 15 minutes.
- Separated `.m4b` entries from music views and queues, including migration of existing entries.
- Refreshed the circular listening interface and browsing views.

### Fixed

- Refuse undersized or unrecognized writers before any NAND write, and check reported capacity
  after flashing to detect writer/image mismatches.
- Repad older short saved stock images into a separate copy, then validate them before restore.
- Preserve USER EQ presets changed on the stock player when reselected in diskOS.

## [1.0.0-beta]

### Added

- Initial public release for the FiiO Snowsky Disc, flash-tested with V2.09 and V2.28 on Linux x86-64.
- Circular player interface with local playback, bezel and alphabet navigation, M3U playlist import,
  and album-art dynamic colors.
- Experimental weather and Last.fm integrations.
- Graphical and command-line installer using a local Python environment.
- Local image builds from user-supplied official FiiO firmware archives.
- Mask-ROM USB flashing with a bad-block-aware writer and block verification.
- Saved-stock restore, persistent stock UI selection, one-time stock boot, and stock UI fallback.
- Opt-in SSH Debug Mode and separate public and development installation variants.
- macOS build-from-source path, unverified for release artifacts and device flashing.
- MIT-licensed installer tooling, recovery documentation, hardware notes, and third-party notices.
  The initial release distributed the UI as a binary; UI source publication followed in v1.1.0.

[1.1.3]: https://github.com/b0hemia/diskos/compare/v1.1.2...v1.1.3
[1.1.2]: https://github.com/b0hemia/diskos/compare/v1.1.1...v1.1.2
[1.1.1]: https://github.com/b0hemia/diskos/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/b0hemia/diskos/compare/v1.0.0-beta...v1.1.0
[1.0.0-beta]: https://github.com/b0hemia/diskos/releases/tag/v1.0.0-beta
