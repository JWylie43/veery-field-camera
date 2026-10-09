# Editing: points and the virtual camera

The editor (the **Director**) turns the full-width panorama into a normal 16:9 video:
you cut the game into points, then steer a "camera" box over each point while it plays.
Quick start is in the [README](../README.md).

On Studio's takes list, a stitched video's **Edit →** opens it in the editor
(`/edit/<video>`). The edit saves itself within a second of every change, next to the
video as `name.director.json` (reopen it to resume), with timestamped backups (at most
one per 5 minutes, the last 20) in `name.director-backups/`.

## 1 · Cut

Play or scrub, press **I** where a point starts and **O** where it ends. Points show as
bars on the timeline, numbered in game order; only they end up in the output. Points
can't overlap. Select a point to relabel it, move its In/Out to the playhead, or delete
it.

## 2 · Frame

Select a point and press **R**: playback starts 3 s before the In (pre-roll, to line up
the box), recording runs from In to Out by itself, then stops.

- The box follows the mouse left/right with its bottom on a fixed line; hold **Shift**
  to move it up/down too.
- It starts at the output size: zoom **1.00× = 1:1**, the sharpest framing (below 1 is
  wider and still sharp, above 1 is upscaled and soft - the readout under the video
  shows which).
- **Scroll** zooms ~4% per notch (**Alt** finer, **−**/**=** 2% steps); **0** or
  double-click resets to 1:1; **Shift+0** jumps to the widest full-height box.
  Scrolling out past that (it stops there once first) goes wider still: the box gets
  taller than the panorama and the output is letterboxed, up to the full panorama width.
- One take covers the whole point; **Esc** abandons a take in progress.

**Takes are protected:** recording is never undone by **Cmd/Ctrl+Z** (undo covers only
adding / deleting / moving points, and asks before removing a point that has a take).
Re-recording, clearing or trimming keeps the old take in the point's history -
**↺ Previous take** brings it back. Shortening a framed point trims its take; lengthening
it marks the point *needs re-take*.

Each take is smoothed after recording - jitter and small back-and-forths removed, the
path following your general movement without lag. Two sliders per point adjust it
(**Smoothing** = how much, **Steady** = how big a wiggle is ignored); they re-smooth
without re-recording.

**V** switches to the big output view; **P** previews the selected point as it will
render; **Preview all** plays every point back to back. **?** lists every key.

## Render

**Render…** writes one video with the points in order, or one file per point
(`name_01_Point_1.mp4`, …), at the chosen output size (1440p default), saved in the takes
folder. It runs as a Studio job - you can leave the page; progress shows on the takes
list too.

- Quality: **H.264 high** (default, plays everywhere), **HEVC high** (same quality,
  ~20% smaller) or **H.264 standard** (smaller, slightly softer).
- Unframed points are left out by default (or rendered as a full-height wide shot).
- Frames stay in the panorama's own YUV from decode to encode (no colour conversion) and
  are scaled bicubically with anti-aliasing, so a 1:1 box is a bit-exact crop before
  encoding.
- With a ~1700 px tall panorama a full-height box is ~3000 px wide, so 1440p has ~1.18×
  zoom before it upscales (1080p: ~1.57×).

The render is the `Director` command-line tool, which also runs by hand:

```bash
studio/native/build/Director --render take.director.json --out game_edit.mp4 [--per-point] \
    [--unframed skip|wide] [--codec h264|hevc] [--quality high|standard]
```
