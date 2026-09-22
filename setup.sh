#!/usr/bin/env bash
#
# setup.sh - check (and optionally install) what the ROCK 5T rig needs to record.
# Run ON THE ROCK:
#     ./setup.sh            # check, and apt-install anything missing
#     ./setup.sh --check    # check only, install nothing
#
# On a stock Radxa OS image most of this is ALREADY present - the vendor image
# ships the GStreamer + Rockchip MPP stack. The half of this script that earns
# its keep is the verification below: driver bound, sensors on I2C, two ISP
# mainpath nodes, the rkaiq daemon up and the IQ file installed. Those are what
# actually go wrong.
#
# This does NOT build the kernel driver or the device-tree overlay. Do that
# first - see rock5t-camera/driver/NOTES.md and ROCK5T_CAMERA.md - then run this
# to confirm the result.
#
# The Mac-side tools are not covered here:
#   calibration/  -> python3 + opencv-contrib-python   (see README)
#   stitching/    -> cmake + OpenCV + ffmpeg           (see README)
#
# The recorder panel itself needs NO pip packages: veery_server.py and
# calib_server.py are Python 3 standard library only, on purpose.

CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

fail=0
ok()   { printf '    \033[32mOK\033[0m    %s\n' "$1"; }
warn() { printf '    \033[33mWARN\033[0m  %s\n' "$1"; }
bad()  { printf '    \033[31mMISS\033[0m  %s\n' "$1"; fail=1; }

# ---------------------------------------------------------------- packages
# Only stock Debian package names here. The Rockchip MPP GStreamer plugin is
# deliberately NOT apt-installed: it comes from the vendor image / Radxa's repo
# and the package name varies by image, so guessing it would just fail the run.
# It is checked for below instead.
PKGS="v4l-utils ffmpeg exfatprogs
      gstreamer1.0-tools gstreamer1.0-plugins-base
      gstreamer1.0-plugins-good gstreamer1.0-plugins-bad"

if [ "$CHECK_ONLY" -eq 0 ]; then
  missing_pkgs=""
  for p in $PKGS; do
    dpkg -s "$p" >/dev/null 2>&1 || missing_pkgs="$missing_pkgs $p"
  done
  if [ -n "$missing_pkgs" ]; then
    echo "==> Installing:$missing_pkgs"
    sudo apt-get update
    # shellcheck disable=SC2086
    sudo apt-get install -y $missing_pkgs
  else
    echo "==> All apt dependencies already installed."
  fi
  echo
fi

# ---------------------------------------------------------------- tools
echo "==> Command-line tools"
for c in gst-launch-1.0 v4l2-ctl media-ctl ffmpeg ffprobe python3; do
  command -v "$c" >/dev/null 2>&1 && ok "$c" || bad "$c not on PATH"
done

# ---------------------------------------------------------------- gst elements
echo
echo "==> GStreamer elements used by the record + preview pipelines"
for e in v4l2src videorate jpegenc multifilesink matroskamux filesink queue; do
  gst-inspect-1.0 "$e" >/dev/null 2>&1 && ok "$e" || bad "$e"
done
# h26xparse live in plugins-bad; the record pipeline will not link without them
for e in h265parse h264parse; do
  gst-inspect-1.0 "$e" >/dev/null 2>&1 && ok "$e" || bad "$e (gstreamer1.0-plugins-bad)"
done
# The hardware encoder - vendor-supplied, not a stock Debian package
for e in mpph265enc mpph264enc; do
  if gst-inspect-1.0 "$e" >/dev/null 2>&1; then
    ok "$e"
  else
    bad "$e - the Rockchip MPP plugin is missing. It ships with the Radxa OS
          image; on a plain Debian install add Radxa's apt repo and install
          their gstreamer-rockchip package. Without it there is no hardware
          encoder and recording will not run."
  fi
done

# ---------------------------------------------------------------- camera stack
echo
echo "==> Camera pipeline"

# Ask sysfs, not lsmod: on this rig the driver is COMPILED IN
# (CONFIG_VIDEO_IMX477=y - see rock5t-camera/kernel-build/README.md), so lsmod
# shows nothing even when it is working. The driver directory exists either way.
# Bound sensors appear inside it as <bus>-<addr> symlinks. Also avoids i2cdetect,
# which needs root and would report a false miss under a plain ./setup.sh.
DRV=/sys/bus/i2c/drivers/imx477
if [ -d "$DRV" ]; then
  ok "imx477 driver registered"
else
  bad "imx477 driver not registered - see rock5t-camera/driver/NOTES.md"
fi

bound=$(ls -1 "$DRV" 2>/dev/null | grep -E '^[0-9]+-00[0-9a-f]+$' | tr '\n' ' ')
nb=$(echo $bound | wc -w | tr -d " ")
if [ "$nb" -eq 2 ]; then
  ok "both sensors bound: $bound"
elif [ "$nb" -gt 0 ]; then
  bad "only $nb sensor bound ($bound), want 2 (3-001a + 4-001a) - check the other ribbon"
else
  bad "no sensor bound to the imx477 driver - check the ribbons + overlay
          manual look: sudo i2cdetect -y 3   (UU at 0x1a = bound)"
fi

mainpaths=$(for v in /sys/class/video4linux/video*; do
              grep -q rkisp_mainpath "$v/name" 2>/dev/null && echo "/dev/$(basename "$v")"
            done | sort | tr '\n' ' ')
n=$(echo $mainpaths | wc -w | tr -d " ")
[ "$n" -eq 2 ] && ok "2 rkisp mainpath nodes: $mainpaths" \
               || bad "found $n rkisp mainpath nodes, want 2: ${mainpaths:-none}
          check: v4l2-ctl --list-devices"

# rkaiq_3A.service is a oneshot wrapper that kills its own daemon at start, so
# the UNIT can read "active" with nothing actually running. Check the process.
if pgrep -x rkaiq_3A_server >/dev/null 2>&1; then
  ok "rkaiq_3A_server running"
else
  bad "rkaiq_3A_server not running - the ISP needs it (sudo rkaiq_3A_server &)
          note: the systemd unit is unreliable here, see rock5t-camera/NEXT_STEPS.md"
fi

[ -f /etc/iqfiles/imx477_RPI-HQ_default.json ] \
  && ok "IQ file installed" \
  || bad "/etc/iqfiles/imx477_RPI-HQ_default.json missing
          cp rock5t-camera/iqfiles/imx477_RPI-HQ_default.json /etc/iqfiles/"

# ---------------------------------------------------------------- storage
echo
echo "==> Storage"
REC_DIR="$HOME/recordings"
if [ -d "$REC_DIR" ]; then
  ok "$REC_DIR present ($(df -h "$REC_DIR" | awk 'NR==2{print $4}') free)"
else
  warn "$REC_DIR does not exist yet - the panel creates it on first record"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "All checks passed."
else
  echo "Some checks FAILED (see MISS above) - recording will not work until they pass."
fi
echo
echo "Record a test take:  cd rock5t-camera/recorder && DUR=10 ./record_dual.sh"
echo "Web panel:           sudo python3 rock5t-camera/recorder/veery_server.py"
echo "                     (then http://<rock-ip>:8080)"
exit "$fail"
