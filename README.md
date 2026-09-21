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

---

## Project layout

```
veery-field-camera/
├── rock5t-camera/        Runs ON THE ROCK — driver, device tree, tuning, recorder
│   ├── driver/             IMX477 kernel driver (out-of-tree module)
│   ├── overlay/            dual-camera device-tree overlay
│   ├── kernel-build/       kernel build script + notes
│   ├── iqfiles/            rkaiq ISP tuning (generated; → /etc/iqfiles/)
│   ├── reference/          vendor DTS / drivers / schematic used as source material
│   └── recorder/           veery_server.py (web panel), record_dual.sh, systemd units
├── calibration/          Runs ON YOUR MAC — ChArUco stereo calibration
│   ├── calib_server.py     Rock-side capture panel (preview + full-res snapshot)
│   ├── snap_pair.sh        Rock-side: capture ONE simultaneous cam0+cam1 pair
│   ├── show_board.py       generate/display the ChArUco board
│   ├── calibrate.py        cam0+cam1 fisheye intrinsics + stereo extrinsics
│   └── rock-rig/           the rig's calibration results (committed)
├── stitching/            Runs ON YOUR MAC/PC — panorama stitcher (C++)
│   ├── stitch_pipeline.cpp   calibration-driven cylindrical stitch + browser tuner
│   ├── pair_check.py         verify a take's two files before stitching
│   └── CMakeLists.txt, include/json.hpp
├── 3d-housing-model/     Printable enclosure (Rock Housing top/bottom, .3mf + .stl)
├── setup.sh              Rock dependency install + pipeline health check
├── ROCK5T_CAMERA.md      Hardware bring-up log: driver, overlay, ISP, genlock
└── WORKFLOW.md           Field workflow: record → offload → calibrate → stitch
```

`ROCK5T_CAMERA.md` is the engineering record for the board itself — what was
tried, what failed and why. Read it before touching the driver or the overlay.

---

## How the pieces fit

**1 — Record (on the Rock).** Two IMX477s, genlocked over XVS, each written to
its own MKV by its own GStreamer pipeline:

```
rkisp mainpath (NV12, tuned IQ) → mpph265enc (CBR) → matroskamux → take_TS_camN.mkv
```

Two **independent** files is deliberate: fault isolation, full per-camera
timestamps, no live cross-coupling. Pairing happens later, at stitch time.

Drive it from the web panel (`veery_server.py`, port 8080) or the script
(`record_dual.sh`).

**2 — Calibrate (capture on the Rock, solve on the Mac).** `calib_server.py` or
`snap_pair.sh` captures full-res ChArUco frames; `calibrate.py` solves per-camera
**fisheye** intrinsics and the stereo extrinsics into `calibration/rock-rig/`.

**3 — Stitch (on the Mac/PC).** `StitchPipeline` reads that calibration and warps
both cameras onto one cylinder. No feature detection — the alignment comes
entirely from the calibrated geometry. Run it headless or use the browser tuner.

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
| IMX477 driver + dual-camera overlay | working on kernel 6.1.84-8-rk2410 |
| ISP tuning (`imx477_RPI-HQ_default.json`) | tuned; see `iqfiles/TRANSLATION_NOTES.md` |
| Dual 4K30 HEVC recording | working (`record_dual.sh`, `veery_server.py`) |
| cam0/cam1 fisheye intrinsics | **solved** — ~0.23 px RMS, 104.5° H |
| Stereo extrinsics | **PLACEHOLDER** (design values, not a measurement) |
| Stitcher | working, fisheye + paired input |

⚠️ `calibration/rock-rig/stereo_extrinsics.json` is still
`placeholder_extrinsics.py` output — design geometry assuming perfect mounting
(60 mm baseline, 74° toe-in). It is enough to run the pipeline end to end, but
the seam will not be right until you capture real simultaneous pairs with
`snap_pair.sh` on the housed rig and re-solve. See WORKFLOW.md §3.

---

## Prerequisites

**On the Rock** — run `./setup.sh`. It installs `v4l-utils`, `i2c-tools`,
`ffmpeg` and the GStreamer + Rockchip MPP stack, then checks the driver, the I²C
sensor binding, the two rkisp mainpath nodes, the rkaiq daemon and the IQ file.
It does *not* build the kernel driver or overlay — see `ROCK5T_CAMERA.md`.

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

## Quick start

```bash
# On the Rock — record a 10-second test take
cd rock5t-camera/recorder && DUR=10 ./record_dual.sh

# On the Mac — sanity-check the pair, then stitch it
python3 stitching/pair_check.py take_..._cam0.mkv
cd stitching && ./build/StitchPipeline --source ../take_..._cam0.mkv --tune
```

Full field procedure — offload, calibration capture, tuning, parallel stitch —
is in [WORKFLOW.md](WORKFLOW.md).

---

## License

See [LICENSE](LICENSE).
