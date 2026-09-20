# Preview a UI build on your Disc (no reflash)

Try a new `mq_ui` build on the FiiO Snowsky Disc **live, over Wi-Fi, without reflashing**. It runs in
seconds, and it's self-cleaning: a reboot restores the flashed build, so you can experiment freely.

This is for quick UI previews. To make a build **permanent**, flash it with the installer instead.

## Before you start

1. **Your Disc on Wi-Fi**, reachable from your computer on the same network.
2. **Debug Mode enabled** - Settings → System → Debug Mode → Enable. It shows an **SSH password** and
   the device's **IP address**. The password is regenerated each time you enable it and is cleared on
   reboot.
3. An **`mq_ui` binary** to try - built from the [`ui/`](../ui) source (see [`ui/README.md`](../ui/README.md))
   or shared by someone.
4. `sshpass` on your computer (optional - or just type the password when SSH prompts).

## Steps

### 1. Copy the build onto the Disc

Push `mq_ui` over SSH and confirm it arrived intact - the two checksums must match.

```bash
# from the folder holding your mq_ui; replace <ip> and <password>
sshpass -p <password> ssh root@<ip> \
  'cat > /usr/data/mq_ui.new && chmod 755 /usr/data/mq_ui.new && md5sum /usr/data/mq_ui.new' < mq_ui

md5sum mq_ui        # compare - the two hashes must be identical
```

### 2. Open a shell on the Disc

```bash
sshpass -p <password> ssh root@<ip>
```

### 3. Swap it in and restart the UI

Replace the running UI **only** - the music engine keeps going untouched. The restart even replays the
boot animation.

```bash
mv /usr/data/mq_ui.new /usr/data/mq_ui
killall -9 mq_ui
setsid /usr/data/mq_ui </dev/null >/dev/null 2>&1 &

# tidy up: a watchdog may relaunch the STOCK UI beside yours - remove any that isn't yours
sleep 4
for p in $(pidof mq_ui); do [ "$(readlink /proc/$p/exe)" != /usr/data/mq_ui ] && kill -9 $p; done
```

### 4. Look at it

Your build is live on the screen. Tap around, watch the boot ring, judge the change. When you're done
(or if anything looks off), just **reboot the Disc** and the flashed build returns automatically.

## Two things that matter

> [!CAUTION]
> **Never stop the music engine.** Only ever restart `mq_ui`. Killing `mq_player` releases the SD
> card, and the Disc's controller responds by hard-rebooting the device.

> [!NOTE]
> **This is temporary - by design.** On the next reboot, the first-boot installer (`S97diskos_install`)
> verifies `/usr/data/mq_ui` against a SHA-256 manifest baked into the read-only rootfs. Your
> hand-copied build won't match, so it's quarantined and the flashed build is restored. Great for
> previews, fail-closed safe, and nothing to undo.

## Make it permanent

Once a build is a keeper, flash it with the diskOS installer (a ~15-minute reflash). Then it survives
reboots and becomes what the Disc boots into. See the main [`README`](../README.md).
