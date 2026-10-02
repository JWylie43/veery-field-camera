#!/usr/bin/env bash
# snap_pair.sh - capture ONE calibration pair (cam0 + cam1 full-res stills).
#
# Run on the ROCK, once per ChArUco board pose:
#     ./snap_pair.sh
# Saves ~/calib/cam0_NNN.png + cam1_NNN.png (auto-numbered). Hold the board
# STILL for the ~4 seconds this takes - captures are sequential, and a static
# board needs no genlock (but sequential capture HANGS in sync mode - a sink
# camera gets no frames once cam0 stops; set genlock=0 first, or use the
# panel's /calib page, which captures both together).
# Images are rotated 180 (hflip,vflip) to match the recorder's orientation.
#
# Afterwards pull to the Mac and solve (from the repo's calibration/ dir).
# These ARE simultaneous pairs, so they are what the EXTRINSICS need:
#     scp -r radxa@veery.local:~/calib images-pairs
#     python3 calibrate.py --use-intrinsics . \
#                          --cam0-glob 'images-pairs/cam0_*.png' \
#                          --cam1-glob 'images-pairs/cam1_*.png'
set -euo pipefail

OUT=${OUT:-$HOME/calib}
mkdir -p "$OUT"
n=$(ls "$OUT"/cam0_*.png 2>/dev/null | wc -l)
idx=$(printf '%03d' "$n")

for c in 0 1; do
  if [ "$c" = 0 ]; then dev=/dev/video22; else dev=/dev/video31; fi
  v4l2-ctl -d "$dev" --set-fmt-video=width=3840,height=2160,pixelformat=NV12 \
    --stream-mmap --stream-count=45 --stream-to=/tmp/cal.nv12
  ffmpeg -loglevel error -f rawvideo -pix_fmt nv12 -s 3840x2160 -i /tmp/cal.nv12 \
    -vf hflip,vflip -update 1 -y "$OUT/cam${c}_${idx}.png"
done

echo "pair $idx saved -> $OUT  ($(ls "$OUT" | wc -l) files total)"
