#!/usr/bin/env bash
#
# setup.sh - set up the Mac side of this repo (Studio, alignment, calibration).
# macOS / Linux only (tested on macOS); Windows is not supported.
#     ./setup.sh
# 1. creates the repo's Python environment, .venv/, and installs requirements.txt
#    into it - every Python script here runs with .venv/bin/python;
# 2. builds Studio's C++ tools (studio/native -> studio/native/build/).
# Safe to re-run: it updates the packages and rebuilds what changed.
#
# Needs (Homebrew): python3, cmake, opencv, ffmpeg; and Xcode's command-line tools
# for the compiler (xcode-select --install).
# The Rock has its own check script: rock5t-camera/setup.sh (run on the Rock).
set -e
cd "$(dirname "$0")"

missing=""
for tool in python3 cmake ffmpeg ffprobe; do
  command -v "$tool" >/dev/null || missing="$missing $tool"
done
if [ -n "$missing" ]; then
  echo "Missing:$missing"
  echo "Install with Homebrew:  brew install python cmake opencv ffmpeg"
  exit 1
fi

echo "==> Python environment (.venv)"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip -q
.venv/bin/python -m pip install -r requirements.txt -q
.venv/bin/python -c "import cv2, numpy; print('    OpenCV', cv2.__version__, '· numpy', numpy.__version__)"

echo "==> Studio's C++ tools (studio/native/build)"
if ! cmake -S studio/native -B studio/native/build >/dev/null; then
  echo "    CMake could not configure the build - is OpenCV installed?  brew install opencv"
  exit 1
fi
cmake --build studio/native/build -j >/dev/null
ls studio/native/build/StitchPipeline studio/native/build/Director | sed 's/^/    /'

echo "Ready. Start Studio with:  studio/studio.command"
