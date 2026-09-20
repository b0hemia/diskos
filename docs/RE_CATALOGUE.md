# Snowsky Disc - mq_player / mq_ui Reverse-Engineering Catalogue

Teardown of the stock V2.09 firmware binaries to document everything the player
and UI are capable of, so diskOS (our LVGL UI) can drive every feature.

- Targets: stock `usr/bin/mq_player` (4126620 B) and `usr/bin/mq_ui` (4418300 B).
  MIPS32 little-endian, **glibc dynamically linked**
  (interp `/lib/ld-linux-mipsn8.so.1`; 28 DT_NEEDED incl libc.so.6/libsqlite3/FFmpeg - NOT
  static musl; only *our* diskOS binary is static-musl).
- Disasm cache (regenerate with `mipsel-linux-gnu-objdump`):
  disassembly, data, and string dumps of the two binaries.
- Verification: addresses below were spot-checked against the cache on 2026-06-25.
  Items marked **(unverified)** have not yet been individually confirmed at the
  instruction level.

---

## ⚑ CORRECTIONS - full verification pass (2026-07)
Handler-level RE of both binaries; corrections spot-checked against the binary and applied inline below.
- **Binaries are glibc dynamic, not static musl** (interp `/lib/ld-linux-mipsn8.so.1`, 28 DT_NEEDED). Affects any replacement-binary / preload work.
- **IPC frame = `TAG(4) + TOTAL_LEN(4hex) + VALUE1(4hex) + optional payload`** - LEN is *total*, not payload length; class-2 cmds add VALUE2. Builder `mq_ui 0x43f82c` (`%s%04X%04X%s`). Replies `%s%04X%04X%s` to `/ui`. Queue `/player` maxmsg=20 msgsize=8192.
- **`06b3` = BT CODEC SELECT** (0=SBC 1=AAC 2/3/4=LDAC; worker 0x40eaf4 → `set_out_dev_sample_array(2,44100,44100)` for SBC/AAC, 96000 for LDAC). **`06b4` = LDAC quality only** (mob/std/high via /usr/data/bt_pipe_recv). ← the two were swapped in our old notes.
- **✅ BT AUDIO OUTPUT NOW WORKS (2026-08-03, live-fixed).** The DAP CAN transmit to a BT speaker (a2dp-source) - it just wasn't exposed and the code is fragile. The crash we hit was NOT the 06b3 codec value and NOT unfinished code: it was **skipping the mandatory `0666000C0006` pre-stop** before `0666000C0002`. Full working route sequence + the `--sbc-quality=medium` (stereo, no stutter) fix → **COMMAND_MAP.md "BT AUDIO OUTPUT" section**. The earlier "SBC fix frame = 06b3000C0000" was wrong/incomplete: diskOS uses `06b3001D0000<MAC>` after the pre-stop + `06c1` init.
- **`06b1`/`06b2`** = BT sample-rate / bit-depth params (not list/query).
- **`0689`** = EQ preset *apply* (21-way engine 0x449544), not a DB-only/media-info op.
- **`0201`** = generic transport toggle (not Roon-specific). **`0657`/`0666`/`06b3` "close_player" = false positive** (shared teardown preamble). `0666`=route (2=BTSRC 4=SPDIF 6=DAC).
- **out_dev** enum has no BTSINK (that's an input/work-mode). 48k→44.1k resample is `libfiio_decoder` over FFmpeg `libswresample` - a stock path, so codec-feasible (CPU headroom still needs an on-device xrun benchmark).
- **IPC-hardening RISK:** stock parser trusts declared LEN and uses `strlen` over the real `mq_receive` byte count → diskOS must emit strictly-correct total lengths and never forward untrusted frames.
- Still UNVERIFIED / deep-RE TODO (AirPlay/DLNA/Roon triggers now RESOLVED for V2.40 - the `0657` mode table, see COMMAND_MAP.md): `0715` runtime volume callback @0x822b14; full `SET_USB_MODE` wire encoding; `0722` OTA worker chain; CS43131 ioctl ABI; the remaining ~190 family-inferred tag meanings.

---

## 1. How mq_player is driven - the command dispatch

mq_player is a background command server. The UI never touches audio; it mails
text commands to a POSIX message queue **`/player`**, and the player mails status
frames back on **`/ui`**.

### Wire format
- Command frame: `TAG(4 ASCII) + LEN(4 ASCII hex) + DATA`.
- Response builder @ **0x488d64** uses format `%s%04X%04X%s`
  (tag, then two 16-bit hex fields, then payload). **(verified: prologue @0x488d64)**

### Dispatch tables (verified structure)
Two arrays of 8-byte `{descriptor_ptr, handler_ptr}` entries:

- **Table A @ 0x7c9d30** - primary handlers. Entry 0 = `{0x64f6e0, 0x411790}`.
  ~131 entries. **(count unverified; structure verified)**
- **Table B @ 0x7ca150** - secondary/extended. Many entries have
  `handler = 0x00000000` = **declared but unimplemented** (e.g. `{0x64fca8, NULL}`).
  ~76 entries. **(count unverified; NULL-handler structure verified)**

Note: 0x7b7870 / 0x7b7c90 (noted in older sessions) are **PLT stubs**, not the
real tables. ~194 tag-like ASCII strings exist in `.str` total; ~30 of the table
slots are NULL / reloc-only / unimplemented (NAS / qplay family). **(unverified)**

A command arrives → tag matched against table → handler thunk → real handler.
Most handlers are tiny getters/setters over an in-memory settings struct.

### Confirmed action commands (tag strings verified present in `.str`)
| Tag  | Action                                   |
|------|------------------------------------------|
| 0100 | play by list (list_type + index)         |
| 0103 | seek                                      |
| 0104 | favorite / unfavorite                     |
| 0201 | transport (play/pause/next/prev)          |
| 0202 | request current state                     |
| 0622 | rescan SD / rebuild DB                     |
| 0657 | source/work-mode transition (NOT play-mode) |
| 0666 | set output route (2=BTSRC 4=SPDIF 6=local-DAC) |
| 0689 | select+APPLY EQ preset (21-way engine 0x449544; NOT DB-only) |
| 0715 | set volume                                |

(0100/0103/0104/0201/0202/0622/0657/0666/0689/0715 all confirmed as ASCII tag
strings in `mq_player.str`. Full 207-tag enumeration is in the raw table dump and
should be transcribed here in a follow-up pass.)

---

## 2. Audio pipeline

Decode → process → output.

### Decode
- **FFmpeg** stack for mp3/flac/wav/aac/aif/m4a/ape/ogg/wma. **(unverified)**
- **libfiio_decoder** for DSD / DFF / DSF / SACD-ISO; DTS. **(unverified)**
- libsndfile present. **(unverified)**

### DSD / DoP
- Modes: NATIVE / DOP / D2P. DoP carrier up to 768k. Roon `configure_*` files in
  `usr/project/config/roon/` corroborate (configure_768_dop, _384_d2p, etc.).
  **(config files verified on disk; mode enum unverified)**

### Output routing (`out_dev`, tag 0666)
LOCAL_ANALOG (CS43131 headphone DAC) / BTSRC / USB_HOST / SPDIF / I2S3.
(BTSINK is NOT an out_dev route - it is an input/work-mode; see §1 and the work-mode enum in §7.)
**(unverified)**

### Volume - set_volume @ 0x489558 **(verified: prologue @0x489558)**
- Applied by shelling out to an amixer control named **`ICODEC HPOUTL GAIN`**
  (string verified @ 0x264dd4 in `.str`).
- Uses inotify to react to external volume changes. **(unverified)**
- On-board MCU over SPI also carries gain/filter/mute opcodes
  (SET_EQ_PARAMETER 0x100B etc.). **(unverified)**

### Bluetooth
- bluealsa-based; source + sink; codecs sbc / aac / ldac. **(unverified)**
- **Stock BT stack = BLUEZ, decoded from `mq_player` strings** (NOT BSA - `bsa_server`/`bt_enable_bsa*.sh`
  are dead code, wrong chip BCM4345C5, no caller; mq_player references bsa_server 0×). Full bring-up embedded
  in mq_player: `brcm_patchram_plus --enable_lpm --enable_hci --no2bytes --tosleep 200000 --baudrate 3000000
  --patchram /lib/firmware/bt_bcm/BCM4343A1_001.002.009.1026.1055.hcd /dev/ttyS0` (downloads the chip fw patch
  over UART → creates working hci0; nothing does this at boot) → `/usr/project/bluetoothd --noplugin=sap
  --plugin=a2dp,avrcp --mode=source` → `bluealsa -S --device=hci0 --profile=a2dp-source --ldac-abr
  --ldac-quality=standard --codec=sbc --initial-volume=48` → `hciconfig hci0 up/piscan/class 0x200414` →
  `bluetoothctl agent on/default-agent/pairable on`. Sink mode = no-LPM patchram + `--mode=sink -p hfp-ag
  --codec=sbc/aac/ldac`. The missing patchram step is why a naive BT enable produces a
  poor scan.

### Gapless
- `audio_track_update_play_gapless`. **(unverified)**

### ReplayGain
- Stored / read from DB. **(unverified)**

---

## 3. EQ - the apply path (the key unlock)

The EQ is a **software parametric biquad cascade inside mq_player**, NOT a
hardware DAC feature. **CORRECTION:** `0689` does NOT merely write
to SQLite - its handler (`0x4961bc`) invokes the **21-way preset engine `0x449544`**,
updates the rate caps and applies the DSP, then replies `a639`. Custom bands are set
separately via `0678` (`0x44a658`). Both are live apply paths, not DB-only.

### DSP engine (verified prologues exist)
- Coefficient calc @ **0x448144** - uses pow/sqrt/sincos to turn
  {filterType, frequency, gain, qValue} into biquad coefficients.
- IIR cascade @ **0x447ed0** and @ **0x4483a4** - per-sample filter, 32-bit
  saturation. **(verified: both are function prologues)**

### Storage - PEQ table (SQL strings verified in `.str`)
Columns: `STYLE_NAME, MASTER_GAIN (REAL), PARAMS_JSON (text), STYLE_PRESET (int)`.
- PARAMS_JSON = array of bands `{filterType, frequency, gain, qValue}` (up to 10)
  + a master gain.
- Confirmed SQL @ ~0x257cb8 region:
  - `INSERT INTO PEQ (STYLE_NAME, MASTER_GAIN, PARAMS_JSON, STYLE_PRESET) VALUES ('%s', %.1f, '%s', %d)`
  - `UPDATE PEQ SET STYLE_NAME = ?, MASTER_GAIN = %.1f, PARAMS_JSON = ? WHERE STYLE_PRESET = ?`
  - `SELECT 1 FROM PEQ WHERE STYLE_PRESET = ? LIMIT 1`

### To apply a custom EQ from diskOS
1. Write/UPDATE the PEQ row (bands JSON + master gain) for a STYLE_PRESET.
2. Send **0689** to select that preset.
The player loads bands → computes coeffs @0x448144 → runs the cascade
@0x447ed0/0x4483a4 on every sample before output.

### ✅ WIRED + CONFIRMED in diskOS (2026-06-30)
Custom EQ is live: `eqcustom.c` (10-band UI, horizontal scroll) → `mdb_set_peq()` writes PEQ
slot 11 in the format below → `ui_apply_eq(11)` sends 0689 → **player applies, sound changes
on-device (user-verified).** `ui_apply_eq` clamp raised 0x0A→20 to reach user slots 11-20.

### CAPTURED PARAMS_JSON format (live, V2.09, 2026-06-30)
Saved a stock "User 1" custom EQ (+12 @ 32 Hz, −12 @ 16 kHz) and read it back. Ground truth:
- **10 fixed bands**: 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz, each ±12.0 dB; plus a **master ±12 dB** = the `MASTER_GAIN` REAL column.
- PARAMS_JSON = JSON **array of 10 objects**, e.g. one band:
  `{"filterType":0,"frequency":32,"position":0,"gain":"12.0","qValue":"0.7"}`
  - `filterType` = 0 (peaking) - **int**; `frequency` Hz - **int**; `position` 0-9 band index - **int**.
  - **`gain` and `qValue` are JSON STRINGS** (quoted: `"12.0"`, `"-12.0"`, `"0.7"`), NOT numbers. gain is one-decimal dB; default qValue `"0.7"`.
- **User slot → STYLE_PRESET**: User 1-10 = **11-20** (User 1 = 11). Built-in presets = 0-10 (off,jazz,rock,r&b,hip-hop,pop,dance,classical,retro,sibilance-atten-1,sibilance-atten-2). Rows 160-169 exist but are unused/placeholder (`PARAMS_JSON` = literal string `"(null)"`).
- Stock writes via `UPDATE PEQ SET ... PARAMS_JSON = ? WHERE STYLE_PRESET = ?` then selects with **0689000C<preset hex>** (User 1 = `0689000C000B`).
- Other stock audio settings seen in the same menu (candidates for diskOS, map to documented name-cmds): Gain H/L, BT codec, SPDIF on/off, DAC digital filter (Fast/Slow LL, Fast/Slow PC, NOS, Wideband FF), DRE on/off.

---

## 4. The `/ui` frames - player → UI

mq_ui opens the **`/ui`** mqueue (name built @0x41b388, recv thread @0x415584,
`mq_receive` wrapper @0x41b5c8). Each frame = `TAG(4) + %x extension + JSON "other"`
(parser @0x415674: `strncpy ...,4` then `sscanf %x`). Dispatch is a
`strncmp(cmd,tag,4)` chain in `ui_ctrl_response` (@0x41bfd0-0x41c098). Recv log
`[UI RECV]: %s` @0x272960. **(IPC path + format instruction-verified)**

Player→UI frames use **`a`-prefixed** tags (replies/reports) + `06d*` (OTA). Tag
meaning is taken from the adjacent log string (string-inferred, high confidence).

| TAG | meaning | payload |
|------|---------|---------|
| a620 | version reply | version `%s` |
| a710 | max-volume reply | `maxVolume` |
| a715 | volume / UAC srate change | `currentVolume` |
| aa1b | UAC sample-rate change | rate; `usbAudio` |
| a704/a705 | wifi connect status | state; `wifi_ssid` |
| a706 | net status | `%d` |
| a615 | language config | `%d` |
| a609 | theme config | `%d` |
| aa27 | charge-protect | `charge_protect` |
| aa24 | sys config | `%d`; `sys_count` |
| a634 | memory-play | `memoryPlay`/`memoryType` |
| a639 | EQ type | preset + `filterType/frequency/gain/qValue` |
| a6b6 | BT paired devices | count + list |
| a6c5/a6c3/a6c2/a6c1/a6c4/a6c0 | BT disc/connect/open replies | state + address |
| aa1c | power-key event | `%d` |
| aa22 | TF-card (SD) insert/remove | event `%d` |
| aa0c/aa0f | screen status | `%X` |
| a60a | tip/toast popup | tip code |
| a644 | now-playing / UI state update | song JSON (below) |
| a622 | SD rescan / DB-rebuild status | scan state |
| 06d0/06d1/06d2 | OTA progress / success / file-count | `%d` |

Verified-present log strings: GET_VERSION_REPLY @0x27255c, GET_WIFI_CONNECT_STATUS
@0x272658, UAC_SRATE_CHANGE @0x27252c.

**Now-playing JSON payload** (fields verified in `.str`): `song_name`, `song_artist_name`
(@0x271b90), `song_album_name`, `song_style_name`, `song_track`, `song_channel`,
`song_duration_time` (@0x271b34), `duration`, `songposition` (@0x271c2c),
`song_sample_rate`, `song_encoding_rate`, `song_bit_rate`, `song_mimetype`,
`song_file_path`, `is_sacd/is_cue/is_dsd`, `love` (favorite), `state` (play state),
`playerflag` (@0x271c08), `curlistlength`, `pos_id`, `playing_num`,
`work_mode` (@0x271b... 0x272b34), `battery` (@0x272c58). Album art → width/height/
quality/rgb → `/usr/data/fiio/cover.png`.

→ For diskOS status indicators (battery/BT/wifi) we read tags a704/a706 (wifi/net),
the BT a6c* family, and `battery` inside a644. **This is the data we were missing.**

### mq_ui feature surface (subsystems)
IPC core (`/ui` in; `/player`,`/bt_control` out) · frame dispatch (cJSON) · local
browse/play (all-songs/album/artist/style/favorite/custom, `db_song_ctrl.c`) ·
now-playing (`class_play_screen.c`, album art `jpg_to_png.c`) · Roon endpoint ·
home/menus (app/system/audio/others/popup `.json`) · EQ (`equalizer.json`) ·
Bluetooth (`bt.json`) · WiFi/NAS (`wpa_supplicant`, `hostapd`, `wl rssi`) ·
system/version/OTA (`ota_update.c`, `set_local_time.c`) · FiiO Link (UDP
224.0.0.255 discovery + TCP control, device id "SNOWSKY DISC") · bundled zlog.

---

## 5. Config + database schema

SQLite 3.23.1, opened via `sqlite3_open_v2`. DB files (paths verified in `.str`):

| File | Holds |
|------|-------|
| `/usr/data/fiio/db/sysconfig.db` | SYSCONFIG (settings, single row ID=1), CUSTOM_THEME, NAS_CONFIG |
| `/usr/data/fiio/db/song.db` | SONG, MY_LOVE, MEMORY_PLAY, PLAY_LIST, LIST_SONG_0/1/2, CUSTOM_PLAYLIST, PLAYLIST_INFO, PEQ |
| `/usr/data/fiio/db/dic.db` | HAN_PINYIN (CODE INTEGER, PINYIN char(1)) - CJK pinyin sort (on-disk verified) |
| `/usr/data/fiio/db/theme.db` | theme assets |
| `/usr/data/fiio/db/ebook.db` | e-book |
| `/usr/data/bluetooth.db` | bt_devices |

Only `dic.db` ships in the rootfs; the rest are created at first boot under
`/usr/data/fiio/db/`. db→file binding is from co-located source names + init block
(high confidence for SONG-family→song.db, SYSCONFIG/theme/NAS→sysconfig.db).

### SONG (song.db) - literal CREATE for MY_LOVE verified verbatim; SONG = same + IS_LOVE
ID(PK) · PATH · NAME · TITLE · ALBUM · ARTIST · GENRE · DISC · TRACK · IS_CUE ·
IS_ISO · IS_DSD · OFFSET(BIGINT) · DURATION(BIGINT) · NAME_CODE · TITLE_CODE ·
ALBUM_CODE · ARTIST_CODE · GENRE_CODE(pinyin sort keys) · ADD_TIME(INT8) ·
SAMPLE_RATE · BIT_PER_SAMPLE · CHANNELS · BIT_RATE · SONG_MIMETYPE ·
SONG_PRODUCTION_YEAR · IS_SELECT · ALBUM_ARTIST · ALBUM_ARTIST_CODE ·
**IS_LOVE** (SONG only, added by upgrade ALTER - favorites flag).
MY_LOVE = same minus IS_LOVE, `UNIQUE(PATH,TRACK)`. **(CREATE verified @mq_ui.str:9793)**

### CUSTOM_PLAYLIST (song.db) - literal CREATE verified @mq_ui.str:9790
MY_LOVE columns + `PLAYLIST_ID` (after ID), `FOREIGN KEY(PLAYLIST_ID) REFERENCES
PLAYLIST_INFO(ID) ON DELETE CASCADE`, `UNIQUE(PLAYLIST_ID,PATH,TRACK)`.

### PLAYLIST_INFO (song.db) - parent of CUSTOM_PLAYLIST. ID(PK) firm; name/count inferred (LOW confidence).

### PLAY_LIST (song.db) - queue registry: ID(PK) · LIST_ID · LIST_NAME.

### LIST_SONG_0/1/2 (song.db) - active play queues, built by `INSERT INTO LIST_SONG_%d ... SELECT ... FROM SONG` (verified). Cols: ID · LIST_ID · POS_ID(=MUSIC_ID join key) · PATH · NAME · TITLE · ALBUM · ARTIST · GENRE · DISC · TRACK · IS_CUE · IS_ISO · OFFSET · DURATION · ADD_TIME · IS_SELECT · SONG_TYPE · ALBUM_ARTIST.

### MEMORY_PLAY (song.db) - resume state, single row. ID(=1) · MUSIC_ID · IS_PLAYING · POSITION · IS_CUE · IS_ISO · TRACK. **(UPDATE verified @mq_player.str:13412)**

### PEQ (song.db) - see §3. ID · STYLE_NAME · MASTER_GAIN(REAL) · PARAMS_JSON · STYLE_PRESET.

### CUSTOM_THEME (sysconfig.db) - ID · POS_ID · NAME · PATH · ALIAS · TYPE · IS_SYSTEM · ATTR · ALPHA · LOCK_TIME/DATE/BATTERY/ID3/MEM_TYPE · FRONT_COLOR.
### NAS_CONFIG (sysconfig.db) - ID · NAS_NAME · NAS_IP · USER · (password) · TYPE · AUTO_LOGIN.
### bt_devices (bluetooth.db) - ID · NAME · ADDR(MAC key) · CODEC · SAMPLING · VOLUME.

### SYSCONFIG (sysconfig.db) - single row, all INT, `UPDATE SYSCONFIG set %s = %d where ID = 1` (verified @0x25a910)
Column names verified from descriptor @mq_player.str:13091+. Runtime pak dump
(verified @0x25e7a4): `DSD_MODE,OUT_DEV,VOLUME,WORK_MODE,LIGHT_LEVEL,LIGTH_ON_TIME
(sic),RGB_LEVEL,MUTE,VOL_MODE,RGB_STATUS,MEM_PLAY`. Full key set (names verified;
**enum value mappings NOT proven** - inferred from name + cross-ref to command tags):

DSD_MODE · OUT_DEV/DEVICE_OUTPUT(→0666) · VOLUME · WORK_MODE · LIGHT_LEVEL ·
LIGTH_ON_TIME(backlight timeout, sic) · RGB_LEVEL · MUTE · VOL_MODE · RGB_STATUS ·
RGB_COLOUR · TRIGGER_IN · SYS_THEME · LANGUAGE · USB_MODE · NETWORK_MODE ·
EQ_TYPE(→0689, =PEQ.STYLE_PRESET) · MAX_VOL · BALANCE_VOL · POWER_SAVE ·
FILTER_TYPE(DAC digital filter) · PLAY_MODE(→0102) · MEMORY_PLAY(resume) ·
FOLDER_JUMP · PLAY_GAP(gapless) · INPUT_MODE · VOL_KNOB_MODE · OTA_CFG · BATTERY ·
BT_CODEC · SCREEN_ROT · PO_PRE_VOL/PO_VOL/PRE_VOL · THEME_MODE · CHARGE_PROTECT ·
LOCK_THEME · TREBLE · BASS · WIFI_STATUS · BT_STATUS · LO_DISABLE · OS_MODE ·
DSD_DECODE · SPDIF · AUTO_TIME · DRE_STATUS · LOCAL_IMG_ANIM · IMG_ANIM_BRIGHTNESS ·
ARM_VERSION · MCU_OTA_FAILED_TIME · KEY_SINGLE/DOUBLE/LONG_CLICK_SLE(gesture→action) ·
ARTIST_CLASS_TYPE · AUDIO_VOLUME_SET.
Config is committed to flash on write (`now try to save config to flash` @0x25a...).

---

## 7. Network streaming + USB modes + COMPLETENESS

**Major gap found:** the earlier sections missed the network-streaming receivers and several subsystems. The device is far more capable than §1-6 implied.

### Work-mode / OUT_DEV enum (the master "audio source" list) - names VERIFIED
mq_player has ONE work-mode enum (pointer-array @ ~0x7bb0a0 data; names in `.str` @0x25f9e0+). Switching mode is **name-driven**: a supervisor `@0x444604` does `strcmp(name,"AIRPLAY"/"DLNA"/"ROON"/...)` and starts/stops that receiver. **Names verified present; integer indices unverified:**
`0 NO_DEFINED · 1 I2S3_OUT · 2 USB_HOST_NULL · 3 NO_WORK_MODE · 4 LOCALPLAYER · 5 BTSINK · 6 ANALOG · 7 UAC(USB-DAC) · 8 DLNA · 9 AIRPLAY · 10 ROON · 11 SPDIF_OPT · 12 SPDIF_RX · 13 DMR · 14 DMC · 15 DMS · 16 MIX · 17 I2S_IN · 18 STREAM_AUDIO` (+ QPLAY). sysconfig keys NETWORK_MODE (cfg+0x5c) and OUT_DEV both feed this.

### AirPlay receiver - BUILT IN (was completely missed)
mq_player embeds a **full shairport-sync fork**: `fiio_airplay.c` + `src/shairport.c, rtsp.c, rtp.c, dacp.c, metadata.c, mdns_avahi.c, player.c` + `libshairplay.so`. Advertises **`_raop._tcp`/`_airplay._tcp`** as **"SNOWSKY DISC"**. Config: `/usr/project/config/airplay2/x2000_airplay2.conf` (Shairport-Sync style, AirPlay 2). Flow: supervisor sees mode "AIRPLAY" → sets `airplay_play=1` (@0x7efbe4) → `start_airplay@0x436008` → pthread `airplay_func@0x435fc0` → shairport loop. Needs **wlan0 up** (`start_switch_network_mode_thread@0x46e634` monitors `/sys/class/net/wlan0/operstate`=="up", then emits `/ui` tag **a706** = GET_NET_STATUS_REPLY - a706 is STATUS, NOT the trigger). Has `airplay_artwork_thread`, DACP remote. Audio → ALSA `snd_pcm_writei` to the DAC after out_dev switch. **TRIGGER COMMAND TAG (SUPERSEDED for V2.40: it is `0657000C000A`, mode 0A = AirPlay, hardware-confirmed - see COMMAND_MAP.md; the AIRPLAY=9 guess below is wrong):** (a `06xx` work-mode-switch carrying AIRPLAY=9, sent via `ui_send_cmd@0x43f82c` → mqueue `/player` idx 3, frame `TAG(4)+ext1(4hex)+len(4hex)+data`). Player also has a name-string command dispatch @0x482328 (e.g. `SET_USB_MODE`). **(SUPERSEDED: the literal tags are now known - AirPlay=`0657000C000A` and the rest of the 0657 mode table, hardware-confirmed on V2.40; see COMMAND_MAP.md.)**

### Also built-in network receivers (missed): 
- **DLNA/UPnP renderer** - `dlna_player.c`, UPnP RenderingControl/AVTransport SCPD. (mode DLNA=8)
- **Roon Ready endpoint** - `roon_player.c` + `libroon.so` RAAT (`roon_transport_*`, volume/seek/shuffle). (mode ROON=10) UI screen `ui_roon_play.c`.
- **QPlay** (Tencent/QQ Music) receiver - `mq_player_server_qplay.c`, MD5(Seed+PSK) auth, SetNetwork/SetLyric.
- **BT SINK** (phone→player A2DP in) - `bt_sink_server.c/bt_sink_control.c` + AVRCP. (mode BTSINK=5)

### USB modes (partially missed)
USB_MODE sysconfig + `usb_mode_change_handler@0x48eb80` (named cmd `SET_USB_MODE`@dispatch 0x482328). Gadgets via configfs: **`uac_demo`** (USB-DAC, functions uac1.a/uac2.a, /dev/usb_dac, UAC_SRATE→/ui tag aa1b) = work-mode UAC(7); **`storage_demo`** (card-reader, mass_storage.0, lun.0/file=/dev/mmcblk0). `set_usb_host`@0x24da24. **USB_MODE integer values (charge/DAC/reader/host) NOT yet mapped.**

### CORRECTIONS to earlier sections
- **NAS is FULLY IMPLEMENTED** (`nas_control.c`: `mount -t cifs vers=3.0`, NFS, `smbclient -L`; NAS_CONFIG live) - §1's "NAS/qplay = NULL/unimplemented" was WRONG.
- **Tag counts:** ~**221** distinct `/player` `0xxx` command tags + ~**102** `axxx` reply tags (mq_ui emits ~161 cmds, handles ~102 replies). §1/§4 documented ~10 + ~30 → **~95% of the command surface is still undocumented** (entire a4xx reply family untouched).
- Other missed subsystems: **lyrics** (`lrc_paser.c`, `/usr/data/fiio/encoder.lrc`!), generic stream/I2S_IN/capture (`stream_audio.c`,`player_capture.c`), animated gif screensaver (LOCAL_IMG_ANIM), serial-number (`sn_nb.c`), image loaders (bmp/jpeg/png/yabmp), SACD/DST internals, MCU OTA (`ENTER_MCU_UPDATE_MODE`), full SPI/MCU handler family.

## 8. FULL command map - inventory complete, meanings mixed-confidence

**V2.09 dispatch tables** (entry = `{tag_str_ptr, handler_thunk_ptr}` 8B; thunks tail-jump via GOT to real handler):
- **Table A @0x7c9d30 = 131 entries** (terminator @0x7ca148). Tags 06xx/07xx/08xx/0a51.
- **Table B @0x7ca150 = 75 entries** (terminator @0x7ca3a8). Tags 00xx/01xx/02xx/04xx/05xx. ~36 in-table NULL (declared-unimplemented), ~11 thunk-but-GOT-null, ~28 live.
- **MCU/SPI name-commands** via hw_ctrl.c @0x48d0b8 + sysconfig-key dispatch @0x488000 (NOT 0x482328 = that's the tag↔enum resolver).

**⚠️ RELIABILITY:** tag list = reliable (mechanical table walk). Per-tag MEANINGS: ~36 string-VERIFIED, ~80 INFERRED-from-family, ~15 UNKNOWN(runtime-registered GOT=0). **Some conflict with the 1.95-era LIVE-tested set** (1.95 tables were @0x7b7870/0x7b7c90; V2.09 layout differs). **RESOLVED:** the `0657`/`0666`/`06b3` "close_player" reading was a false positive - all three call a shared teardown preamble at the start of a disruptive mode change; that teardown is NOT their meaning. `0657`=source/work-mode transition, `0666`=output route (2/4/6), `06b3`=BT codec select. **Treat remaining INFERRED meanings as needing verification; trust live tests + resolved-handler traces over family inference.**

**String-VERIFIED V2.09 commands (Table A):** 0802=song count, 0620=get WIFI_MAC(/usr/data/fiio/nb.txt), 0704=wifi scan list, 0705=wifi connect(psid/passwd), 0800=write wpa_supplicant.conf, 0701=wifi init, 0700=close network card, 0720/0722=OTA(wget ota_patch), 0639/0689/0690/0634/0641=media-info query→a639 reply, 0675/0630=EQ/PEQ get(gain/filterType/frequency), **0678=set PEQ band(filterType/frequency/gain/qValue)**, 0621=DROP DB tables, 0624=mount/storage, 0815=set MEMORY_PLAY position, 0820/0821/0822=key single/double/long action, 0647=gapless, 0648=artist_class_type, **06b1=BT sample-rate param(0x40d4d8), 06b2=BT bit-depth param(0x40d66c), 06b3=BT CODEC SELECT(0=SBC 1=AAC 2/3/4=LDAC; 0x40eaf4; SBC frame `06b3000C0000` pins 44100), 06b4=LDAC QUALITY only(0x40dbe8; mob/std/high)**, 06b7=BT paired, 06c0=BT trust/power, 06c1=BT alias, 06c4=BT disconnect/remove. (0657/0666/06b3 close_player = RESOLVED false positive, see above.)
**Table B verified:** **0100=main PLAY** (jumptable @0x650e2c by list_type), 0201=transport play/pause toggle (generic, →0x419ff4; NOT Roon-specific), 0112=playlist add song, 0115=add to custom list, 0113=love-list delete, 0114=custom-list delete, 0117=playlist reorder(src/dst), 0408=file/folder browse, 0413=album query, 0414=style/genre query, 02xx=NAS scan/browse(mq_player_server_nas.c, many GOT-null/unimplemented).
**MCU/SPI name-commands (hw_ctrl.c, SPI to MCU):** GET/SET_DEVICE_MAX_VOL, SET_DEVICE_VOL, GET/SET_INPUT_MODE, SET_OUTPUT_MODE, SET_GAIN, GET/SET_DAC_FILTER, **SET_USB_MODE**, GET/SET_EQ_PRE, GET/SET_EQ_PARAMETER, SET_EQ_RESET, SAVE_EQ, SET_AUDIO_FORMAT, SET_FACTORY, **ENTER_MCU_UPDATE_MODE**, SET_MCU_POWER, SET_MUTE, SET_POWER_DOWN_TO_MCU (shutdown path, 63 call sites), GET/SET_ZERO_DATA_DETECT_TIME, BT_REPORT_RATE/STATE/CODEC_TO_MCU. sysconfig-key setters: RGB_COLOUR/TRIGGER_IN/SYS_THEME/LANGUAGE/USB_MODE/NETWORK_MODE/EQ_TYPE/MAX_VOL/BALANCE_VOL/POWER_SAVE/FILTER_TYPE/PLAY_MODE/FOLDER_JUMP/PLAY_GAP/INPUT_MODE/VOL_KNOB_MODE/OTA_CFG/PO_PRE_VOL/PO_VOL/PRE_VOL/TREBLE/BASS/LO_DISABLE/OS_MODE/DSD_DECODE.

## 6. TODO (next passes)
- [x] ~~Live-verify 0657/0666~~ RESOLVED (handler-trace): 0657=source/work-mode transition, 0666=output route(2/4/6). "close_player" was a shared teardown preamble.
- [ ] **Pin the AirPlay trigger tag** (mq_ui work-mode menu → callers of ui_send_cmd@0x43f82c) - the one fact needed to enable AirPlay from diskOS.
- [x] **0622 is NOT a working rescan on V2.09:** diskOS "Rescan Library" sends 0622 → runs a long scan
  (~13min, scan_songs_thread) but writes 0 rows to song.db (mtime unchanged). Clearing SONG + reboot also scanned but
  wrote nothing. So song.db must be hand-built (laptop `tools/build_db.py` from sync_manifest.json → SONG +
  PLAYLIST_INFO + CUSTOM_PLAYLIST; diskOS lib sorts by TEXT so *_CODE only affects play-queue order). The real scan
  trigger (comm_scan_songs_thread) is still un-pinned. song.db schema is in this catalogue's table notes / dump from
  song.db.bak. Stock leaves DURATION/SAMPLE_RATE/BIT_RATE/CHANNELS=0 and ALBUM=TITLE for tagless rips - match that.
- [x] **USB_MODE card-reader mode = VALUE 2 - LIVE-VERIFIED WORKING (transferred 32GB this way).** This is the clean
  stock MSC path for file transfer, and it SIDESTEPS the dangerous 0666 entirely. Details:
  - `usb_mode_change_handler` @ **0x48eb??** (log str "usb_mode_change_handler = %d" @vaddr 0x670358, ref'd
    by `addiu v1,v1,856` @0x48eb84). Reads/writes current usb_mode global @ **0x822d20**. Branches on the
    target mode value (s0): observed cases 2, 5, 6.
  - On **usb_mode==2** (`bne s0,2` fall-through @0x48ebc4) it accesses
    **`/sys/kernel/config/usb_gadget/storage_demo`** (lui 0x66 + 13620 = vaddr 0x663534) → builds a
    `mass_storage.0` function, **lun.0/file = `/dev/mmcblk0`** (whole SD), toggles cdrom/nofua/removable/ro,
    binds the UDC. (All configfs paths string-verified @ file 0x263534-0x263d8f.)
  - **CLEAN handoff CONFIRMED:** before/while exporting it UNMOUNTS the SD - `unmount_udisk` @0x668ed4 (called
    ~7× around 0x470134-0x470388) runs `umount %s` with 5 retries + `umount -lf %s 2>/dev/null` lazy-force
    fallback on `/tmp/sdcard` (strs @0x268be8/0x266e40/0x268e40). So it properly releases mq_player's SD hold
    (the EBUSY blocker) - no corruption, unlike a raw composite-gadget export.
  - Trigger: **`SET_USB_MODE`** name-command (str @vaddr 0x66fe40; dispatch refs @0x48232c/0x4863ec/0x48d154;
    builders @0x408044/0x40fa94) and/or the `USB_MODE` sysconfig field. EXACT payload/value-encoding for
    SET_USB_MODE still to pin (one more trace), but the value is **2**.
  - ⚠ CAVEAT (untested): a UDC binds ONE gadget at a time, so binding `storage_demo` almost certainly UNBINDS
    our `serial_demo` ACM → **the serial shell will drop while in reader mode**; exiting reader mode (mode
    switch / replug) should restore it. TEST LIVE WITH USER PRESENT (power-cycle fallback). This is still far
    safer than 0666 (no player crash / MCU-reboot path).
  - ⇒ This unblocks **music-transfer-over-USB** via the stock mechanism: set USB_MODE=2 → laptop sees the SD as
    a USB drive → copy 32GB at USB speed + fsck → exit reader mode → in-device rescan. Replaces the unsafe
    wifi-push (watchdog reboots) and the blocked raw-MSC attempt.
- [ ] Transcribe the full 221-tag command table + 102 a-frame replies.
- [ ] Transcribe the full 207-tag command table (A+B) with handler addresses.
- [ ] Verify audio/decoder/BT/DSD claims (§2) at instruction level (currently unverified).
- [ ] Resolve SYSCONFIG enum value→option integer mappings.
- [ ] Pin PLAYLIST_INFO's full column list (currently only ID firm).
- [ ] diskOS wiring: status indicators from a644/a704/a706/a6c* + custom-EQ via PEQ write + 0689.

## 5b. SYSCONFIG - the master settings table (decoded 2026-06-30)
`/usr/data/fiio/db/sysconfig.db` → table **SYSCONFIG** (single row), one INT column per setting.
This is where every audio/system setting persists (the config-named commands write here).
Columns incl: DSD_MODE, OUT_DEV, VOLUME, WORK_MODE, LIGHT_LEVEL(brightness), LIGTH_ON_TIME,
MUTE, VOL_MODE, USB_MODE, NETWORK_MODE, EQ_TYPE(=0689 preset), MAX_VOL, BALANCE_VOL(channel
balance), POWER_SAVE, FILTER_TYPE(DAC filter), PLAY_MODE(=0102), MEMORY_PLAY, FOLDER_JUMP,
PLAY_GAP, INPUT_MODE, VOL_KNOB_MODE, BT_CODEC, SCREEN_ROT, PO_PRE_VOL/PO_VOL/PRE_VOL,
THEME_MODE, CHARGE_PROTECT, LOCK_THEME, TREBLE, BASS, WIFI_STATUS, BT_STATUS, LO_DISABLE,
OS_MODE, DSD_DECODE, SPDIF, AUTO_TIME, DRE_STATUS, DEVICE_OUTPUT, KEY_SINGLE/DOUBLE/LONG_CLICK_SLE,
ARTIST_CLASS_TYPE, AUDIO_VOLUME_SET.
Live sample (V2.09): FILTER_TYPE=1, DRE_STATUS=1, BALANCE_VOL=0, SPDIF=0, MAX_VOL=120,
BT_CODEC=3, MEMORY_PLAY=0, FOLDER_JUMP=0, PLAY_GAP=0, SCREEN_ROT=0, CHARGE_PROTECT=0,
ARTIST_CLASS_TYPE=0, TREBLE=0, BASS=0, OUT_DEV=6, OS_MODE=0, LO_DISABLE=0, DSD_MODE=1,
INPUT_MODE=1, VOL_MODE=1. (No explicit GAIN column - likely VOL_MODE or encoded in OUT_DEV;
confirm by toggling.) Filter enum (others.json 70-75): 0=FAST_LL,1=SLOW_LL,2=SLOW_PC,3=FAST_PC,
4=NON_OS,5=Wideband_FF. To wire a setting in diskOS: write its SYSCONFIG column + send its apply
command (verified: gapless 0647, BT codec 06b3 [0=SBC...4=LDAC-high], artist 0648, memory 0815, keys 0820-22; Gain/
Filter/DRE/Balance apply-cmds TBD via strace-correlate of stock mq_ui - strace on mq_ui is SAFE,
only strace-on-mq_player triggers the MCU reboot).

## 5c. Audio/DAC apply-commands - DECODED LIVE (2026-06-30, strace of stock mq_ui)
Captured by strace `mq_timedsend` on stock mq_ui while toggling each setting (SAFE - strace
on mq_ui, never mq_player). Frame format `<tag>000C<value 4hex>`. Note: SYSCONFIG column
writes are DEFERRED (didn't update live), so these were correlated by the FRAME, not the DB.
Background-noise tags to ignore: 0807 (recurring 0/1 status), 0704 (wifi scan).
- **DRE** = `0812` - `0812000C0000` = ON, `0812000C0001` = OFF (startup sent 0 when DRE on).
- **Gain** = `0645` - `0645000C0000`/`0001` (Low/High; startup sent 0).
- **Channel balance** = `0713` - `0713000C01<level>`; center = `0101`, nudging streamed 0105..010C
  (one frame per step; 0x01 hi-byte = channel/side, lo-byte = level).
- **DAC Filter** = `0653` - enum = menu order (clean sweep): 0=FAST_LL, 1=SLOW_LL, 2=SLOW_PC, 3=FAST_PC, 4=NON_OS, 5=Wideband_FF.
- **SPDIF** = via output-route `0666` - SPDIF on = `0666000C0004`, analog = `0666000C0006`
  (0666 = OUT_DEV/output route: LOCAL_ANALOG=6, SPDIF=4; mutually exclusive with headphone).
- **BT codec** = `06b3` (**CORRECTED**: mq_ui codec menu 0x464e90 sends 06b3, not 06b4):
  codec-settings menu sends payload-less `06b3000C0000`=SBC `...0001`=AAC `...0002/3/4`=LDAC mob/std/high.
  **But the ROUTE-to-speaker frame (live-captured 2026-08-03) is `06b3<len>000X<MAC>`** (X=codec, MAC payload
  kept for stock frame-shape; worker ignores it). diskOS route uses `06b3001D0000<MAC>` (SBC). `06b4` is
  LDAC-quality only. **Full working BT-transmit sequence → COMMAND_MAP.md "BT AUDIO OUTPUT" section.**
- Also re-confirmed: `0666`=output route, `0642`=USB gadget selector (from startup sync).

## 5d. More settings - DECODED LIVE 2026-06-30 (strace stock mq_ui, clean batch)
- **Channel balance** = `0713` - `0713000C<HHLL>`: center=`0000`; one side `00NN` (NN=step), other side `01NN`. (Same tag the audio-cluster capture saw.) Mixer-style, likely safe.
- **Gapless** = `0647` - `0647000C0001` on / `0000` off. (matches COMMAND_MAP 0647=set gapless)
- **Artist list grouping** = `0648` - `0648000C0000` Artist / `0001` Album-Artist. (ARTIST_CLASS_TYPE)
- **Memory playback (resume)** = `0684` - `0684000C00<0|1|2>` = Off / Position / Song. (0684 was "media getter" in COMMAND_MAP - now confirmed the MEMORY_PLAY setter)
- **Max volume** = `0711` - `0711000C00<NN>` where NN(hex)=cap, 0..0x78(120).
NOTE: only SPDIF (0666 route) wedges on a raw send; these are config/mixer commands, expected safe - but per the wedge lesson, apply on live user change only, don't blind-send at boot.

## 5e. Sibilance EQ presets wired (2026-07-01, code-only, staged not-yet-deployed)
Stock has 11 EQ presets (equalizer.json 0-10); diskOS had 9. Added preset 9="Sibilance 1",
10="Sibilance 2" to OPT_EQ (settings.c) + T_EQ (npmenus.c), nopts 9->11, clamps q>8->q>10.
Uses the existing 0689 path (ui_apply_eq, clamp already 0..20) so 0689000C0009/000A select them.
