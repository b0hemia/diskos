# Changelog

All notable changes to diskOS are documented here.

Entries follow the Keep a Changelog format, with Added, Changed, and Fixed categories where applicable.
diskOS remains beta software; version numbers do not imply broad hardware or feature validation.

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
- Dedicated Books view for single-file `.m4b` audiobooks with saved listening positions.
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

[1.1.1]: https://github.com/b0hemia/diskos/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/b0hemia/diskos/compare/v1.0.0-beta...v1.1.0
[1.0.0-beta]: https://github.com/b0hemia/diskos/releases/tag/v1.0.0-beta
