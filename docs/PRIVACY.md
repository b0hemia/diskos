# diskOS privacy and network disclosure

This lists everything the installer and the diskOS UI (`mq_ui`) send over the network. The UI source
is published under GPL-3.0-or-later in [`ui/`](../ui/); this disclosure is based on that code and on
observed behaviour. It is a good-faith summary, not a guarantee - read the source for the last word.

## The installer (this repo)

- **`./install.sh` uses `pip`** to upgrade `pip` itself and download two packages - `pyusb` and
  `pycryptodome` - from **PyPI** into a local `.venv`. That `pip`/PyPI traffic is the only network
  activity in setup. Point `pip` at your own mirror if you prefer, or pre-install the packages offline.
- **The installer app itself makes no network requests.** Building the image, decrypting/extracting
  your firmware, saving the recovery image, and flashing over USB are all fully local. It does not
  phone home and sends no telemetry.

## The diskOS UI (`mq_ui`) on the device

diskOS is a local music player. The online features below reach out **only when you use them**; each
request unavoidably reveals your device's public IP to the service it contacts.

| Feature | Endpoint | Protocol | What is sent |
|---|---|---|---|
| **Weather** | `wttr.in` | **plain HTTP** | A location string if you set one, else nothing - wttr.in then **auto-geolocates you by IP** (approximate). Over HTTP, so treat it as visible on your network. |
| **Lyrics** | `lrclib.net` | HTTPS | The current track's **title and artist**, to find matching lyrics. |
| **Scrobbling (Last.fm)** | `ws.audioscrobbler.com` | HTTPS | If enabled and connected: the tracks you play (artist/title/album/timestamp) are reported to **your** Last.fm account. |
| **Last.fm sign-in** | `www.last.fm/api/auth/` | HTTPS | Only a QR code containing the approval URL is shown; **your phone**, not the device, opens it. `www.last.fm/api/account/create` is shown as text so you know where to get an API key. |

### Last.fm credential setup happens over your LOCAL network in plaintext

To connect Last.fm, diskOS runs a **temporary web server on the device** at
`http://<device-wifi-ip>:8080/<random-token>/`, shows a QR for it, and your phone posts your Last.fm
**API key and shared secret** to it. Important properties:
- It is **plain HTTP on your Wi-Fi LAN** - your API key/secret cross your local network unencrypted.
  Do this on a network you trust.
- The server is **transient** (runs only during setup), bound to the Wi-Fi interface, and gated by a
  random URL token.
- Your Last.fm **API key, secret, and session key are then stored on the device** under `/usr/data`
  (device config); **offline scrobbles are queued** in `/usr/data/lastfm.queue` until they can be sent.
  Nothing Last.fm-related is sent anywhere except your device, your phone (during setup), and Last.fm.

> **Last.fm is BETA and unverified end-to-end** - see the README's Known Issues. The transport and
> signing are tested, but a full connect-and-scrobble round-trip to a live account has not been.

### The stock FiiO player still runs alongside diskOS

diskOS replaces only the UI; FiiO's original player process still runs underneath it. That stock
component - not diskOS - is responsible for any Bluetooth, firmware-update checks, and LAN media
endpoints (DLNA/AirPlay/Roon/QPlay-style discovery) the device may present on your local network. Those
behaviours are inherited from the stock firmware and are outside diskOS's code.

## What diskOS does NOT do

- No analytics, telemetry, or usage tracking.
- It does not upload your music library, your listening history (beyond Last.fm scrobbles if you turn
  them on), or any personal files.
- If you never enable weather, lyrics, or Last.fm, diskOS itself makes no outbound requests.

## Reporting

Report anything that looks like unexpected data leaving the device via the process in
[`SECURITY.md`](../SECURITY.md).
