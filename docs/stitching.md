# Stitching

Turning a take's two camera files into one panorama. Quick start is in the
[README](../README.md); Studio itself is described in [studio/README.md](../studio/README.md).

- [In Studio](#in-studio)
- [Check a take](#check-a-take)
- [The command line](#the-command-line)
- [How a render runs](#how-a-render-runs)

---

## In Studio

On the takes list, a pair's **Stitch →** opens its stitch page (`/stitch/<take>`). It
loads the take's own alignment (`.align.json`, filling in the shear - see
[calibration.md](calibration.md#per-take-alignment)) or the base calibration, and shows
any frame of the pair warped exactly as the stitcher will warp it:

- **Shift far (top)** / **Shift near (bottom)** - the parallax shear. ←/→ shift both.
- **Rotate** - levels the finished panorama: it rotates the whole panorama and THEN
  applies the crop box, exactly as the preview shows (positive = clockwise; it doesn't
  change how the cameras are aligned).
- **Crop box** - drag it, or its yellow corners; it is the output frame.
- **In ← here** / **Out ← here** - stitch only part of the take (default: all).
- **Seam line** / **overlap blend** - preview aids. The seam is the middle of the
  overlap (the smart seam routes it around moving players); blending and exposure match
  are fixed defaults.
- **Output** - a file name, saved in the takes folder.

**Stitch** starts the job (one at a time; progress on the page and the takes list) and
records every setting in the video's metadata - which is also what gives the video its
**Edit →** button.

---

## Check a take

Studio checks every take: a pair's **Details** on the takes list shows each file's frame
count and any **timing gaps** (missing time in one camera - everything after a gap is out
of step with the other), and the stitch page shows the frame offset between the two
files, read exactly from their capture timestamps. The two files should have
**identical** frame counts, or differ only by that start offset - the recorder stamps a
rigid 30 fps grid, so a capture drop shows up as a duplicated frame, not a shorter file.

**Make a take seekable.** A file that wasn't finalized cleanly has no seek index: it
won't scrub, and stitching falls back to slow frame-grabbing. Remux both halves (fast,
lossless), keeping the `_cam0`/`_cam1` suffixes so pairing still works:

```bash
ffmpeg -fflags +genpts -i take_TS_cam0.mkv -c copy take_TS_cam0_seekable.mkv
ffmpeg -fflags +genpts -i take_TS_cam1.mkv -c copy take_TS_cam1_seekable.mkv
```

---

## The command line

`StitchPipeline` is a plain command-line tool - Studio runs exactly this, and each
stitched video's metadata holds the command that made it. From the repo root:

```bash
studio/native/build/StitchPipeline --source ~/Desktop/takes/take_TS_cam0.mkv \
    --shift-top 4 --shift-bottom 20 --crop 341,119,6141,2134 --out-file stitched.mp4
```

**Every input is a pair** - there is no single-file mode. Pass either file of a pair and
its partner is found next to it (`take_TS_cam0.mkv` / `_cam1`, or `/calib`'s
`cam0_NNN.png` / `cam1_`), or give both as `"left.mkv::right.mkv"`. A take's
`.align.json` is used automatically.

| Flag | Meaning |
|---|---|
| `--source <file>` | input - either file of a pair, or `"a::b"` |
| `--shift-top N` / `--shift-bottom N` | far/near edge alignment (default: the take's `.align.json`) |
| `--shift-x N` / `--shift-y N` | uniform horizontal / vertical shift of cam1 |
| `--pair-offset N\|auto` | frame offset between the files (default `auto`: exact from the capture timestamps on shared-clock takes, brightness estimate on older ones) |
| `--crop x,y,w,h` | output box (full-canvas coords); applied AFTER `--degrees` |
| `--degrees N` | rotate the finished panorama, positive = clockwise |
| `--start N` / `--end N` | frame range (frames: at 30 fps, `frame = seconds × 30`) |
| `--scale F` | render the cylinder at F× radius - same FOV, fewer pixels |
| `--seam N` / `--bands N` / `--no-smart-seam` | seam placement and blending |
| `--no-exposure` | skip matching cam1's brightness to cam0 |
| `--bitrate auto\|90M` | output rate (default `auto`, ~0.20 bpp) |
| `--venc <name>` / `--no-hwenc` | pick or disable the hardware encoder |
| `--calib-dir <dir>` | calibration folder (default: found by walking up) |
| `--out-file <path>` / `--out <dir>` | output |
| `--metadata-file <f>` | store an ffmpeg FFMETADATA file's tags in the output (Studio's settings record) |
| `--no-align` | ignore the take's `.align.json` |

A time range, 1:30 → 3:00:

```bash
studio/native/build/StitchPipeline --source take_TS_cam0.mkv --start 2700 --end 5400 --out-file clip.mp4
```

Confirm no frames were lost - the count should be near `fps × seconds` (or
`end − start + 1`):

```bash
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 stitched.mp4
```

---

## How a render runs

One process, as a pipeline: decode → remap + exposure → seam → blend → encode, with
several frames in flight and worker threads taking whichever stage has work. Decode,
seam and encode take frames strictly in order (the smart seam follows the previous
frame's seam), so the seam is one continuous chain over the whole range and the output
is a single file.

The camera files are decoded by ffmpeg with their own colour tags (the cameras record
**full-range BT.709**) and converted exactly. (OpenCV's reader, used until 2026-10-04,
treated them as limited-range BT.601: contrast too high, grass oversaturated and pushed
toward green - panoramas stitched before that date carry that look.)

On an M5 Pro a 5923×1697 crop renders at ~55 fps (just under 2× real time) - the
hardware HEVC encoder's ceiling at that size; the stitching itself is faster. Output is
HEVC in `.mp4`, tagged `hvc1`, bitrate `auto` (~0.20 bpp). `--start` jumps straight to
its frame on an indexed file; otherwise it logs `grab-skipping … (SLOW; remux)`.

**Panorama size** is derived from the calibration, not configured: the cylinder's radius
in pixels equals cam0's focal length, so frame centre is sampled ~1:1 - with this rig's
2104 px focal, about 6800×2370. `--scale` gives fewer pixels at the same field of view;
`--crop` actually crops.
