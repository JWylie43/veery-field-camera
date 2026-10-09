# Veery — ROCK 5T Stereo Field Camera

A field camera for filming a whole game from the sideline: two cameras on a
**Radxa ROCK 5T** record the field in 4K, and a Mac app stitches the two views into one
wide panorama and cuts it into a normal 16:9 video you "film" afterwards with a virtual
camera.

```
  ROCK 5T (on the rig)                       Mac
  ──────────────────────                     ──────────────────────────────────────────
  two IMX477 cameras, genlocked              Studio (browser app, studio/)
  → recorder panel :8080                       Align  → each take's camera alignment
  → take_TS_cam0.mkv + _cam1.mkv   ──SSD──►    Stitch → one panorama (.mp4)
                                               Edit   → points + virtual camera → 16:9 video
```

Two Raspberry Pi HQ (**Sony IMX477**) sensors with 110° fisheye lenses in a 3D-printed
housing, each recorded at **4K30** by the RK3588's hardware HEVC encoder.

> **Platforms:** the Rock runs Radxa's Debian. The Mac side (Studio, calibration) is
> **macOS / Linux only** (tested on macOS); Windows is not supported.
> This is the `radxa-rock-5t` branch; the earlier Jetson Orin Nano rig is on
> [`nvidia-orin-jetson-nano`](../../tree/nvidia-orin-jetson-nano).

---

## 1. Set up the Rock

1. Download **`rock5t-camera-image-N.img.xz`** from the newest `image-N`
   [release](https://github.com/JWylie43/veery-field-camera/releases) and flash it to
   an SD card or NVMe drive (**16 GB+**) with balenaEtcher or Raspberry Pi Imager.
2. On the card's **`config`** drive (it appears on the Mac after flashing), fill in
   **`camera-network.txt`**: your Wi-Fi, and the password for the Rock's own hotspot
   (`VeeryCam` by default - change its default password).
3. Connect the cameras and power on. The first boot takes a few minutes.

The recorder is then at `http://veery.local:8080` at home, or - when your Wi-Fi isn't in
range - on the `VeeryCam` hotspot at `http://10.43.0.1:8080`. Log in with
`ssh radxa@veery.local` (password `radxa`; change it with `passwd`).

Details - network behaviour, updating a running Rock, setting up from Radxa's own image,
the tested versions: **[docs/rock.md](docs/rock.md)**.

## 2. Set up the Mac

```bash
brew install python cmake opencv ffmpeg     # plus Xcode's tools: xcode-select --install
git clone https://github.com/JWylie43/veery-field-camera.git && cd veery-field-camera
./setup.sh                                  # Python environment + Studio's C++ tools
```

(On Linux, install the same four with the system package manager and a C++ compiler.)
Start Studio with `studio/studio.command` (or double-click it). It opens in the browser
and works on the takes folder `~/Desktop/takes` (`--takes DIR` for another).

## 3. A game

1. **Record** - open the recorder panel, check both previews, press **Record**;
   **Stop** at the end. Each take is two files, `take_TS_cam0.mkv` + `take_TS_cam1.mkv`
   (~25 GB per hour).
2. **Offload** - plug the shuttle SSD into a blue USB 3 port on the Rock, **Manage
   Files → Copy to drive**, eject; copy the takes into `~/Desktop/takes` on the Mac.
   Copy → verify → only then delete from the Rock.
3. **Align** - in Studio, tick the new takes → **Align selected** (measures each take's
   camera alignment; the mount settles slightly between sessions).
4. **Stitch** - **Stitch →** on a take: set the crop box (and rotation / shear if
   needed) on any frame → **Stitch**.
5. **Edit** - **Edit →** on the stitched video: mark each point with **I** / **O**, then
   press **R** and steer the box with the mouse while it plays → **Render…**.

## More

| | |
|---|---|
| [docs/rock.md](docs/rock.md) | the Rock: image setup, Wi-Fi and hotspot, `setup.sh`, tested versions, recording, offload |
| [docs/calibration.md](docs/calibration.md) | the base calibration and per-take alignment |
| [docs/stitching.md](docs/stitching.md) | the stitch page, checking a take, the stitcher's command line, how it works |
| [docs/editing.md](docs/editing.md) | the editor: cutting, framing, keys, rendering |
| [studio/README.md](studio/README.md) | how Studio works: pages, jobs, what a stitched video records |
| [rock5t-camera/image-build/](rock5t-camera/image-build/README.md) | building and publishing the Rock image |
| [rock5t-camera/kernel-build/](rock5t-camera/kernel-build/README.md) | building and publishing the camera kernel |
| [rock5t-camera/ROCK5T_CAMERA.md](rock5t-camera/ROCK5T_CAMERA.md) | hardware bring-up log - read before touching the driver, overlay or ISP tuning |

## Status

| Piece | State |
|---|---|
| IMX477 driver + dual-camera overlay | working - camera kernel `6.1.84-8-rk2410-imx477` ([`kernel-1`](https://github.com/JWylie43/veery-field-camera/releases/tag/kernel-1)) |
| ISP tuning | tuned (`rock5t-camera/iqfiles/`) |
| Dual 4K30 recording | working |
| Calibration | solved and refined on footage (2026-10-03); per-take alignment in Studio |
| Rock image | [`image-1`](https://github.com/JWylie43/veery-field-camera/releases/tag/image-1) - flashed and verified on the rig |
| Studio: align / stitch / edit | working |
| Open | capture bitrate (28 Mbit/s per camera) looks low for stitching - see [docs/rock.md](docs/rock.md#open-questions) |

## Project layout

```
veery-field-camera/
├── rock5t-camera/        runs ON THE ROCK
│   ├── recorder/           the recorder panel (server.py) + its service
│   ├── setup.sh            installs / updates / checks everything on a running Rock
│   ├── system/             network setup + rkaiq 3A fix (installed by setup.sh / the image)
│   ├── image-build/        builds the ready-to-flash image
│   ├── kernel-build/       builds the camera kernel
│   ├── driver/  overlay/  iqfiles/   the IMX477 driver, camera overlay, ISP tuning
│   └── reference/          vendor source material (drivers, overlays, schematic)
├── studio/               runs ON THE MAC - Studio (server.py, static/ pages, native/ C++ tools)
├── calibration/          the base calibration (calibrate.py, the board, the JSONs)
├── docs/                 the detailed guides
├── 3d-housing-model/     the printable enclosure
├── setup.sh              Mac setup (.venv + Studio's C++ tools)
└── requirements.txt      the Mac's Python packages
```

## License

See [LICENSE](LICENSE).
