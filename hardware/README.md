# Hardware

The physical rig: what it's made of, why, and how it goes together. The software
(everything else in this repo) assumes this hardware - the camera driver, the overlay,
the tuning and the calibration are all specific to it.

- [Parts list](#parts-list)
- [Genlock](#genlock)
- [Design choices](#design-choices)
- [Assembly](#assembly)
- [Files](#files)

---

## Parts list

### The camera

| Part | Qty | Notes | Price | Link |
|---|---|---|---|---|
| **Raspberry Pi HQ Camera - M12 mount** (Sony IMX477) | 2 | the sensors, with an M12 lens mount built in. Ships with **three lens locking rings** (one used, two spare) and a 1/4"-20 tripod thread | $52.50 each | [Adafruit 5661](https://www.adafruit.com/product/5661) |
| **Commonlands CIL391** lens, 3.25 mm M12, 110° | 2 | fisheye, -16% barrel; ~104.5° horizontal in the 4K mode. Made for the M12 HQ Camera | $39 each | [Commonlands](https://commonlands.com/products/gopro-replacement-m12-lens) |
| **Radxa ROCK 5T** (RK3588), 8 GB | 1 | the computer: two camera connectors, hardware HEVC encoder, Wi-Fi 6 | ~$297 | [Radxa](https://radxa.com/products/rock5/5t) |
| **Radxa Heatsink 6240B** | 1 | the board's heatsink + fan (fits ROCK 5B / 5B+ / 5T) | from Radxa's partners | [Radxa](https://radxa.com/products/accessories/heatsink-6240b/) |
| **Radxa FPC cable AC008** (31-pin 0.3 mm → 15-pin 1.0 mm) | 2 | board ↔ camera | ~$9 | [Radxa](https://radxa.com/products/accessories/fpc-adapter-cable-ac008) |
| **Noctua NF-A4x10 5V** fan (40×10 mm) | 2 | housing cooling | ~$17 each | [Amazon](https://www.amazon.com/dp/B00NEMGCIA) |
| **NVMe SSD**, M.2 2280, **500 GB minimum** | 1 | system + recordings (~25 GB per hour of recording). This rig: WD Green SN350 500 GB | ~$114 | [Amazon (WD SN350 500 GB)](https://www.amazon.com/dp/B0BXVVYCRN) |
| Genlock wire | 1 | a single wire between the two cameras' **XVS** pins - see [Genlock](#genlock) | - | - |
| 3D-printed housing (top + bottom) | 1 set | [`housing/`](housing/); holds the board and both cameras at the fixed toe-in | ~$12 | print it |
| 1/4"-20 brass heat-set inserts | as needed | the mount thread in the housing | ~$11 (30-pack) | [Amazon](https://www.amazon.com/dp/B0DNQLXJHY) |

> This rig was actually built with the **C/CS-mount** HQ Camera
> ([Adafruit 4561](https://www.adafruit.com/product/4561)) plus an M12-to-CS adapter
> (from an Arducam lens kit). The M12-mount version above replaces both and holds the
> same lens directly.

### Power

| Part | Qty | Notes | Price | Link |
|---|---|---|---|---|
| **USB-C PD to DC barrel cable** (5-20 V selectable) | 1 | **set it to 15 V** for the ROCK 5T | ~$17 | [Amazon](https://www.amazon.com/dp/B0GRJC9TR1) |
| **USB-C PD power bank** | 1 | any power bank with USB-C Power Delivery output. This rig: Anker A1289 | ~$119 | [Anker A1289](https://www.anker.com/products/a1289) |

**Power path:** power bank (USB-C PD) → PD-to-DC cable, **set to 15 V** → the ROCK 5T's
DC jack.

---

## Genlock

The two cameras are **genlocked**: a single wire joins the **XVS** (vertical sync) pins of
the two sensors. One camera, cam0, drives the frame timing and the other, cam1, follows
it, so both expose every frame at the same instant: moving players line up across the
seam, and the two recordings have an exact, constant frame offset. The driver sets which
camera leads (the kernel parameter `imx477.genlock`, on by default: cam0 leads, cam1
follows).

This is **why the Raspberry Pi HQ Camera** is used: its board exposes the IMX477's XVS
signal. Smaller IMX477 modules don't break it out, and without it the two cameras would
free-run and drift apart in time.

---

## Design choices

- **ROCK 5T**: its RK3588 encodes two 4K30 HEVC streams in hardware, so the CPU stays
  near idle and the rig has thermal headroom for hours outdoors. Two camera connectors,
  one per sensor.
- **Two Pi HQ (IMX477) cameras**: well-documented sensor with a Raspberry Pi driver and
  tuning to start from; the ROCK 5T needed its own driver for it (see
  [`../rock5t-camera/`](../rock5t-camera/)). The M12-mount version takes small, light
  M12 lenses (5 g each), which keeps the two cameras close together in a compact housing.
- **110° fisheye lenses, ~75° apart**: two wide views overlapping in the middle cover a
  whole field from the sideline. The overlap is where they're stitched.
- **Genlock (XVS)**: both sensors expose at the same instant, so moving players line up
  across the seam and the two files have an exact frame offset - the reason for the HQ
  Camera (see [Genlock](#genlock)).
- **Cameras mounted upside down**: images are rotated 180° in software everywhere they
  are saved or shown (see [docs/calibration.md](../docs/calibration.md#two-facts)).
- **Fixed 4K30, 28 Mbit/s per camera**: one less thing to get wrong at a game (see the
  open bitrate question in [docs/rock.md](../docs/rock.md#open-questions)).
- **A rigid housing**: the stitch depends on the cameras' exact angles, so nothing may
  move. The mount still settles slightly between sessions, which per-take alignment in
  Studio corrects.

---

## Assembly

The points the software depends on:

1. Mount both cameras in the housing **upside down**, at the housing's fixed toe-in.
2. Connect each camera to one of the ROCK 5T's two camera connectors with an FPC cable.
3. Wire the two cameras' XVS pins together ([Genlock](#genlock)).
4. **Focus each lens and lock it:** screw a locking ring onto the lens, screw the lens
   into the camera's M12 mount until the image is sharp, then tighten the ring against
   the mount. Never turn or reseat a lens afterwards without recalibrating that camera.
5. Fit the NVMe drive, flash the Rock image (see the [README](../README.md#1-set-up-the-rock)),
   and confirm both cameras with `rock5t-camera/setup.sh --check`.
6. Calibrate the finished rig - [docs/calibration.md](../docs/calibration.md). Once per
   housing build, and again if the mount is disturbed.

---

## Files

- [`housing/`](housing/) - the enclosure, top and bottom: `.3mf` (editable) and `.stl`;
  the `PRINT ORIENTED` files are rotated for printing.
- [`../rock5t-camera/SCHEMATIC_FACTS.md`](../rock5t-camera/SCHEMATIC_FACTS.md) - the
  ROCK 5T's camera connector wiring, from Radxa's schematic.
- [`../rock5t-camera/ROCK5T_CAMERA.md`](../rock5t-camera/ROCK5T_CAMERA.md) - the hardware
  bring-up log: measured lens FOV, cable signal-integrity findings, the 4K link speed.
