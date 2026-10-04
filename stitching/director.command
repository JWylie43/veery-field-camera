#!/usr/bin/env bash
#
# director.command — double-click launcher for the Director (virtual-camera editor).
#
# Opens the editor in the browser: "Open video…" loads a stitched panorama, then
# 1 · Cut marks the points of the game (I / O) and 2 · Frame steers a 16:9 box over
# each point. The edit saves itself next to the video (name.director.json);
# "Render…" writes the finished 1440p video. Builds on first run.
set -e
cd "$(dirname "$0")"

if [ ! -x build/Director ]; then
  echo "==> Building Director (first run)…"
  cmake -S . -B build >/dev/null
  cmake --build build --target Director >/dev/null
fi

exec ./build/Director "$@"
