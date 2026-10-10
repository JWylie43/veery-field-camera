# The Rock: setup, network, recording, offload

Everything that runs on the ROCK 5T. Quick start is in the [README](../README.md);
this is the detail.

- [Set up a Rock from the image](#set-up-a-rock-from-the-image)
- [Wi-Fi and hotspot](#wi-fi-and-hotspot)
- [Set up from Radxa's image, or update a running Rock](#set-up-from-radxas-image-or-update-a-running-rock)
- [Tested setup](#tested-setup)
- [Recording](#recording)
- [Offload](#offload)
- [Open questions](#open-questions)

---

## Set up a Rock from the image

1. Download `rock5t-camera-image-N.img.xz` from the newest `image-N` in this repo's
   [Releases](https://github.com/JWylie43/veery-field-camera/releases). Needs a **16 GB or larger** SD card or NVMe drive.
2. Flash it with any image flasher (balenaEtcher, Raspberry Pi Imager), or from
   Terminal: `diskutil unmountDisk /dev/diskN`, then
   `xz -dc rock5t-camera-image-N.img.xz | sudo dd of=/dev/rdiskN bs=4m` (check the
   disk number with `diskutil list` first - it erases that disk). A built-in Mac SD
   reader shows as an *internal* drive, which Etcher hides unless you tick "show
   hidden".
3. When it's done the card's small **`config`** partition shows up on the Mac like a
   USB stick (re-insert the card if it doesn't; if macOS offers to initialise a disk
   it can't read, click *Ignore* - those are the Linux partitions). Fill in
   **`camera-network.txt`** on it - see [Wi-Fi and hotspot](#wi-fi-and-hotspot).
4. Connect the cameras and power on. The first boot takes a few minutes: Radxa's
   first-boot setup creates the `radxa` user (password `radxa` - change it with
   `passwd`), makes the Rock's own SSH keys, grows the system partition to fill the
   drive, and enables SSH when no screen is connected; then the rig's networks are set
   up and the recorder starts.
5. The recorder is at `http://<rock-ip>:8080` (or `http://veery.local:8080`). SSH in
   (`ssh radxa@veery.local`) and confirm everything:

   ```bash
   ~/veery-field-camera/rock5t-camera/setup.sh --check
   ```

After a reflash the Rock has new SSH keys, so your Mac refuses to connect until it
forgets the old ones: `ssh-keygen -R veery.local` (and any other name or IP you use
for it).

The image is Radxa's stock image with the camera kernel, overlay, IQ tuning, 3A fix,
network setup, recorder and a git checkout of this repo (in `~/veery-field-camera`)
already installed. `/etc/camera-rig-image` records what it was built from. How it is
built and published: [rock5t-camera/image-build/](../rock5t-camera/image-build/README.md).

---

## Wi-Fi and hotspot

`camera-network.txt`, on the image's `config` partition:

```
WIFI_NAME="Gilly's Garden"     # a network to join: home, club... (quotes for spaces)
WIFI_PASSWORD="..."
HOTSPOT_NAME="VeeryCam"        # the Rock's own network, for the field (default)
HOTSPOT_PASSWORD="..."         # default changeme123 - change it; blank = an OPEN hotspot
```

The first boot reads it **once** and sets up NetworkManager:

- **Wi-Fi** - only when both `WIFI_NAME` and `WIFI_PASSWORD` are filled in (priority
  100).
- **Hotspot** - whenever `HOTSPOT_NAME` is set (default `VeeryCam`, password
  `changeme123` - the same on every image, so change it): a 2.4 GHz network the Rock
  creates itself, the Rock at `10.43.0.1`. Join it from a phone or laptop and open
  `http://10.43.0.1:8080` (SSH: `ssh radxa@10.43.0.1`). With `HOTSPOT_PASSWORD` blank
  it is **open**: anyone nearby can join and use the recorder panel. Blank
  `HOTSPOT_NAME` = no hotspot.

At **every boot** the Rock scans once: if a known Wi-Fi network is in range it connects
to it, and if that fails (wrong password) it starts the hotspot; if none is in range
it starts the hotspot **right away** - in the field a phone can join ~20 s after
power-on. With no Wi-Fi set up it starts the hotspot straight away (or use Ethernet,
which configures itself).

- **Change the hotspot password** on a running Rock (e.g. if you kept `changeme123`):
  `sudo nmcli connection modify VeeryCam wifi-sec.psk "<new password>"` - it applies the
  next time the hotspot starts.
- The Rock never joins other networks on its own, open or not.
- Networks added later with `sudo nmtui` count as known Wi-Fi too (priority 0 unless
  you set otherwise: `sudo nmcli connection modify "<name>" connection.autoconnect-priority <n>`).
- Once the hotspot is up the radio is busy being an access point, so the Rock looks
  for the Wi-Fi again at the next boot (or: `sudo nmcli connection up "<name>"`).
- After applying, the file becomes `camera-network.applied.txt` with the passwords
  blanked (they're kept on the Rock, not on the card). To apply new settings later,
  put a filled-in `camera-network.txt` back on the `config` partition (on the Rock:
  `/config/`) and reboot, or use `sudo nmtui`.
- What happened at boot: `sudo journalctl -b -u camera-network -u wifi-fallback`.

How it works: [rock5t-camera/system/](../rock5t-camera/system/README.md).

---

## Set up from Radxa's image, or update a running Rock

`rock5t-camera/setup.sh` does on a running Rock what the image has pre-installed. Use
it to **update** a Rock after `git pull` (a new kernel release, tuning, recorder), or
to turn a stock Radxa image into the rig:

```bash
git clone https://github.com/JWylie43/veery-field-camera.git && cd veery-field-camera
rock5t-camera/setup.sh            # installs what's missing, then asks to reboot if needed
rock5t-camera/setup.sh --check    # check only, change nothing
```

It installs, skipping whatever is already done:

1. **packages** - GStreamer, ffmpeg and tools (apt);
2. **the camera kernel** - Radxa's kernel with the IMX477 driver built in (stock
   kernels have none), from this repo's `kernel-N` release; installed beside the stock
   kernel, which stays in the boot menu as a fallback; kernel packages are held so a
   Radxa update can't replace it;
3. **the camera overlay** - tells the board both cameras are attached and which one
   leads the genlock;
4. **the IQ tuning file** the 3A daemon (auto exposure / colour) loads;
5. **a fix for Radxa's `rkaiq_3A.service`**, which as shipped kills the 3A daemon right
   after starting it; and the services that apply `camera-network.txt` (not the file
   itself - copy `rock5t-camera/system/camera-network.txt` to `/config/`, fill it in
   and reboot to use it on a Rock set up this way);
6. **the recorder service** - the web panel, started at every boot (only restarted if
   it changed, so a take in progress isn't cut).

Then it asks to reboot if the kernel, overlay or tuning changed, and otherwise runs its
checks: kernel and boot default, Radxa's camera software versions, tools and GStreamer
elements (including the Rockchip hardware encoder), driver and both sensors, the two
ISP nodes, the 3A daemon, the recorder, storage. `--yes` reboots without asking. It
never flashes anything, and doesn't set the hostname (this rig's is `veery`, hence
`veery.local`).

---

## Tested setup

The image is built on exactly this. Setting up from Radxa's image, flash the same one:
the camera kernel is built from that image's kernel source, Radxa's camera software (3A
daemon, hardware encoder) has to match it, and the IQ file is written for that rkaiq
version's format. `setup.sh --check` warns if a Rock's camera software differs.

| | Version |
|---|---|
| Radxa OS image | ROCK 5T Debian 12 (bookworm) KDE, release `rsdk-r7` (2026-07-06) - mirrored unmodified as this repo's release [`radxa-rsdk-r7`](https://github.com/JWylie43/veery-field-camera/releases/tag/radxa-rsdk-r7) (Radxa's original: [radxa-build/rock-5t](https://github.com/radxa-build/rock-5t/releases/tag/rsdk-r7)) |
| Kernel | `6.1.84-8-rk2410-imx477` (release [`kernel-1`](https://github.com/JWylie43/veery-field-camera/releases/tag/kernel-1)), from Radxa's `6.1.84-8-rk2410` |
| 3A daemon / ISP tuning | `camera-engine-rkaiq` 6.8.0-rk3588 |
| Hardware encoder | `librockchip-mpp1` 1.5.0-1, `gstreamer1.0-rockchip1` 1.14-4, `libv4l-rkmpp` 1.7.0-1 |
| Boot | `u-boot-menu` 4.2.2, `u-boot-rock-5t` 2017.09-64-455bd2a, `rsetup` 0.4.27 |

The recorder needs no pip packages - `recorder/server.py` is Python 3 standard library
only, on purpose. The hardware bring-up (driver, overlay, kernel, tuning) is recorded in
[rock5t-camera/ROCK5T_CAMERA.md](../rock5t-camera/ROCK5T_CAMERA.md).

---

## Recording

The recorder starts by itself at every boot (`recorder.service`). Its panel:

- two live previews (continuous, ISP selfpath, 1080p/5 fps)
- one **Record** button - starts both cameras, writes two MKVs (`take_TS_cam0.mkv` +
  `take_TS_cam1.mkv` in `~/recordings`)
- **Manage Files** - browse takes, mount/copy to the shuttle SSD, delete
- `/calib` - calibration snapshots (see [calibration.md](calibration.md))

Both cameras are recorded by ONE GStreamer pipeline (`rkisp mainpath → videorate →
mpph265enc → matroskamux`): one clock, so genlocked frames carry the same timestamps in
both files, each file keeps its first frame's real start time, and both are tagged
`shared-clock` - Studio and the stitcher read the frame offset between them exactly.
The trade-off, chosen deliberately: a fault in one camera ends the whole take. Previews
run on the *selfpath* and recording on the *mainpath*, so previews keep running during
a take; Stop sends SIGINT → GStreamer EOS → a finalized, seekable file.

The sensor mode and bitrate are **fixed** - 4K30 (3840×2160), HEVC CBR, 28 Mbit/s per
camera (~25 GB/hr for the pair, ~32 GB for a 75-minute game). No audio, by design.

**The clock.** The Rock has no clock battery: powered off it forgets the time, and at
boot it restarts from the last time it saved (its last internet sync or clean shutdown).
At home it then corrects itself from the internet. In the field there is none, so the
recorder panel fixes it: opening the panel on a phone (or any device) sends that
device's time, and the Rock takes it - only when it hasn't synced from the internet,
isn't recording, and is more than 2 s off. The panel then shows "Rock clock set from this
device". So: **open the panel before the first take** and the take names get the right
date and time.

Only one process can hold a camera node: to run anything else against the cameras, stop
the recorder first - `sudo systemctl stop recorder` (`start` to bring it back; logs:
`journalctl -u recorder -f`). By hand: `sudo python3 rock5t-camera/recorder/server.py`.

---

## Offload

**From the panel (preferred):** plug the shuttle SSD into a **blue USB 3 port** →
**Manage Files** → mount the drive → select takes → **Copy to drive**. It runs `rsync`
with progress and byte-size verification, then lets you eject. Copies land in
`<drive>/rock-recordings/`. Copy and delete are refused while recording.

**Over the network**, from the Mac:

```bash
rsync -avP radxa@veery.local:'/home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam*.mkv' ~/Desktop/takes/
```

**By hand on the Rock:**

```bash
lsblk -f                                    # find the partition
sudo mount /dev/sda1 /mnt/usb
sudo rsync -avh --progress /home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam*.mkv /mnt/usb/rock-recordings/
ls -l /home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam0.mkv /mnt/usb/rock-recordings/take_YYYYmmdd_HHMMSS_cam0.mkv   # sizes must match
sync && sudo umount /mnt/usb                # wait for the prompt: then it's safe to unplug
```

**Golden rule: copy → verify → only then delete.** A full drive mid-recording produces
an unfinalized, non-seekable MKV - keep headroom (`df -h /home/radxa`).

---

## Open questions

**Capture bitrate looks low.** 28 Mbit/s per camera at 4K30 is ~0.11 bits/pixel. The
stitcher's own target for its HEVC *output* is ~0.20 bpp (from VMAF runs: good H.264 at
~0.26 bpp, HEVC ~40% better), so the capture master - de-warped, stitched and
re-encoded afterwards - runs at about half that, on high-entropy content (grass,
motion). Matching 0.20 bpp would be ~50 Mbit/cam (~45 GB/hr for the pair). Worth an A/B
on real footage before a real game.
