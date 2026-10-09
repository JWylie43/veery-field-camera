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
- [Studio: align, stitch and edit in the browser](#studio-align-stitch-and-edit-in-the-browser)
- [1. Record](#1-record-on-the-rock)
- [2. Offload](#2-offload-on-the-rock)
- [3. Calibrate](#3-calibrate)
- [4. Check a take](#4-check-a-take-before-stitching-on-the-mac)
- [5. Stitch](#5-stitch-on-the-mac--pc)
- [6. Verify the output](#6-verify-the-output)
- [7. Edit: points and virtual camera](#7-edit-points-and-virtual-camera)

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
│   ├── director.cpp/.html    edit: cut the game into points, steer a 16:9 virtual camera, render
│   ├── pair_check.py         verify a take's two files before stitching
│   └── CMakeLists.txt, include/json.hpp
├── studio/               Runs ON YOUR MAC — Veery Studio, the takes folder in a browser
│   ├── server.py             local web app: takes list, batch align, stitch page, editor, jobs
│   ├── refine_extrinsics.py  per-take camera alignment (--align / --check / --apply)
│   ├── rig.py                take files, calibration and the stitcher's geometry in Python
│   └── studio.command        double-click to start (own venv: setup.sh, requirements.txt)
├── 3d-housing-model/     Printable enclosure (Rock Housing top/bottom, .3mf + .stl)
├── setup.sh              Rock dependency + pipeline health check
└── ROCK5T_CAMERA.md      Hardware bring-up log
```

The three calibration JSONs live directly in `calibration/` because that is
where the stitcher looks by default — no `--calib-dir` needed.

---

## How the pieces fit

**1 — Record (on the Rock).** Two IMX477s, genlocked over XVS, both recorded by
ONE GStreamer pipeline into one MKV per camera:

```
rkisp mainpath (NV12, tuned IQ) → videorate → mpph265enc (CBR, rotation=180) → matroskamux → take_TS_camN.mkv
```

One pipeline means one clock: genlocked frames carry the same timestamps in
both files, each file keeps its first frame's real start time, and both are
tagged `veery-shared-clock`. The stitcher (and `pair_check.py`) read the two
start times and get the frame offset **exactly** - no brightness guessing.
The trade-off, chosen deliberately: a fault in one camera ends the whole take.

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

`calibration/stereo_extrinsics.json` is the real housed-rig solve (2026-10-03:
74.7° yaw), with its rotation then corrected on real footage so the cameras line
up vertically (see "Scene refinement" in [§3](#3-calibrate)). Re-solve it if the
mount is ever disturbed.

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
| **No audio** | by design - neither the recorder nor the stitcher handles audio |

**Golden rule:** *copy → verify → only then delete.*

---

## Studio: align, stitch and edit in the browser

Once the takes are on the Mac, **Veery Studio** does steps 3–7 for a folder of
takes in one local web app (`studio/README.md` has the details):

```bash
studio/studio.command                 # or double-click it; first run sets up studio/.venv
```

- **Takes list** (`http://127.0.0.1:8100/`): every `take_…_cam0/_cam1` pair in
  `~/Desktop/veery-takes` (`--takes DIR` for another folder). Tick several and
  **Align selected** - each runs `refine_extrinsics.py --align` as a job with live
  status; the row's details show the result and the log.
- **Stitch →** opens `/stitch/<take>`: the tuner's controls on any frame of the
  pair, with its alignment file (or the base calibration) and shear filled in.
  **Stitch** runs `StitchPipeline` as a job (one at a time) and writes the
  settings into the video's metadata.
- **Stitched videos** (those with that metadata) get **Edit →**: the Director at
  `/edit/<video>`; Render runs `Director --render` as a job.

The C++ tools still run on their own exactly as below; Studio just calls them.

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

> **Orientation.** The cameras are mounted upside down. The sensors read out
> as they are, and everything that saves or shows an image (recording
> encoder, panel previews, `/calib` snapshots, `calib_server.py`,
> `snap_pair.sh`) rotates it 180°, so files on disk are upright. Calibration
> must be solved in that same orientation. Files solved from older
> upside-down captures are converted once with
> `python3 calibration/rotate180.py` (exact: it moves the principal point and
> conjugates the extrinsic rotation, and marks each file so it is never
> converted twice). The stitcher then sees cam1 to the left of cam0 and swaps
> the two automatically.

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

To re-solve only the extrinsics against already-good intrinsics, from pairs
taken on the panel's `/calib` page (`scp -r veery:~/calib-pairs images-pairs`):

```bash
python3 calibrate.py --use-intrinsics . --square-mm 71 --marker-mm 53 \
        --cam0-glob 'images-pairs/cam0_*.png' --cam1-glob 'images-pairs/cam1_*.png'
```

`--square-mm`/`--marker-mm` are the board **as displayed** - measure a square
on the screen every session (it changes with the viewer's zoom). 71/53 mm was
the TV in full-screen Preview on 2026-10-03.

First housed solve (2026-10-03, 26 pairs): yaw 74.7 deg, 1.5 deg residual
tilt, baseline 67.8 mm, 1.87 px RMS (an upper bound - each board pose comes
from cam0 alone and is carried into cam1); the seam through the board is
continuous.

**Scene refinement (same day).** On real footage that solve left cam1 ~12 px
low at the seam and tipped (~23 px per 1000 px across the overlap) - a ~0.3°
tilt and ~1.2° roll error, plus a 4.6 mm height difference the mount doesn't
have. With the board only ~1 m away and seen at the fisheye edges, a small tilt
and a small vertical offset look alike, so the solver traded one for the other.
The rotation was corrected from footage using only **parallax-free**
measurements, so nothing tied to where the rig stands went into the calibration:

| Correction | Measured from |
|---|---|
| pitch −0.29° | vertical offset at the seam column |
| roll +1.24° | change of vertical offset across the overlap, after fitting out the parallax part (which grows toward the near rows) |
| yaw −0.40° | horizontal offset on the far field just below the horizon |

After it the cameras agree for a scene at infinity (vertical ~0.3 px, far-field
horizontal ~0), and `--shift-top`/`--shift-bottom` carry only parallax - for
the 2026-10-03 test position (~15-18 ft up) that was −3 / +8.4, `--shift-y` 0.
Re-tune the shear whenever the rig moves; the calibration stays. The
corrections and the original matrix are under `scene_refinement` in the JSON.
`translation_mm` is still the board value; the stitcher ignores it.

**Per-take alignment (run on every new take).** The mount can settle by a
fraction of a degree between sessions (2026-10-08: −11 px vertical at the seam
after a week in the same housing), which shear can't fix. So each take gets its
own alignment, measured on its own footage. Easiest: tick the takes in
[Studio](#studio-align-stitch-and-edit-in-the-browser) and **Align selected**. By hand, on the Mac, from the repo root:

```bash
studio/.venv/bin/python studio/refine_extrinsics.py --align ~/Desktop/veery-takes/take_TS_cam0.mkv
```

(~10 s; either file of the pair works; the files are paired by their capture
timestamps, the same way the stitcher pairs them.) It solves that take's tilt / roll / yaw
correction from parallax-free measurements only and measures its shear, and
writes both to `take_TS.align.json` next to the take. From then on the stitcher
and tuner use that file automatically for that take: its rotation replaces the
base rotation, and its shear fills in Shift far / Shift near (CLI: used unless
you pass `--shift-top`/`--shift-bottom`/`--shift-x`; `--no-align` ignores the
file). Takes without a file use the base calibration (`stereo_extrinsics.json`,
the calibration-day refinement above), which `--align` never changes.

- `--check` measures and reports only (using the take's file if it has one;
  `--base` to measure against the base) - shows the offsets and the shear.
- `--apply` writes a correction into the BASE calibration instead (with a
  `.bak-<time>` backup) - only for when the rig has settled for good.

Needs a daytime take with textured ground in the overlap. The rotation is
parallax-free; the shear is the measured parallax for that rig position (the
straight-line best fit for the ground - nearer people/objects are left to the
smart seam).

> **`calibrate.py` refuses to write `stereo_extrinsics.json` above 3 px RMS.**
> That guard exists because the failure is silent: a bad extrinsic still
> produces a plausible-looking panorama. The usual cause is feeding it the two
> *intrinsics* folders — those are independent per-camera shoots, both numbered
> `img_NNN`, so sort-order pairing matches unrelated frames. Use `/calib` (or
> `snap_pair.sh`) pairs. `--force-extrinsics` overrides, but you almost never
> want that.
>
> The stereo solve is fisheye-aware: corners are mapped through each camera's
> fisheye model to normalized coordinates before `cv2.stereoCalibrate`, which
> only knows the pinhole model (fed fisheye coefficients directly it gave
> 28 px RMS and a 2.8 deg "toe-in" on good pairs).

Eyeball the sanity images it writes: straight lines straight in
`camN_undistort_sample.jpg`, and in `stereo_reprojection_sample.jpg` the red
crosses (where the solved geometry puts each corner) inside the green circles
(where it was detected) in BOTH halves. Then stitch one pair and look at the
seam - that is the real test:
`./build/StitchPipeline --source "../calibration/images-pairs/cam0_013.png::../calibration/images-pairs/cam1_013.png" --out-file pano.jpg`

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

**Every input is a pair.** Pass either file of a pair and its partner is found
next to it, or give both explicitly:

```bash
--source take_TS_cam0.mkv                    # partner found automatically
--source take_TS_cam1.mkv                    # either half works
--source images-pairs/cam1_013.png           # /calib page pairs too
--source "left.mkv::right.mkv"               # explicit, any names
```

There is no single-file mode; a source that resolves to neither is an error.

### 5a. Interactive tuner (recommended first)

```bash
./stitch.command
```

Opens a browser tuner: **Import source…** (either file of a pair) → align the
far/near edges → **Stitch all frames**. **Output → Choose…** picks the file up
front; once one is chosen the equivalent CLI command is shown and kept in step
with every change, so you can copy it without stitching (Stitch also asks for an
output if none is chosen yet).

Controls: the two edge shifts (near/far parallax, below), **Rotate** (levels the
finished panorama: it rotates the whole panorama and THEN applies the crop box,
exactly as the preview shows - positive = clockwise; it does not change how the
cameras are aligned),
**show seam line** / **crop to box**, **overlap blend** (preview aid), and the
frame seeker. The seam is always the middle of the overlap (smart seam routes it
around moving players); shift-y, blending and exposure match are fixed defaults.

Why the edge shifts still matter with a real calibration: the calibration
aligns the cameras' *directions*, which is exact only for distant things. The
lenses are ~68 mm apart, so a nearer object lands at a different spot in each
view - roughly 2104 px × 0.068 m / distance: ~3 px at 50 m, ~15 px at 10 m,
~30 px at 5 m. The far edge of the field usually needs nothing; the near edge
(bottom of frame) can need a few to a few tens of px depending on how close the
rig is to the touchline.

### 5b. Headless

```bash
./build/StitchPipeline --source take_TS_cam0_seekable.mkv \
    --shift-top 4 --shift-bottom 20 --out-file stitched.mp4
```

| Flag | Meaning |
|---|---|
| `--source <file>` | input — either file of a pair (`_cam0`/`_cam1`, or `/calib` `cam0_`/`cam1_`), or `"a::b"` |
| `--shift-top N` / `--shift-bottom N` | far/near edge alignment (your tuned values) |
| `--shift-x N` / `--shift-y N` | uniform horizontal / vertical shift of cam1 |
| `--pair-offset N\|auto` | frame offset between the two files (default `auto`: exact from the capture timestamps on shared-clock takes, brightness estimate on older ones) |
| `--crop x,y,w,h` | output box (full-canvas coords); applied AFTER `--degrees` |
| `--degrees N` | rotate the finished panorama, positive = clockwise (as in the tuner) |
| `--start N` / `--end N` | frame range |
| `--scale F` | render the cylinder at F× radius — same FOV, fewer pixels |
| `--seam N` / `--bands N` / `--no-smart-seam` | seam placement and blending |
| `--no-exposure` | skip matching cam1's brightness to cam0 |
| `--bitrate auto\|90M` | output rate (default `auto`, ~0.20 bpp) |
| `--venc <name>` / `--no-hwenc` | pick or disable the hardware encoder |
| `--calib-dir <dir>` | calibration folder (default: found by walking up) |
| `--out-file <path>` / `--out <dir>` | output |
| `--tune` | open the browser tuner |

### 5c. A time range only

`--start`/`--end` are **frames**. At 30 fps, `frame = seconds × 30`:

```bash
./build/StitchPipeline --source take_TS_cam0_seekable.mkv \
    --start 2700 --end 5400 --out-file clip_1m30-3m.mp4     # 1:30 → 3:00
```

### 5d. How a video render runs

One process, as a pipeline: decode → remap + exposure → seam → blend → encode,
with several frames in flight and worker threads picking up whichever stage has
work. Decode, seam and encode take frames strictly in order (the smart seam
follows the previous frame's seam), so the seam is one continuous chain over the
whole range and the output is a single file — there is nothing to tune.

The camera files are decoded by ffmpeg with their own colour tags (the cameras
record **full-range BT.709**) and converted to RGB exactly. OpenCV's reader -
used until 2026-10-04 - treated them as limited-range BT.601: contrast too high,
~2.7% of pixels clipped (vs ~1.2% now), grass oversaturated and pushed toward
green. Panoramas stitched before that date carry that look.

On an M5 Pro a 5923×1697 crop renders at ~55 fps (just under 2× real time). That
is the hardware HEVC encoder's ceiling at that size (~57 fps, quality-first
settings); the stitching itself runs faster. `--start` jumps straight to its frame on an
**indexed** file (the remuxed `_seekable` pair); otherwise it logs
`grab-skipping … (SLOW; remux)`.

### 5e. Panorama size

The output size is **derived from the calibration**, not configured: the
cylinder's radius in pixels equals cam0's focal length, so frame centre is
sampled ~1:1. With this rig's 2104 px focal that is **6774×2371**. Use `--scale`
for fewer pixels at the same field of view, or `--crop` to actually crop.

---

## 6. Verify the output

Confirm no frames were lost:

```bash
ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 stitched.mp4
```

The count should land near `fps × seconds` (or `end − start + 1`).

---

## 7. Edit: points and virtual camera

The **Director** turns the full-width panorama into a normal 16:9 video: you
cut the game into points, then steer a "camera" box over each point while it
plays.

```bash
cd stitching && ./director.command          # Windows: director.bat
```

It opens in the browser. **Open video…** loads a stitched panorama; the edit
saves itself within a second of every change, next to it as
`name.director.json` (reopen the video to resume), with timestamped backups
(at most one per 5 min, last 20) in `name.director-backups/`.

**1 · Cut** — play or scrub, press **I** where a point starts and **O** where it
ends. Points show as bars on the timeline and are numbered in game order;
only they end up in the output. Points can't overlap (marking one inside
another is refused). Select a point to relabel it, move its In/Out to the
playhead, or delete it.

**2 · Frame** — select a point and press **R**: playback starts 3 s before the
In (pre-roll, to line up the box), recording runs from In to Out by itself,
then stops. The box follows the mouse left/right with its bottom on a fixed
line; hold **Shift** to move it up/down too. The box starts at the output size
(zoom **1.00× = 1:1**, the sharpest framing; below 1 is wider and still sharp,
above 1 is upscaled/soft - the readout under the video shows which). **Scroll**
zooms ~4% per notch (**Alt** finer, **−**/**=** 2% steps); **0** or double-click
resets to 1:1, **Shift+0** jumps to the widest (full-height) box. Scrolling out
past that (it stops there once first) goes wider still: the box gets taller than
the panorama, stays centred on it, and the output is letterboxed with black
bars - up to the full panorama width. One take covers the whole point; **Esc**
abandons a take in progress.

Takes are protected: recording is never undone by **Cmd/Ctrl+Z** (undo covers
only adding / deleting / moving points, and asks before removing a point that
has a take). Re-recording, clearing or trimming keeps the old take in the
point's history - **↺ Previous take** brings it back. Shortening a framed point
trims its take; lengthening it marks the point *needs re-take*.

Each take is smoothed after recording - jitter and small back-and-forths are
removed and the path follows your general movement without lag. Two sliders per
point adjust it (**Smoothing** = how much, **Steady** = how big a wiggle is
ignored); they re-smooth the take without re-recording.

**V** switches to the big output view; **P** previews the selected point as it
will render, **Preview all** plays every point back to back. **Cmd/Ctrl+Z**
undoes. **?** lists every key.

**Render…** writes one video with the points in order, or one file per point
(`name_01_Point_1.mp4`, …), at the chosen output size (1440p default).
Quality: **H.264 high** (default, plays everywhere), **HEVC high** (same quality,
~20% smaller) or **H.264 standard** (smaller, slightly softer). Frames stay in
the panorama's own YUV from decode to encode (no colour conversion), and are
scaled bicubically with anti-aliasing, so a 1:1 box is a bit-exact crop before
encoding. Unframed points are left out by default (or rendered as a full-height
wide shot). Headless:

```bash
./build/Director --render take.director.json --out game_edit.mp4 [--per-point] [--unframed skip|wide] \
    [--codec h264|hevc] [--quality high|standard]
```

Output size and sharpness: with a 1697 px tall panorama a full-height box is
~3017 px wide, so 1440p has ~1.18× zoom before it upscales (1080p: ~1.57×).

---

## License

See [LICENSE](LICENSE).
