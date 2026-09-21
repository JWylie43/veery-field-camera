# Field Workflow — Record, Offload, Calibrate, Stitch

Operational cheat-sheet for the Veery ROCK 5T rig. Two machines are involved:

- **The Rock** (Linux) — recording, browsing takes, copying to the shuttle SSD.
- **Mac / PC** — calibration solving and stitching (`StitchPipeline` lives here).

Commands say which machine they run on. Replace `take_YYYYmmdd_HHMMSS` with your
actual take name.

---

## Quick reference

| Fact | Value |
|---|---|
| Takes live on the Rock at | `/home/radxa/recordings/` |
| Each take produces | `take_TS_cam0.mkv` **and** `take_TS_cam1.mkv` (two files, one per camera) |
| Capture mode | 4K30 (3840×2160 @ 30 fps), HEVC, CBR |
| Bitrate | **28 Mbit/s per camera** → ~12.6 GB/hr each, **~25 GB/hr for the pair** |
| A ~75-min game | ~**32 GB** total |
| Web panel | `http://veery.local:8080` (or `http://<rock-ip>:8080`) |
| Shuttle SSD copies land in | `<drive>/rock-recordings/` |
| Calibration lives in | `calibration/rock-rig/` (cam0/cam1 intrinsics + extrinsics) |
| Stitcher output | HEVC in `.mp4`, tagged `hvc1`, bitrate `auto` (~0.20 bpp) |
| **No audio** | the Rock recorder does not capture audio today |

**Golden rule:** *copy → verify → only then delete.* Never delete a take off the
Rock until the copy is verified.

---

## 1. Record (on the Rock)

### 1a. Web panel — the normal way

```bash
sudo python3 rock5t-camera/recorder/veery_server.py
```

Or let systemd run it at boot (`veery.service`, already configured for
`/home/radxa/veery-field-camera`). Then open `http://veery.local:8080`:

- two live previews (continuous, ISP selfpath, 1080p/5 fps)
- one **Record** button — starts both cameras, writes two MKVs
- **Manage Files** — browse takes, mount/copy to the shuttle SSD, delete

Previews run on the *selfpath* and recording on the *mainpath*, so previews keep
running during a take. Stop sends SIGINT to the process group → GStreamer emits
EOS → a finalized, seekable file.

> Only one process can hold a camera node. Stop the service before running
> `record_dual.sh` by hand: `sudo systemctl stop veery`.

### 1b. Script — for tests and non-default modes

```bash
cd rock5t-camera/recorder
DUR=10 ./record_dual.sh                      # 10-second 4K30 test
W=2028 H=1520 FPS=40 DUR=30 ./record_dual.sh # binned mode
BR=20000000 DUR=60 ./record_dual.sh          # 20 Mbit/cam
```

Dials: `W` `H` `FPS` (must be a driver mode: 4056×3040@10, 3840×2160@30,
2028×1520@40), `DUR`, `BR`, `OUT`, `CODEC` (`h265`|`h264`).

It prints frames-actually-in-the-file vs frames-requested when it finishes —
read that number, it is the honest completion check.

---

## 2. Offload (on the Rock)

The chosen path is a **USB SSD shuttle**: mount the drive from the panel, copy,
eject, carry it to the Mac.

### 2a. From the web panel (preferred)

**Manage Files** → mount the drive → select takes → **Copy to drive**. It runs a
background `rsync` with progress and byte-size verification, then lets you
eject. Copies land in `<drive>/rock-recordings/`. Copy and delete are refused
while recording.

### 2b. By hand

```bash
lsblk -f                                    # find the partition
sudo mount /dev/sda1 /mnt/usb
sudo rsync -avh --progress /home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam*.mkv /mnt/usb/rock-recordings/
```

Verify before deleting — byte counts must match exactly:

```bash
ls -l /home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam0.mkv
ls -l /mnt/usb/rock-recordings/take_YYYYmmdd_HHMMSS_cam0.mkv
```

Bit-perfect check (slower): `sha256sum` both.

Then unmount — **wait for the prompt**, that is the safe-to-unplug signal:

```bash
sync && sudo umount /mnt/usb
```

Only now delete the originals, and check free space:

```bash
rm /home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam*.mkv
df -h /home/radxa
```

> A full drive mid-recording produces an unfinalized, non-seekable MKV. Keep
> headroom.

### 2c. Network alternatives

`smb-share.sh` shares `~/recordings` over SMB so Finder can mount it. It works
but is **not** the chosen path — it topped out at ~63 MB/s, limited by macOS's
SMB client. Kept as a documented fallback; see the script header.

Plain `rsync` over the LAN works too, from the **Mac**:

```bash
rsync -avP radxa@veery.local:'/home/radxa/recordings/take_YYYYmmdd_HHMMSS_cam*.mkv' ~/Desktop/veery-takes/
```

---

## 3. Calibrate

Do this once per housing build, and again any time the mount is disturbed.
Intrinsics are mount-independent; **extrinsics are not**.

### 3a. Capture (on the Rock)

For **intrinsics**, one camera at a time is fine — walk the board around each
camera's whole frame, edges and corners included:

```bash
python3 calibration/calib_server.py     # http://<rock-ip>:8081
```

Preview comes from the selfpath, full-res 3840×2160 snapshots from the
mainpath. Shots land in `~/calib0` / `~/calib1`.

For **extrinsics** you need genuinely **simultaneous pairs** of the same board
pose — hold the board still where *both* cameras see it and run, once per pose:

```bash
./calibration/snap_pair.sh              # → ~/calib/cam0_NNN.png + cam1_NNN.png
```

Aim for ~30–40 poses at varied depth and tilt.

### 3b. Solve (on the Mac)

Pull the shots into the `calibration/` folder, then:

```bash
cd calibration
scp -r radxa@veery.local:~/calib0 images-cam0
scp -r radxa@veery.local:~/calib1 images-cam1
python3 calibrate.py --out rock-rig
```

The model is **fisheye (equidistant), always** — there is no `--model` flag any
more. Board defaults are the measured 7×10 / 78 mm / 58 mm board; if you reprint
at another scale, measure a square and pass `--square-mm` / `--marker-mm`, or the
baseline scale will be wrong.

What good output looks like: **RMS well under 1 px** (this rig solves at
~0.23 px) and ~104.5° horizontal FOV.

To re-solve only the extrinsics against already-good intrinsics:

```bash
python3 calibrate.py --use-intrinsics rock-rig --out rock-rig \
        --cam0-glob 'images-pairs/cam0_*.png' --cam1-glob 'images-pairs/cam1_*.png'
```

> **`calibrate.py` refuses to write `stereo_extrinsics.json` above 3 px RMS.**
> That guard exists because the failure is silent: a bad extrinsic still
> produces a plausible-looking panorama. The usual cause is feeding it the two
> *intrinsics* folders — those are independent per-camera shoots, both numbered
> `img_NNN`, so sort-order pairing matches unrelated frames. Use `snap_pair.sh`
> output. `--force-extrinsics` overrides, but you almost never want that.

Eyeball the two sanity images it writes: straight lines straight in
`camN_undistort_sample.jpg`, and the same feature on the same green line in
`stereo_rectified_sample.jpg`.

---

## 4. Check a take before stitching (on the Mac)

Optional — the stitcher estimates the frame offset itself. Run this when a take
looks wrong:

```bash
python3 stitching/pair_check.py take_YYYYmmdd_HHMMSS_cam0.mkv
```

It reports frame counts, PTS gaps, duplicated frames and the estimated
cam0↔cam1 offset with its correlation margin. The two files should have
**identical** frame counts — the pipeline stamps a rigid 30 fps grid, so a
capture drop shows up as a duplicated frame, not a shorter file.

### Make a take seekable

If a file was not finalized cleanly it has no seek index: it will not scrub, and
parallel stitching falls back to slow frame-grabbing. Remux — fast and lossless:

```bash
ffmpeg -fflags +genpts -i take_TS_cam0.mkv -c copy take_TS_cam0_seekable.mkv
ffmpeg -fflags +genpts -i take_TS_cam1.mkv -c copy take_TS_cam1_seekable.mkv
```

Remux **both** halves and keep the `_cam0`/`_cam1` suffixes so pairing still works.

---

## 5. Stitch (on the Mac / PC)

Build once — `./stitch.command` (Mac, double-click works) or `stitch.bat`
(Windows). Run from `stitching/`.

**Every input is a pair.** Pass the `_cam0` file and `_cam1` is found next to it,
or give both explicitly:

```bash
--source take_TS_cam0.mkv                    # partner found automatically
--source "left.mkv::right.mkv"               # explicit, any names
```

There is no single-file mode; a source that resolves to neither is an error.

### 5a. Interactive tuner (recommended first)

```bash
./stitch.command
```

Opens a browser tuner: **Import source…** → align the far/near edges → **Stitch
all frames** (native save dialog). It also prints the equivalent CLI command, so
you can reproduce a tuned render by hand.

### 5b. Headless

```bash
./build/StitchPipeline --source take_TS_cam0_seekable.mkv \
    --shift-top 4 --shift-bottom 20 --jobs 6 --out-file stitched.mp4
```

| Flag | Meaning |
|---|---|
| `--source <file>` | input — a `_cam0` file, or `"a::b"` |
| `--shift-top N` / `--shift-bottom N` | far/near edge alignment (your tuned values) |
| `--shift-x N` / `--shift-y N` | uniform horizontal / vertical shift of cam1 |
| `--pair-offset N\|auto` | frame offset between the two files (default `auto`) |
| `--crop x,y,w,h` | restrict to a bounding box (full-canvas coords) |
| `--start N` / `--end N` | frame range |
| `--scale F` | render the cylinder at F× radius — same FOV, fewer pixels |
| `--seam N` / `--bands N` / `--no-smart-seam` | seam placement and blending |
| `--no-exposure` | skip matching cam1's brightness to cam0 |
| `--bitrate auto\|90M` | output rate (default `auto`, ~0.20 bpp) |
| `--venc <name>` / `--no-hwenc` | pick or disable the hardware encoder |
| `--jobs N` / `--no-jobs` | parallel processes (default 4) |
| `--calib-dir <dir>` | calibration folder (default: found by walking up) |
| `--out-file <path>` / `--out <dir>` | output |
| `--tune` | open the browser tuner |

### 5c. A time range only

`--start`/`--end` are **frames**. At 30 fps, `frame = seconds × 30`:

```bash
./build/StitchPipeline --source take_TS_cam0_seekable.mkv \
    --start 2700 --end 5400 --out-file clip_1m30-3m.mp4     # 1:30 → 3:00
```

### 5d. Parallel stitch

`--jobs N` splits the range across N processes and `ffmpeg`-concats the parts.
It is only fast on an **indexed** file — point it at the remuxed `_seekable`
pair. Each child logs `seek: indexed jump … (fast)` (good) or
`grab-skipping … (SLOW; remux)` (the input is not indexed).

Tune `N` upward until CPU or GPU hits ~90–100% or VRAM fills, then stop.

### 5e. Panorama size

The output size is **derived from the calibration**, not configured: the
cylinder's radius in pixels equals cam0's focal length, so frame centre is
sampled ~1:1. With this rig's 2104 px focal that is **6774×2371**. Use `--scale`
for fewer pixels at the same field of view, or `--crop` to actually crop.

---

## 6. Verify the output

Confirm no frames were lost — especially comparing a `--jobs` run against
`--no-jobs`:

```bash
ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 stitched.mp4
```

The two counts should match, and land near `fps × seconds`.
