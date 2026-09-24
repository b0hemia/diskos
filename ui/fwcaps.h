/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef FWCAPS_H
#define FWCAPS_H
/* Firmware-capability layer.
 *
 * The diskOS installer is version-AGNOSTIC (it gates on the boot-hook structure, not the version
 * number), so ONE diskOS binary may run on top of different stock firmware versions. A few stock
 * IPC command tags moved between firmware generations, so where behaviour differs we detect the
 * stock MAIN_OS_VER at runtime and adapt. Each firmware is still tested on-device before we
 * publish a base built on it.
 *
 * Source of truth = /etc/product_version/version.in (MAIN_OS_VER=NNN), read once and cached. */

/* Stock main-OS version (e.g. 209, 228); 0 if it can't be read. Cached after the first call. */
int fw_os_ver(void);

/* True for firmware generations whose player needs diskOS's runtime LOCALPLAYER init handshake
 * (0666 route + 0657 work-mode, sent once per player generation), and separately for those that
 * need diskOS to mount the SD directly. Both are false for unknown firmware. They are independent
 * capabilities that currently cover the same firmware set (V2.40 and V2.57). */
int fw_needs_localplayer_init(void);
int fw_needs_direct_sd_mount(void);

/* DAC gain-set command tag for the running firmware, or NULL if the firmware is not a verified
 * version (unknown/unreadable) - in which case the caller MUST NOT send a gain command (fail closed;
 * a guessed tag could hit an unrelated handler). V2.09 uses 0645; V2.28 uses 0649 (its 0645 handler
 * is a NULL no-op). Verified by RE of both mq_player dispatch tables. Returns a 4-hex-char prefix. */
const char *fw_gain_tag(void);

#endif /* FWCAPS_H */
