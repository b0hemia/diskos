# Snowsky Disc V2.09 - mq_player full command map

Dispatch entry = `{tag_str_ptr, handler_thunk_ptr}` (8B); thunks tail-jump via GOT to the real handler.
Confidence: **V**=string-verified · **I**=inferred from family · **U**=unknown (runtime-registered, GOT slot 0).
⚠ = static reading conflicts with our 1.95-era LIVE tests (trust live; V2.09 meanings need live re-verify).

## SOURCE / WORK-MODE SWITCH - 0657 (corrected; supersedes "close player" reading)
`0657` = switch audio SOURCE, payload = integer mode index (jump table @0x6736b0). Frame = `0657000C000<hex>`.
Full mode table, confirmed on V2.40 hardware by zmd22 ([discussion #6](https://github.com/b0hemia/diskos/discussions/6)):

| Frame | Mode | Result |
|---|---|---|
| `0657000C0001` | 01 | USB DAC |
| `0657000C0002` | 02 | Optical out |
| `0657000C0006` | 06 | Bluetooth sink |
| `0657000C0007` | 07 | Bluetooth streaming |
| `0657000C0008` | 08 | **Local playback** (the safe recovery frame; diskOS sends this) |
| `0657000C0009` | 09 | Roon |
| `0657000C000A` | 0A | **AirPlay** (RAOP listener on port 5000) |
| `0657000C000B` | 0B | AirPlay + DLNA (5000 and 33291) |
| `0657000C000C` | 0C | DLNA (49494) |

Key corrections over the earlier guess: `08` is **local playback**, not a network-receiver mode (which is why "mode 8" always returned to normal playback and AirPlay could never be triggered); `09` is **Roon**; `01` is **USB DAC**. The safe local-recovery frame is `0657000C0008`, which diskOS's runtime already sends. The network-receiver modes need no `0642` companion (a plain `0657` switch is enough). `0642` itself is the USB-gadget selector used by the local / USB-DAC / storage modes (see main.c), not a network-receiver control; `NETWORK_MODE` in `SYSCONFIG` stays `0` throughout. Network receivers need WLAN up.
- ✅ **PRE-STOP PINNED + CONFIRMED (2026-08-03, live strace of stock mq_ui + fixed on device):** the pre-stop is **`0666000C0006`** (out_dev=6, local). Stock always sends it BEFORE `0666000C0002` (route to BT). Skipping it = the **g_fiio_local trap** → mq_player SIGSEGV → SD freed → MCU reboot. Mechanism: direct→2 can leave shadow-out=2 with DAC-flag=1 (split state); 6→2 normalizes DAC-flag=0. **This was THE cause of every BT-route reboot.** See "BT AUDIO OUTPUT" section below.
- AirPlay (mode 0A) is confirmed working on V2.40 (receiver discoverable and plays); it needs a normal router, not an iPhone Personal Hotspot (client/mDNS isolation).
NB: table confirmed on V2.40; the 1.95 "0657=play-mode" was a different binary/table. Our earlier play-mode toggle using 0657 was WRONG (it sent source-switch values) - FIXED 2026-06-25 (now uses 0102, below).

## PLAY-MODE - 0102 (GROUND TRUTH, captured from stock UI 2026-06-25)
`0102` = LOCAL play-mode setter. Frame = `0102000C000<v>`. Captured by strace'ing the stock
`/usr/bin/mq_ui`'s `mq_timedsend` while tapping its play-mode control (the loop icon, NP
transport page) - the stock UI cycles these 5 values 0→1→2→3→4→0:
| value | frame | stock icon | mode |
|---|---|---|---|
| 0 | `0102000C0000` | →→ (two arrows) | Sequential (play in order) |
| 1 | `0102000C0001` | ⇄ (crossed)    | Shuffle (random) |
| 2 | `0102000C0002` | ↻ with "1"     | Repeat One (single loop) |
| 3 | `0102000C0003` | ↻ (loop)       | Repeat All (list loop) |
| 4 | `0102000C0004` | "1" + arrow    | Single (play one track, stop) |
NB: this SUPERSEDES the old "0102 = Roon-only no-op" reading - stock uses 0102 for LOCAL
play-mode live. diskOS sends this via ui_set_workmode (main.c). The cycle order above is
the stock order; diskOS's prior 4-mode icons (seq/shuffle/repeat-one/repeat-all) already
match values 0-3.

## LIVE-tested (ground truth)
| tag | meaning |
|---|---|
| 0100 | Play by list (list_type + start index)  ✅ |
| 0102 | **Play-mode** 0102000C000<0..4> (seq/shuffle/rep-one/rep-all/single)  ✅ stock-captured 2026-06-25 |
| 0103 | Seek (ms)  ✅ verified live 2026-06-25 (pos jumped to target) |
| 0104 | Favorite toggle (current song) |
| 0201 | Transport play/pause toggle (generic, VALUE1-driven) - verified NOT Roon-specific |
| 0622 | Rescan SD / rebuild DB |
| 0657 | **SOURCE switch** (NOT play-mode) - see section above |
| 0666 | Output route (→set_out_device 0x461e74): 2=BTSRC 4=SPDIF 6=local-DAC. V2.09-confirmed; the "close_player" sighting was a shared teardown preamble, not this cmd's meaning. |
| 0715 | Volume absolute 0-120  ✅ verified 2026-06-25 (set 20 via 0715000C0014, persisted+displayed) |

## BT AUDIO OUTPUT (transmit to a BT speaker, a2dp-source) - ✅ WORKING (2026-08-03)
Captured from a live strace of stock mq_ui doing a working transmit, then replicated + fixed in diskOS `ui_route_bt()`. **Plays stereo, no stutter, no reboot.** The device IS designed to transmit (not only the "Bluetooth Receiving Mode"/a2dp-sink); stock transmit works but STUTTERS because its default-quality stereo SBC exceeds the X2000 CPU.

Working route-to-BT sequence (diskOS, MAC = the connected speaker, uppercased):
```
0666000C0006   PRE-STOP: switch output to LOCAL first (MANDATORY - skip = g_fiio_local crash → MCU reboot)
0642000C0000   reset USB gadget selector to local/no-export
0657000C0008   work-mode 8
06c1000C0000   start player BT-init thread (06b3 no-ops until this completes; async on cold start)
0666000C0002   route: out_dev = BT source
0657000C0008   work-mode 8 (again, as stock does)
06b3001D0000<MAC>   codec select: 0=SBC (our bluealsa is sbc-only) + MAC payload (stock frame-shape; worker ignores it)
0715000C<vol>  volume
```
Then play normally (`0100...`). Reverse (BT→local) = `ui_route_analog()`: `0666000C0006` then `0657000C0008`.
**No-stutter requires bluealsa `--sbc-quality=medium`** (bit-pool ~33): stock default-quality stutters; medium plays clean stereo (~86% CPU idle on device). Set in `bt.c` bt_enable/bt_ensure_services. `--a2dp-force-mono` also works but is NOT needed (stereo is fine at medium quality).

## Table A @0x7c9d30 - 131 entries (terminator 0x7ca148)
| tag | thunk | meaning | conf |
|---|---|---|---|
| 0802 | 0x411790 | get total song count (SELECT count(*) FROM SONG) | V |
| 0620 | 0x4117b0 | get WIFI MAC (/usr/data/fiio/nb.txt) | V |
| 0710 | 0x4117d0 | network/wifi getter | I |
| 0711 | 0x4117f0 | network/wifi getter | I |
| 0712 | 0x411810 | network/wifi op | I |
| 0713 | 0x411830 | network/wifi op | I |
| 0714 | 0x411850 | network/wifi op | I |
| 0715 | 0x411870 | network/wifi op (NB: vs live 0715=volume - table-A 0715 differs) | I |
| 0716 | 0x411890 | network/wifi op | U |
| 0601 | 0x4118b0 | media getter | U |
| 0651 | 0x4118d0 | media getter/setter | U |
| 0602 | 0x4118f0 | media getter | U |
| 0652 | 0x411910 | media getter/setter | U |
| 0606 | 0x411930 | media DB getter | I |
| 0656 | 0x411950 | media DB getter/setter | I |
| 0603 | 0x411970 | media DB getter | I |
| 0653 | 0x411990 | media DB getter/setter | I |
| 0604 | 0x4119b0 | media DB getter | I |
| 0654 | 0x4119d0 | media DB getter/setter | I |
| 0605 | 0x4119f0 | media query -> reply ([PLAY] send) | V |
| 0655 | 0x411a10 | media DB getter/setter | I |
| 0608 | 0x411a30 | media DB getter | I |
| 0658 | 0x411a50 | media DB getter/setter | I |
| 0611 | 0x411a70 | media DB getter | I |
| 0661 | 0x411a90 | media DB getter/setter | I |
| 0612 | 0x411ab0 | media DB getter | I |
| 0662 | 0x411ad0 | media DB getter/setter | I |
| 0613 | 0x411af0 | media DB getter | I |
| 0663 | 0x411b10 | media DB getter/setter | I |
| 0609 | 0x411b30 | media DB getter | I |
| 0659 | 0x411b50 | media DB getter/setter | I |
| 0615 | 0x411b70 | media DB getter | I |
| 0665 | 0x411b90 | media DB getter/setter | I |
| 0614 | 0x411bb0 | media DB getter | I |
| 0664 | 0x411bd0 | media DB getter/setter | I |
| 0607 | 0x411bf0 | media query -> reply ([PLAY] send) | V |
| 0657 | 0x411c10 | reads "close_player"/killall avahi-publish (⚠ 1.95=play-mode) | V⚠ |
| 0801 | 0x411c30 | playback/system getter | I |
| 0702 | 0x411c50 | wifi/network getter | I |
| 0703 | 0x411c70 | wifi/network getter | I |
| 0704 | 0x411c90 | get wifi scan list | V |
| 0705 | 0x411cb0 | connect wifi (psid/passwd) / SET_POWER_DOWN_TO_MCU | V |
| 0706 | 0x411cd0 | wifi/network op | I |
| 0707 | 0x411cf0 | wifi/network op | U |
| 0708 | 0x411cf0 | wifi/network op | U |
| 0639 | 0x411d30 | media-info query -> reply a639 | V |
| 0689 | 0x411d50 | **EQ PRESET SELECT** - invokes 21-way preset engine 0x449544 (via 0x4961bc), updates rate caps, replies a639. NOT a media-info query. | V |
| 0690 | 0x411d70 | media-info query -> play-send | V |
| 0675 | 0x411d90 | get EQ/PEQ (gain/filterType/frequency/loading) | V |
| 0626 | 0x411db0 | EQ getter (family) | I |
| 0627 | 0x411dd0 | EQ getter | I |
| 0677 | 0x411df0 | EQ getter/setter | I |
| 0628 | 0x411e10 | EQ getter/setter (PEQ) | I |
| 0678 | 0x411e30 | SET PEQ band (filterType/frequency/gain/qValue) | V |
| 0629 | 0x411e50 | EQ getter/setter | I |
| 0630 | 0x411e70 | get EQ (gain/filterType/frequency/loading) | V |
| 0803 | 0x411e90 | playback/system getter | I |
| 0724 | 0x411eb0 | system/OTA op | I |
| 0720 | 0x411ed0 | OTA/network (killall wget, ip route, wlan0) | V |
| 0624 | 0x411ef0 | mount/storage path (mnt/) | V |
| 0622 | 0x411f10 | system getter (NB: live 0622=rescan; table-A differs) | I |
| 0800 | 0x411f30 | write wpa_supplicant.conf (country=%s) | V |
| 0623 | 0x411f50 | system getter | I |
| 0616 | 0x411f70 | media/system getter | I |
| 0a51 | 0x411f90 | extended command (only 0aXX tag) | I |
| 0634 | 0x411fb0 | media-info query -> play-send | V |
| 0684 | 0x411fd0 | media getter | I |
| 0725 | 0x411ff0 | system/OTA op | I |
| 0617 | 0x412010 | media/system getter | I |
| 0667 | 0x412030 | media getter/setter | I |
| 0621 | 0x412050 | DROP DB tables (SONG/MY_LOVE/PLAY_LIST) | V |
| 06a0 | 0x412070 | BT/media getter | I |
| 06a1 | 0x412090 | BT/media getter | I |
| 06a3 | 0x4120b0 | BT/media getter | I |
| 0640 | 0x4120d0 | media getter/setter | I |
| 0644 | 0x4120f0 | media getter/setter | I |
| 0643 | 0x412110 | media op | U |
| 06a2 | 0x412130 | BT/media getter | I |
| 06b1 | 0x412150 | BT sample-rate param (→0x496ac4→change_rate_set_params 0x40d4d8; VALUE1 selects 44100/48000/82000?/96000) | V |
| 06b2 | 0x412170 | BT bit-depth param (→0x496b58→change_format_set_param 0x40d66c; 16/24/32-bit) | V |
| 06b3 | 0x412190 | **BT CODEC SELECT** (→0x496bb8→worker 0x40eaf4): VALUE1 0=SBC 1=AAC 2=LDAC-mob 3=LDAC-std 4=LDAC-high. Stock's connect callback sends `06b3<len>000X<MAC>` (X=persisted BT_CODEC, MAC as ignored-by-worker payload for frame-shape). **diskOS sends `06b3001D0000<MAC>` (X=0 SBC, our bluealsa is `--codec=sbc` only) - value 3 only "works" on an SBC sink via a fragile `/usr/data/bt_codec` fallback, so pick the codec our bluealsa actually enables.** Worker requires `06c1` BT-init done first (else no-ops). | V (live) |
| 06b4 | 0x4121b0 | LDAC **quality** only (→0x496c94→0x40dbe8 via /usr/data/bt_pipe_recv): VALUE1 0=mobile 1=standard 2=high. Does NOT select SBC or set rate. | V |
| 06b6 | 0x4121d0 | BT op | I |
| 06b7 | 0x4121f0 | BT get paired addr+name | V |
| 06b8 | 0x412210 | BT send connected device list | V |
| 06c3 | 0x412230 | BT op | I |
| 06c2 | 0x412250 | BT op | I |
| 06c0 | 0x412270 | BT trust/power (trust/power off/hci down) | V |
| 06c4 | 0x412290 | BT disconnect/remove (bluetooth.db) | V |
| 06c5 | 0x4122b0 | BT op | I |
| 06c1 | 0x4122d0 | BT set device alias | V |
| 0679 | 0x4122f0 | media getter/setter | I |
| 0666 | 0x412310 | reads close_player (⚠ 1.95=output route) | V⚠ |
| 06b5 | 0x412330 | BT op | I |
| 0804 | 0x412350 | playback/system getter | I |
| 0805 | 0x412370 | playback/system op | U |
| 0701 | 0x412390 | wifi init/restart (init_wifi; killall udhcpc/wpa) | V |
| 0700 | 0x4123b0 | close network card (network.c) | V |
| 0808 | 0x4123d0 | playback/system getter | I |
| 0813 | 0x4123f0 | playback/system getter | I |
| 0816 | 0x412410 | playback/system getter | I |
| 0818 | 0x412430 | playback/system getter | I |
| 0817 | 0x412450 | playback/system getter | I |
| 0811 | 0x412470 | playback/system getter | I |
| 0809 | 0x412490 | playback/system getter | I |
| 0815 | 0x4124b0 | set MEMORY_PLAY position (UPDATE ... POSITION) | V |
| 0810 | 0x4124d0 | playback/system getter/setter | I |
| 0812 | 0x4124f0 | playback/system getter/setter | I |
| 0806 | 0x412510 | playback/system getter | I |
| 0645 | 0x412530 | media getter/setter | I |
| 0646 | 0x412550 | media getter/setter | I |
| 0807 | 0x412570 | playback/system getter | I |
| 0660 | 0x412590 | media op | U |
| 0610 | 0x4125b0 | media op | U |
| 0687 | 0x4125d0 | media getter | I |
| 0637 | 0x4125f0 | media op | U |
| 0814 | 0x412610 | playback/system getter/setter | I |
| 0642 | 0x412630 | media getter/setter | I |
| 0641 | 0x412650 | media-info query -> play-send | V |
| 0618 | 0x412670 | media op | U |
| 0668 | 0x412690 | media op | U |
| 0619 | 0x4126b0 | media op | U |
| 0669 | 0x4126d0 | media op | U |
| 0722 | 0x412710 | OTA update (wget ota_patch_user.json) | V |
| 0820 | 0x412730 | set key SINGLE-click action | V |
| 0821 | 0x412750 | set key DOUBLE-click action | V |
| 0822 | 0x412770 | set key LONG-press action | V |
| 06b9 | 0x4126f0 | BT op | U |
| 0647 | 0x412790 | set gapless | V |
| 0648 | 0x4127b0 | set artist_class_type | V |
| 0649 | 0x4127d0 | system-config op | U |

## Table B @0x7ca150 - 75 entries (terminator 0x7ca3a8); many NULL=unimplemented
| tag | handler | meaning | conf |
|---|---|---|---|
| 0425 0435 0445 | NULL | unimplemented | V |
| 0501 | 0x4130f0 | media/thumb type (png/gif/mp4) -> a501/a60a | I |
| 0105 | 0x413110 | emits a102 (status/ack) | I |
| 0202 | 0x413130 | NAS getter/setter | I |
| 0599 | 0x413150 | system/version (-> a599) | I |
| 0401 | 0x413170 | boolean setter -> a401 | I |
| 0411 0450 0451 0452 0453 | NULL | unimplemented (04xx settings) | V |
| 0402 | 0x413190 | boolean setter -> a402 | I |
| 0416 0460 0461 0462 | NULL | unimplemented | V |
| 0403 | 0x4131b0 | boolean setter -> a403 | I |
| 0410 0470 0471 0473 0474 | NULL | unimplemented | V |
| 0404 | 0x4131d0 | setter -> a404 | I |
| 0417 0480 0481 0482 | NULL | unimplemented | V |
| 0405 0418 0419 0420 0421 0422 | NULL | unimplemented (replies declared) | V |
| 0406 | 0x4131f0 | setter -> a406 | I |
| 0426 0407 | NULL | unimplemented | V |
| 0502 | 0x413210 | 05xx misc -> a502 | I |
| 0201 | 0x413230 | Transport play/pause toggle (generic, VALUE1-driven → 0x419ff4) - verified NOT Roon-specific | V |
| 0102 | 0x413250 | playback control (transport) | I |
| 0104 | 0x413270 | playback control (72-byte frame; live: favorite) | I |
| 0111 | 0x413290 (GOT0) | unimplemented stub | V |
| 0112 | 0x4132b0 | playlist: add song (playlist idx, song_path) | V |
| 0115 | 0x4132d0 | add to custom list | V |
| 0113 | 0x4132f0 | love-list delete | V |
| 0114 | 0x413310 | custom-list delete | V |
| 0116 | 0x413330 | playlist op (list mutation) | I |
| 0203 | 0x413350 | NAS op (shares worker w/0412) | I |
| 0412 | 0x413370 | -> reply a412 | I |
| 0413 | 0x413390 | album query (unknown_album) -> a413 | V |
| 0414 | 0x4133b0 | style/genre query (unknown_style) -> a414 | V |
| 0415 | 0x4133d0 | query w/ strcmp -> a415 | I |
| 0465 | 0x4133f0 | -> a465 | I |
| 0495 0496 0497 0498 | NULL | unimplemented | V |
| 0408 | 0x413410 | file/folder browse (mnt/, path build) -> a408 | V |
| 0490 0491 0409 | NULL | unimplemented | V |
| 0117 | 0x413490 | playlist reorder/move (src_list/dst_list) | V |
| 0463 0464 0483 0484 | GOT0 | unimplemented stubs | V |
| 0210 | 0x4134b0 (GOT0) | NAS stub | V |
| 0211 | 0x4134d0 (GOT0) | NAS (json_data, /tmp/nas/) stub | V |
| 0212 0213 0214 | GOT0 | NAS folder op stubs | V |
| 0103 | 0x413430 | song-info/get op | I |
| 0100 | 0x413450 | MAIN PLAY (jumptable @0x650e2c by list_type; unknown_artist) | V |
| 0101 | 0x413470 (GOT0) | unimplemented stub (a101 declared) | V |

## MCU / SPI name-commands (hw_ctrl.c @0x48d0b8, over internal SPI to MCU)
GET_FIRMWARE_VERSION · GET/SET_DEVICE_MAX_VOL · GET/SET_BALANCED_VOL · REPORT/READ_DEVICE_VOL · SET_DEVICE_VOL ·
REPORT_LCD_ACTION · GET/SET_INPUT_MODE · SET_OUTPUT_MODE · SET_GAIN · GET/SET_DAC_FILTER · SET_USB_MODE ·
GET/SET_EQ_PRE · GET/SET_EQ_PARAMETER · SET_EQ_RESET · SAVE_EQ/RESAVE_EQ · SET/REPORT_AUDIO_FORMAT ·
MCU/ARM_REPORT_STATUS · SET_FACTORY · ENTER_MCU_UPDATE_MODE/REPORT_UPDATE_STATUS ·
GET/SET_ZERO_DATA_DETECT_TIME + ZERO_DATA_STATUS · SET_MCU_POWER · SET_MUTE ·
SET_STATUS_TO_MCU/SET_POWER_DOWN_TO_MCU (shutdown, 63 call sites) · BT_REPORT_RATE/STATE/CODEC_TO_MCU

## sysconfig-key setters (system_c... @0x488000)
RGB_COLOUR · TRIGGER_IN · SYS_THEME · LANGUAGE · USB_MODE · NETWORK_MODE · EQ_TYPE · MAX_VOL · BALANCE_VOL ·
POWER_SAVE · FILTER_TYPE · PLAY_MODE · FOLDER_JUMP · PLAY_GAP · INPUT_MODE · VOL_KNOB_MODE · OTA_CFG ·
PO_PRE_VOL · PO_VOL · PRE_VOL · TREBLE · BASS · LO_DISABLE · OS_MODE · DSD_DECODE

## Reply frames
~102 `axxx` player->UI frames. Known: a639=media-info, a706=net status, a714=volume, aa1b=UAC srate, a644=now-playing JSON, a704/a705=wifi status, a6c*=BT. Most undocumented.
