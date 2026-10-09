# Veery Studio

One local web app for a folder of takes: **align** them, **stitch** them, **edit** the
results. It runs on the Mac and opens in the browser; nothing leaves the machine
(it listens on 127.0.0.1 only).

```bash
studio/studio.command                       # or double-click it in Finder
studio/studio.command --takes /path/to/takes --port 8100
```

The first run creates Studio's own Python environment (`studio/.venv`, from
`requirements.txt` via `setup.sh`). Needs `ffmpeg`/`ffprobe` on PATH and the two
C++ tools built in `stitching/build/` (`cd stitching && cmake -S . -B build && cmake --build build`).

## Pages

| Page | What it is |
|---|---|
| `/` | The takes folder. **Camera pairs** (`take_…_cam0` + `_cam1`): tick several → **Align selected**, or a row's **Align** / **Stitch →**. **Stitched videos** (only videos Studio stitched, see below): **Edit →**. Every row shows its job's live status; **Details** has the alignment result, frame-timing check, settings and each job's full log. |
| `/stitch/<take>` | Reached from a pair's **Stitch →**. The stitcher's tuner on that pair: any frame (slider / box, `,` `.` step), shift far/near (shear), rotation, crop box, In/Out points, output name. Loads the take's `.align.json` (rotation + default shear) or the base calibration. **Stitch** starts the job. |
| `/edit/<video>` | Reached from a stitched video's **Edit →**. The Director (cut points, steer the virtual camera). The edit autosaves next to the video (`name.director.json`, backups in `name.director-backups/`); **Render…** starts the job. |

The take or video is in the URL, so a refresh, a bookmark or a second tab all
open the same thing; nothing about "the current take" lives in the server.

## Jobs

Each align / stitch / render is **one process** - the same command you would
type in a terminal - started by the server, which keeps its output as the job's
log and turns its progress lines into a percent. The pages poll `GET /api/jobs`
(once a second); a job keeps running if you close or reload the page, and its
status shows on the takes list and on its own page.

| Job | Command | At once |
|---|---|---|
| align | `studio/refine_extrinsics.py --align <cam0>` | 2 (the rest queue) |
| stitch | `stitching/build/StitchPipeline --source … --metadata-file …` | 1 (a second is refused) |
| render | `stitching/build/Director --render <project> …` | 1 |

A take can't be aligned while it is stitching (or the reverse). **Cancel** stops
the process and its ffmpeg children; a cancelled stitch's partial file is removed.
Job history lives in memory - a server restart forgets it (the files it made stay).

## What a stitched video records

The stitch job writes this into the MP4's `comment` tag (JSON), which is also how
the takes list knows a video is a stitch it can edit:

```json
{ "veery": "stitch", "version": 1, "take": "take_20261003_165256",
  "sources": ["take_…_cam0.mkv", "take_…_cam1.mkv"], "stitched": "2026-10-08T20:46:00",
  "calibration": { "base": "calibration/stereo_extrinsics.json",
                   "take_alignment": "take_20261003_165256.align.json",
                   "corrections_from_base_deg": {…}, "rotation_matrix": [[…]] },
  "pairing": { "offset": 0, "method": "timestamps", "detail": "…" },
  "panorama": [6823, 2371],
  "settings": { "degrees": 0.4, "crop": [341, 119, 6141, 2134], "shift_top": -5.4,
                "shift_bottom": 15.3, "start": 3000, "end": 3059, … },
  "command": "…/StitchPipeline --source … --out-file …" }
```

Read it back with
`ffprobe -v error -show_entries format_tags=comment -of default=nw=1:nk=1 video.mp4`.
Videos stitched before Studio (or with an old StitchPipeline build without
`--metadata-file`) have no record and are not listed.

## The stitch preview

The preview is the stitcher's own warp, recomputed in Python (`rig.py` +
`preview.py`): same cylinder canvas, same fisheye model, same left/right choice,
same frame pairing (`--pair-offset` from the capture timestamps, passed to the
stitcher explicitly). Checked against StitchPipeline output: identical canvas
size and seam, alignment within 0.003 px, same frame index at any position.
Like the tuner, it shows the plain warp - the smart seam, 6-band blend and
exposure match happen in the stitch.

## Files

```
studio/
├── server.py             the web app: pages, API, starts jobs
├── jobs.py               one process per job, run in a pseudo-terminal; progress parsers
├── library.py            scans the takes folder (pairs, stitched videos, frame-timing check)
├── preview.py            the stitch page's frames
├── rig.py                take files, calibration, pairing and the stitcher's geometry
├── refine_extrinsics.py  per-take alignment (also usable on its own, see the main README)
├── static/               index.html (takes), stitch.html, edit.html (the Director's page, wired to Studio), studio.css
├── requirements.txt, setup.sh, studio.command
```
