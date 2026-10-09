# Calibration and alignment

How the rig's camera geometry is measured: the base calibration (once per housing) and
the per-take alignment (every take). Quick start is in the [README](../README.md).

- [Two facts](#two-facts)
- [Base calibration](#base-calibration)
- [Per-take alignment](#per-take-alignment)

---

## Two facts

**The calibration is fisheye, always.** The CIL391 lenses are 110° with -16% barrel. A
pinhole+polynomial fit leaves a uniform ~2.6 px residual - a model mismatch, not noise;
the equidistant model lands at ~0.23 px. `calibrate.py` writes `"model": "fisheye"` and
the stitcher **rejects** anything else, because the wrong projection doesn't fail
loudly - it silently yields a plausible-looking panorama built from the wrong geometry.

**Orientation.** The cameras are mounted upside down. Everything that saves or shows an
image (recording encoder, panel previews, `/calib` snapshots) rotates it 180°, so files
on disk are upright, and calibration is solved in that orientation (the current JSONs
are). The stitcher then sees cam1 to the left of cam0 and swaps the two automatically.

---

## Base calibration

`calibration/cam0_intrinsics.json`, `cam1_intrinsics.json` and `stereo_extrinsics.json`
- the stitcher finds them there. Do this once per housing build, and again if the mount
is disturbed. Intrinsics are mount-independent; **extrinsics are not**.

### Capture (on the Rock)

Show `calibration/charuco_board.png` full-screen (a TV works) and open the recorder
panel's **Calibration Snapshots** page, `http://veery.local:8080/calib`. Each **Take
Snapshot** saves a full-res 3840×2160 pair, `cam0_NNN.png` + `cam1_NNN.png`, into
`~/calib-pairs` on the Rock.

- For **intrinsics**, walk the board around each camera's whole frame, edges and
  corners included (a shot only one camera sees still counts for that one).
- For **extrinsics**, hold the board still where *both* cameras see it - the pairs are
  simultaneous, which is what the stereo solve needs.

Aim for ~30-40 poses at varied depth and tilt.

### Solve (on the Mac)

```bash
cd calibration
scp -r radxa@veery.local:~/calib-pairs images-pairs
../.venv/bin/python calibrate.py --cam0-glob 'images-pairs/cam0_*.png' --cam1-glob 'images-pairs/cam1_*.png' \
        --square-mm <measured> --marker-mm <measured>
```

Results are written next to the script, which is where the stitcher looks. The model is
fisheye, always - there is no `--model` flag. `--square-mm`/`--marker-mm` are the board
**as displayed**: measure a square on the screen every session (it changes with the
viewer's zoom; 71/53 mm was the TV in full-screen Preview on 2026-10-03). Wrong values
scale the baseline wrongly.

To re-solve only the extrinsics against already-good intrinsics:

```bash
../.venv/bin/python calibrate.py --use-intrinsics . --square-mm 71 --marker-mm 53 \
        --cam0-glob 'images-pairs/cam0_*.png' --cam1-glob 'images-pairs/cam1_*.png'
```

**Good output:** RMS well under 1 px (this rig's intrinsics solve at ~0.23 px) and
~104.5° horizontal FOV. Eyeball the sanity images it writes: straight lines straight in
`camN_undistort_sample.jpg`; in `stereo_reprojection_sample.jpg` the red crosses
(solved) inside the green circles (detected) in BOTH halves. Then stitch one pair and
look at the seam - the real test (from the repo root):
`studio/native/build/StitchPipeline --source calibration/images-pairs/cam0_013.png --out-file pano.jpg`.

**`calibrate.py` refuses to write `stereo_extrinsics.json` above 3 px RMS**, because a
bad extrinsic still produces a plausible-looking panorama. The usual cause is pairing
frames that weren't taken together (two separate per-camera shoots sort into unrelated
"pairs") - use `/calib` pairs. `--force-extrinsics` overrides. The stereo solve maps
corners through each camera's fisheye model before `cv2.stereoCalibrate`, which only
knows the pinhole model (fed fisheye coefficients directly it gave 28 px RMS and a
2.8° "toe-in" on good pairs).

### The current calibration

First housed solve (2026-10-03, 26 pairs): yaw 74.7°, 1.5° residual tilt, baseline
67.8 mm, 1.87 px RMS (an upper bound - each board pose comes from cam0 alone and is
carried into cam1); the seam through the board is continuous.

**Scene refinement (same day).** On real footage that solve left cam1 ~12 px low at the
seam and tipped (~23 px per 1000 px across the overlap) - a ~0.3° tilt and ~1.2° roll
error. With the board only ~1 m away and seen at the fisheye edges, a small tilt and a
small vertical offset look alike, so the solver traded one for the other. The rotation
was corrected from footage using only **parallax-free** measurements:

| Correction | Measured from |
|---|---|
| pitch −0.29° | vertical offset at the seam column |
| roll +1.24° | change of vertical offset across the overlap, after fitting out the parallax part (which grows toward the near rows) |
| yaw −0.40° | horizontal offset on the far field just below the horizon |

After it the cameras agree for a scene at infinity (vertical ~0.3 px, far-field
horizontal ~0), and the shear (`--shift-top`/`--shift-bottom`) carries only parallax.
The corrections and the original matrix are under `scene_refinement` in the JSON;
`translation_mm` is still the board value (the stitcher ignores it).

---

## Per-take alignment

The mount can settle by a fraction of a degree between sessions (2026-10-08: −11 px
vertical at the seam after a week in the same housing), which shear can't fix. So each
take gets its own alignment, measured on its own footage: in Studio, tick the takes and
**Align selected**. By hand, from the repo root:

```bash
.venv/bin/python studio/refine_extrinsics.py --align ~/Desktop/takes/take_TS_cam0.mkv
```

(~10 s; either file of the pair works; the files are paired by their capture timestamps,
as the stitcher pairs them.) It solves that take's tilt / roll / yaw correction from
parallax-free measurements only, measures its shear, and writes both to
`take_TS.align.json` next to the take. The stitcher and Studio then use it for that take:
its rotation replaces the base rotation, and its shear fills in Shift far / Shift near
(CLI: unless you pass `--shift-top`/`--shift-bottom`/`--shift-x`; `--no-align` ignores
the file). Takes without one use the base calibration, which `--align` never changes.

- `--check` measures and reports only (against the take's file if it has one; `--base`
  for the base calibration).
- `--apply` writes the correction into the BASE calibration instead (with a
  `.bak-<time>` backup) - only for when the rig has settled for good.

Needs a daytime take with textured ground in the overlap. The rotation is parallax-free;
the shear is the measured parallax for that rig position (the straight-line best fit for
the ground - nearer people and objects are left to the smart seam).

**Why shear at all:** the calibration aligns the cameras' *directions*, which is exact
only for distant things. The lenses are ~68 mm apart, so a nearer object lands at a
different spot in each view - roughly 2104 px × 0.068 m / distance: ~3 px at 50 m,
~15 px at 10 m, ~30 px at 5 m. The far edge of the field needs ~nothing; the near edge
(bottom of frame) a few to a few tens of px, depending on how close the rig is to the
touchline.
