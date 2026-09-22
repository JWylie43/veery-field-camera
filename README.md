# Veery — ROCK 5T Stereo Field Camera

An end-to-end field rig for a dual-camera stereo head on a **Radxa ROCK 5T**:
**record** two genlocked 4K streams, **calibrate** the pair, and **stitch** the
result into a single cylindrical panorama.

Built around two Raspberry Pi HQ (**Sony IMX477**) sensors with 110° CIL391
fisheye lenses in a 3D-printed housing, recording **4K30 per camera** to the
RK3588's hardware HEVC encoder — so the CPU stays near-idle and the rig has the
thermal headroom to run outdoors for hours.

> This is the `radxa-rock-5t` branch. The earlier Jetson Orin Nano rig lives on
> [`nvidia-orin-jetson-nano`](../../tree/nvidia-orin-jetson-nano); none of its
> code is on this branch.

**[ROCK5T_CAMERA.md](ROCK5T_CAMERA.md)** is the hardware bring-up log — what was
tried, what failed and why. Read it before touching the driver, the overlay or
the ISP tuning. This file is everything else: what the project is, and how to
run it.

---

## Contents

- [Project layout](#project-layout)
- [How the pieces fit](#how-the-pieces-fit)
- [The two facts that bite](#the-two-facts-that-bite)
- [Status](#status)
- [Prerequisites](#prerequisites)
- [Quick reference](#quick-reference)
- [1. Record](#1-record-on-the-rock)
- [2. Offload](#2-offload-on-the-rock)
- [3. Calibrate](#3-calibrate)
- [4. Check a take](#4-check-a-take-before-stitching-on-the-mac)
- [5. Stitch](#5-stitch-on-the-mac--pc)
- [6. Verify the output](#6-verify-the-output)

---

## Project layout

```
veery-field-camera/
├── rock5t-camera/        Runs ON THE ROCK — driver, device tree, tuning, recorder
│   ├── driver/             IMX477 kernel driver (built into a custom kernel, =y)
│   ├── overlay/            dual-camera device-tree overlay
│   ├── kernel-build/       kernel build script + notes
│   ├── iqfiles/            rkaiq ISP tuning (generated; → /etc/iqfiles/)
│   ├── reference/          vendor DTS / drivers / schematic used as source material
│   └── recorder/           veery_server.py (web panel), veery.service, ae_follower.py
├── calibration/          Runs ON YOUR MAC — ChArUco stereo calibration
│   ├── calib_server.py     Rock-side capture panel (preview + full-res snapshot)
│   ├── snap_pair.sh        Rock-side: capture ONE simultaneous cam0+cam1 pair
│   ├── show_board.py       generate/display the ChArUco board
│   ├── calibrate.py        cam0+cam1 fisheye intrinsics + stereo extrinsics
│   └── cam0_intrinsics.json, cam1_intrinsics.json, stereo_extrinsics.json
├── stitching/            Runs ON YOUR MAC/PC — panorama stitcher (C++)
│   ├── stitch_pipeline.cpp   calibration-driven cylindrical stitch + browser tuner
│   ├── pair_check.py         verify a take's two files before stitching
│   └── CMakeLists.txt, include/json.hpp
├── 3d-housing-model/     Printable enclosure (Rock Housing top/bottom, .3mf + .stl)
├── setup.sh              Rock dependency + pipeline health check
└── ROCK5T_CAMERA.md      Hardware bring-up log
```

The three calibration JSONs live directly in `calibration/` because that is
where the stitcher looks by default — no `--calib-dir` needed.

---

## How the pieces fit

**1 — Record (on the Rock).** Two IMX477s, genlocked over XVS, each written to
its own MKV by its own GStreamer pipeline:

```
rkisp mainpath (NV12, tuned IQ) → mpph265enc (CBR) → matroskamux → take_TS_camN.mkv
```

Two **independent** files is deliberate: fault isolation, full per-camera
timestamps, no live cross-coupling. Pairing happens later, at stitch time.

**2 — Calibrate (capture on the Rock, solve on the Mac).** `calib_server.py` or
`snap_pair.sh` captures full-res ChArUco frames; `calibrate.py` solves per-camera
**fisheye** intrinsics and the stereo extrinsics into `calibration/`.

**3 — Stitch (on the Mac/PC).** `StitchPipeline` reads that calibration and warps
both cameras onto one cylinder. No feature detection — the alignment comes
entirely from the calibrated geometry.

---

## The two facts that bite

**Every input is a pair.** The rig writes one file per camera and the stitcher
has no single-file mode. Pass the `_cam0` file and its `_cam1` partner is found
next to it, or give both as `"cam0path::cam1path"`.

**The calibration is fisheye, always.** The CIL391 lenses are 110° with -16%
barrel. A pinhole+polynomial fit leaves a uniform ~2.6 px residual — a model
mismatch, not noise; the equidistant model lands at ~0.23 px. `calibrate.py`
writes `"model": "fisheye"` and the stitcher **rejects** anything else, because
the wrong projection doesn't fail loudly — it silently yields a plausible-looking
panorama built from the wrong geometry.

---

## Status

| Piece | State |
|---|---|
| IMX477 driver + dual-camera overlay | working on kernel 6.1.84-8-rk2410-imx477 |
| ISP tuning (`imx477_RPI-HQ_default.json`) | tuned; see `iqfiles/TRANSLATION_NOTES.md` |
| Dual 4K30 HEVC recording | working (`veery_server.py`) |
| cam0/cam1 fisheye intrinsics | **solved** — ~0.23 px RMS, 104.5° H |
| Stereo extrinsics | **PLACEHOLDER** (design values, not a measurement) |
| Stitcher | working, fisheye + paired input |

⚠️ **Capture bitrate is unvalidated and looks low.** The panel records 28 Mbit/s
per camera at 4K30 = **0.113 bits/pixel** (measured 27.3 Mbit on the first dual
take). The stitcher's own `auto` target for its HEVC *output* is **0.20 bpp**,
derived from VMAF runs that put good H.264 at ~0.26 bpp with HEVC buying ~40%.
So the capture master — which then gets de-warped, stitched and re-encoded — is
running at roughly **half** the bits/pixel this project already established as
"good", on high-entropy content (grass, motion). `rock5t-camera/NEXT_STEPS.md`
lists "confirm against footage" as an open item; that was never done. Matching
0.20 bpp would mean ~50 Mbit/cam (~45 GB/hr for the pair, vs ~25 today). Worth
an A/B on real footage before a real game.

⚠️ `calibration/stereo_extrinsics.json` is still `placeholder_extrinsics.py`
output — design geometry assuming perfect mounting (60 mm baseline, 74° toe-in).
Enough to run the pipeline end to end, but the seam will not be right until you
capture real simultaneous pairs with `snap_pair.sh` on the housed rig and
re-solve. See [§3](#3-calibrate).

---

## Prerequisites

**On the Rock** — run `./setup.sh` (or `./setup.sh --check` to install nothing).
It checks the tools, the GStreamer elements the pipelines actually use
(including `h265parse` and the Rockchip `mpph265enc`), the driver binding, the
two ISP mainpath nodes, the rkaiq daemon, the IQ file and the `veery.service`
install. It does *not* build the kernel driver or overlay — see
[ROCK5T_CAMERA.md](ROCK5T_CAMERA.md) and `rock5t-camera/driver/NOTES.md`.

The recorder needs **no pip packages** — `veery_server.py` and
`calib_server.py` are Python 3 standard library only, on purpose.

**On the Mac/PC** — calibration needs Python 3 with OpenCV:

```bash
python3 -m venv .venv && ./.venv/bin/pip install -U opencv-contrib-python numpy
```

Stitching needs CMake, OpenCV (4.x or 5.x) and `ffmpeg` on PATH. Build it once:

```bash
cd stitching && ./stitch.command
```

(`stitch.bat` on Windows. Both build on first run, then launch the tuner.)

---

## Quick reference

| Fact | Value |
|---|---|
| Takes live on the Rock at | `/home/radxa/recordings/` |
| Each take produces | `take_TS_cam0.mkv` **and** `take_TS_cam1.mkv` |
| Capture mode | 4K30 (3840×2160 @ 30 fps), HEVC, CBR |
| Bitrate | **28 Mbit/s per camera** → ~12.6 GB/hr each, **~25 GB/hr for the pair** |
| A ~75-min game | ~**32 GB** total |
| Web panel | `http://veery.local:8080` |
| Shuttle SSD copies land in | `<drive>/rock-recordings/` |
| Calibration lives in | `calibration/` (cam0/cam1 intrinsics + stereo extrinsics) |
| Stitcher output | HEVC in `.mp4`, tagged `hvc1`, bitrate `auto` (~0.20 bpp) |
| **No audio** | the Rock recorder does not capture audio today |

**Golden rule:** *copy → verify → only then delete.*

---

## 1. Record (on the Rock)

```bash
sudo python3 rock5t-camera/recorder/veery_server.py
```

Or let systemd run it at boot (`veery.service`). Then open
`http://veery.local:8080`:

- two live previews (continuous, ISP selfpath, 1080p/5 fps)
- one **Record** button — starts both cameras, writes two MKVs
- **Manage Files** — browse takes, mount/copy to the shuttle SSD, delete

Previews run on the *selfpath* and recording on the *mainpath*, so previews keep
running during a take. Stop sends SIGINT to the process group → GStreamer emits
EOS → a finalized, seekable file.

The sensor mode and bitrate are **fixed** at the rig's target — 4K30, 28 Mbit
per camera — deliberately: one less thing to get wrong at a game. Change the
tuning, not the panel. (See the bitrate note in [Status](#status).)

> Only one process can hold a camera node. If you run anything else against the
> cameras, stop the panel first: `sudo systemctl stop veery`.

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

### 2c. Network alternative

Plain `rsync` over the LAN, from the **Mac**:

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
python3 calibrate.py
```

Results are written alongside the scripts, which is where the stitcher looks.

The model is **fisheye (equidistant), always** — there is no `--model` flag.
Board defaults are the measured 7×10 / 78 mm / 58 mm board; if you reprint at
another scale, measure a square and pass `--square-mm` / `--marker-mm`, or the
baseline scale will be wrong.

What good output looks like: **RMS well under 1 px** (this rig solves at
~0.23 px) and ~104.5° horizontal FOV.

To re-solve only the extrinsics against already-good intrinsics:

```bash
python3 calibrate.py --use-intrinsics . \
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

Build once — `./stitch.command` (Mac) or `stitch.bat` (Windows). Run from
`stitching/`.

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

---

## License

See [LICENSE](LICENSE).
