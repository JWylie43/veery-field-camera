#!/usr/bin/env bash
#
# setup.sh - turn a stock Radxa OS ROCK 5T into the camera rig, then check it.
# Run ON THE ROCK, as the normal user (it uses sudo), from anywhere in the checkout:
#     rock5t-camera/setup.sh            # install whatever is missing, then check
#     rock5t-camera/setup.sh --yes      # same, and reboot without asking if needed
#     rock5t-camera/setup.sh --check    # check only, change nothing
#
# A fresh Rock is: flash Radxa OS, clone this repo, run this, reboot. Every step is
# skipped when it is already done, so re-running is safe - on a finished Rock it
# changes nothing and just runs the checks. It installs, in order:
#   1. apt packages        GStreamer, ffmpeg, tools, and what step 3 builds with
#   2. camera kernel       Radxa's kernel with the IMX477 driver built in (stock
#                          kernels have no IMX477 driver), from this repo's GitHub
#                          Releases; installed beside the stock kernel, which stays
#                          in the boot menu as a fallback; kernel updates are held
#   3. camera overlay      tells the board both cameras are attached + the genlock roles
#   4. IQ tuning file      the camera tuning the 3A daemon loads
#   5. 3A service fix      Radxa's rkaiq_3A.service kills its own daemon at start
#  5b. network settings    the service that applies /config/camera-network.txt (Wi-Fi +
#                          hotspot) at boot - the file itself only ships on the image
#   6. recorder service    the web panel (recorder/server.py), started at every boot
# Then it asks to reboot if the kernel / overlay / tuning changed (the camera checks
# can only pass on the camera kernel), and otherwise runs the checks.
#
# The Mac side is the repo-root ./setup.sh, not this.
# The recorder needs NO pip packages: recorder/server.py is Python 3 standard library
# only, on purpose.

CHECK_ONLY=0; YES=0
for a in "$@"; do
  case "$a" in
    --check) CHECK_ONLY=1 ;;
    --yes|-y) YES=1 ;;
    *) echo "usage: rock5t-camera/setup.sh [--check] [--yes]"; exit 2 ;;
  esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"            # rock5t-camera/
REPO_DIR="$(cd "$HERE/.." && pwd)"

# ---- the camera kernel this rig runs (bump both together when publishing kernel-N+1)
KREL="6.1.84-8-rk2410-imx477"                    # its `uname -r`
KREV=1                                           # the GitHub release: kernel-$KREV
KPKG_VER="$KREL-$KREV"                           # the .deb package version

OVERLAY_SRC="$HERE/overlay/rock-5t-dual-rpi-hq-imx477.dts"
OVERLAY_DST=/boot/dtbo/rock-5t-dual-rpi-hq-imx477.dtbo
IQ_SRC="$HERE/iqfiles/imx477_RPI-HQ_default.json"
IQ_DST=/etc/iqfiles/imx477_RPI-HQ_default.json
AIQ_SRC="$HERE/system/rkaiq_3A-override.conf"
AIQ_DST=/etc/systemd/system/rkaiq_3A.service.d/override.conf
UNIT_SRC="$HERE/recorder/recorder.service"
UNIT_DST=/etc/systemd/system/recorder.service

fail=0; REBOOT=0
step() { printf '\n==> %s\n' "$1"; }
ok()   { printf '    \033[32mOK\033[0m    %s\n' "$1"; }
did()  { printf '    \033[36mDONE\033[0m  %s\n' "$1"; }
warn() { printf '    \033[33mWARN\033[0m  %s\n' "$1"; }
bad()  { printf '    \033[31mMISS\033[0m  %s\n' "$1"; fail=1; }
die()  { printf '\n\033[31mERROR:\033[0m %s\n' "$1"; exit 1; }
same() { [ -f "$2" ] && cmp -s "$1" "$2"; }      # same SRC DST: installed copy is identical

# ---- only on a ROCK 5T (this installs a kernel and boot overlays for that board)
MODEL="$( { tr -d '\0' < /proc/device-tree/model; } 2>/dev/null)"
case "$MODEL" in
  *"ROCK 5T"*|*"Rock 5T"*|*"ROCK5T"*) ;;
  *) [ "$CHECK_ONLY" -eq 1 ] || die "this is for a Radxa ROCK 5T (this board: ${MODEL:-unknown})" ;;
esac
[ "$(id -u)" -ne 0 ] || die "run it as your normal user (not with sudo) - it calls sudo itself"

if [ "$CHECK_ONLY" -eq 0 ]; then
  sudo -v || die "needs sudo"

  # ---------------------------------------------------------------- 1. packages
  # Stock Debian names only. The Rockchip MPP GStreamer plugin (the hardware encoder)
  # ships with the Radxa OS image and its package name varies, so it is checked below
  # rather than installed.
  step "1. Packages"
  PKGS="v4l-utils ffmpeg exfatprogs curl cpp device-tree-compiler
        gstreamer1.0-tools gstreamer1.0-plugins-base
        gstreamer1.0-plugins-good gstreamer1.0-plugins-bad"
  missing=""
  for p in $PKGS; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
  if [ -n "$missing" ]; then
    sudo apt-get update -q && sudo apt-get install -y -q $missing || die "apt install failed:$missing"
    did "installed:$missing"
  else
    ok "all present"
  fi

  # ---------------------------------------------------------------- 2. kernel
  step "2. Camera kernel ($KREL, release kernel-$KREV)"
  have="$(dpkg-query -W -f='${Version}' "linux-image-$KREL" 2>/dev/null)"
  if [ "$have" = "$KPKG_VER" ] && [ "$(uname -r)" = "$KREL" ]; then
    ok "installed and running"
  elif [ "$have" = "$KPKG_VER" ]; then
    ok "installed - not running yet (still on $(uname -r))"
    REBOOT=1
  else
    # the release lives on this repo's GitHub page; the URL comes from the clone's remote
    remote="$(git -C "$REPO_DIR" remote get-url origin 2>/dev/null)"
    slug="$(printf '%s' "$remote" | sed -E 's#^(https://github.com/|git@github.com:)##; s#\.git$##')"
    [ -n "$slug" ] && [ "$slug" != "$remote" ] || die "cannot tell the GitHub repo from the git remote ($remote)"
    tmp="$(mktemp -d)"
    for kind in image headers; do
      f="linux-$kind-${KREL}_${KPKG_VER}_arm64.deb"
      echo "    downloading $f"
      curl -fsSL -o "$tmp/$f" "https://github.com/$slug/releases/download/kernel-$KREV/$f" \
        || die "download failed: https://github.com/$slug/releases/download/kernel-$KREV/$f"
    done
    # --force-hold: the kernel packages are held (below, and on the image) - that stops
    # apt replacing them, and this is the one place meant to update them
    sudo dpkg -i --force-hold "$tmp"/linux-image-*.deb "$tmp"/linux-headers-*.deb || die "kernel install failed"
    rm -rf "$tmp"
    did "installed ${have:+(was $have) }- active after the reboot"
    REBOOT=1
  fi
  # A Radxa kernel update would add a newer stock kernel that boots INSTEAD of ours
  # (newest first) and has no IMX477 driver - hold kernel packages.
  if ! apt-mark showhold | grep -qx "linux-image-$KREL"; then
    sudo apt-mark hold 'linux-image*' 'linux-headers*' >/dev/null && did "kernel updates held"
  fi

  # ---------------------------------------------------------------- 3. overlay
  # Built against the CAMERA kernel's headers (installed with it in step 2), so this
  # works before the reboot, while the stock kernel is still the one running.
  step "3. Camera overlay"
  H="/usr/src/linux-headers-$KREL"
  [ -d "$H/include" ] || die "missing $H - the camera kernel's headers did not install"
  tmp="$(mktemp -d)"
  cpp -nostdinc -I "$H/include" -undef -x assembler-with-cpp "$OVERLAY_SRC" \
    | dtc -q -I dts -O dtb -@ -o "$tmp/overlay.dtbo" - || die "overlay build failed"
  # compare what the overlays SAY (decompiled), not their bytes: an identical overlay
  # built by another dtc version differs in bytes only
  dts_of() { dtc -q -I dtb -O dts -s "$1" 2>/dev/null; }
  if [ -f "$OVERLAY_DST" ] && [ "$(dts_of "$tmp/overlay.dtbo")" = "$(dts_of "$OVERLAY_DST")" ]; then
    ok "installed ($OVERLAY_DST)"
  else
    sudo install -D -m 644 "$tmp/overlay.dtbo" "$OVERLAY_DST"
    did "installed $OVERLAY_DST - active after the reboot"
    REBOOT=1
  fi
  rm -rf "$tmp"
  if [ "$REBOOT" -eq 1 ]; then
    sudo u-boot-update >/dev/null || die "u-boot-update failed"
    did "boot menu updated"
  fi

  # ---------------------------------------------------------------- 4. IQ file
  step "4. IQ tuning file"
  if same "$IQ_SRC" "$IQ_DST"; then
    ok "installed"
  else
    sudo install -D -m 644 "$IQ_SRC" "$IQ_DST"
    did "installed $IQ_DST - loaded when the 3A daemon next starts (reboot)"
    REBOOT=1
  fi

  # ---------------------------------------------------------------- 5. 3A fix
  step "5. 3A service fix"
  if same "$AIQ_SRC" "$AIQ_DST"; then
    ok "installed"
  else
    sudo install -D -m 644 "$AIQ_SRC" "$AIQ_DST"
    # the shipped unit is world-writable
    frag="$(systemctl show -p FragmentPath --value rkaiq_3A 2>/dev/null)"
    [ -n "$frag" ] && [ -f "$frag" ] && sudo chmod 644 "$frag"
    sudo systemctl daemon-reload
    did "installed $AIQ_DST"
    REBOOT=1
  fi
  sudo systemctl enable rkaiq_3A >/dev/null 2>&1

  # ---------------------------------------------------------------- 5b. network settings
  # /config/camera-network.txt (Wi-Fi + hotspot), applied at boot by camera-network.service
  step "5b. Network settings (camera-network.txt)"
  changed=0
  for f in camera-network wifi-fallback; do
    if ! same "$HERE/system/$f" "/usr/local/sbin/$f"; then
      sudo install -m 755 "$HERE/system/$f" "/usr/local/sbin/$f"; changed=1; fi
    if ! same "$HERE/system/$f.service" "/etc/systemd/system/$f.service"; then
      sudo install -m 644 "$HERE/system/$f.service" "/etc/systemd/system/$f.service"; changed=1; fi
  done
  if [ "$changed" -eq 1 ]; then
    sudo systemctl daemon-reload
    did "installed camera-network + wifi-fallback (known Wi-Fi first, else the hotspot)"
  else
    ok "installed"
  fi
  sudo systemctl enable camera-network wifi-fallback >/dev/null 2>&1
  # The template is NOT dropped in here: its default hotspot (open unless a password is
  # set) would appear next to a Rock's existing networks. To use it on this Rock: copy
  # rock5t-camera/system/camera-network.txt to /config/, fill it in, reboot.

  # ---------------------------------------------------------------- 6. recorder
  # The unit file has a @REPO_DIR@ placeholder; the installed copy gets this
  # checkout's real path. Only restarted when it changed (a restart ends a take).
  step "6. Recorder service"
  tmp="$(mktemp)"
  sed -e "s#@REPO_DIR@#$REPO_DIR#g" "$UNIT_SRC" > "$tmp"
  if same "$tmp" "$UNIT_DST"; then
    ok "installed"
  else
    sudo install -m 644 "$tmp" "$UNIT_DST"
    sudo systemctl daemon-reload
    [ "$REBOOT" -eq 0 ] && sudo systemctl restart recorder
    did "installed $UNIT_DST"
  fi
  rm -f "$tmp"
  sudo systemctl enable recorder >/dev/null 2>&1
  [ "$REBOOT" -eq 0 ] && ! systemctl is-active --quiet recorder && sudo systemctl start recorder

  # ---------------------------------------------------------------- reboot?
  if [ "$REBOOT" -eq 1 ]; then
    echo
    echo "Installed. A reboot starts the camera kernel, overlay and tuning; the recorder"
    echo "then starts by itself at http://<rock-ip>:8080. After it, confirm with:"
    echo "    rock5t-camera/setup.sh --check"
    if [ "$YES" -eq 1 ]; then ans=y
    elif [ -t 0 ]; then read -r -p "Reboot now? [Y/n] " ans; ans="${ans:-y}"
    else ans=n                                   # no terminal to ask: never reboot unasked
    fi
    case "$ans" in [Yy]*) sudo reboot; exit 0 ;; esac
    echo "Not rebooted - the camera checks will fail until you do."
    exit 0
  fi
fi

# ==================================================================== checks
step "Kernel + boot"
if [ "$(uname -r)" = "$KREL" ]; then
  ok "running the camera kernel ($KREL)"
else
  bad "running $(uname -r), not the camera kernel $KREL - run rock5t-camera/setup.sh (and reboot)"
fi
first="$(grep -m1 -E '^\s*(linux|kernel)\s' /boot/extlinux/extlinux.conf 2>/dev/null)"
case "$first" in
  *"$KREL"*) ok "boots the camera kernel by default" ;;
  "")        warn "no /boot/extlinux/extlinux.conf - cannot check the boot default" ;;
  *)         bad "the boot menu's default is not the camera kernel ($first) - sudo u-boot-update" ;;
esac
[ -f "$OVERLAY_DST" ] && ok "camera overlay installed" || bad "camera overlay missing ($OVERLAY_DST)"
[ "$(cat /sys/module/imx477/parameters/genlock 2>/dev/null)" = "Y" ] \
  && ok "genlock enabled" || warn "imx477 genlock parameter not Y"

# Radxa's camera software has to match the kernel's camera drivers, and the IQ file
# is written for this rkaiq version's format - see "Tested Rock setup" in the README.
step "Radxa camera software (tested versions)"
for pv in "camera-engine-rkaiq 6.8.0-rk3588" "librockchip-mpp1 1.5.0-1" "gstreamer1.0-rockchip1 1.14-4"; do
  p="${pv% *}"; want="${pv#* }"
  got="$(dpkg-query -W -f='${Version}' "$p" 2>/dev/null)"
  if [ "$got" = "$want" ]; then ok "$p $got"
  elif [ -z "$got" ]; then warn "$p not installed (tested: $want)"
  else warn "$p $got - tested with $want; if the cameras misbehave, this is the first suspect"
  fi
done

step "Tools + GStreamer"
for c in gst-launch-1.0 v4l2-ctl media-ctl ffmpeg ffprobe python3; do
  command -v "$c" >/dev/null 2>&1 && ok "$c" || bad "$c not on PATH"
done
for e in v4l2src videorate jpegenc multifilesink matroskamux filesink queue h265parse h264parse; do
  gst-inspect-1.0 "$e" >/dev/null 2>&1 && ok "$e" || bad "GStreamer element $e"
done
for e in mpph265enc mpph264enc; do
  gst-inspect-1.0 "$e" >/dev/null 2>&1 && ok "$e" \
    || bad "$e - the Rockchip MPP plugin (hardware encoder) is missing. It ships with the
          Radxa OS image; without it recording will not run."
done

step "Camera pipeline"
# The driver is built INTO the kernel, so lsmod shows nothing - ask sysfs. Bound
# sensors appear in the driver directory as <bus>-<addr> links.
DRV=/sys/bus/i2c/drivers/imx477
[ -d "$DRV" ] && ok "imx477 driver registered" || bad "imx477 driver not registered (not on the camera kernel?)"
bound=$(ls -1 "$DRV" 2>/dev/null | grep -E '^[0-9]+-00[0-9a-f]+$' | tr '\n' ' ')
nb=$(echo $bound | wc -w | tr -d " ")
if [ "$nb" -eq 2 ]; then ok "both sensors bound: $bound"
elif [ "$nb" -gt 0 ]; then bad "only $nb sensor bound ($bound), want 2 (3-001a + 4-001a) - check the other ribbon"
else bad "no sensor bound - check the ribbons and the overlay (sudo i2cdetect -y 3: UU at 0x1a = bound)"
fi
mainpaths=$(for v in /sys/class/video4linux/video*; do
              grep -q rkisp_mainpath "$v/name" 2>/dev/null && echo "/dev/$(basename "$v")"
            done | sort | tr '\n' ' ')
n=$(echo $mainpaths | wc -w | tr -d " ")
[ "$n" -eq 2 ] && ok "2 ISP mainpath nodes: $mainpaths" \
               || bad "found $n ISP mainpath nodes, want 2: ${mainpaths:-none} (v4l2-ctl --list-devices)"
# The shipped unit can read "active" with the daemon dead - check the process.
pgrep -x rkaiq_3A_server >/dev/null 2>&1 && ok "3A daemon (rkaiq_3A_server) running" \
  || bad "3A daemon not running - is $AIQ_DST installed? (rock5t-camera/setup.sh)"
same "$IQ_SRC" "$IQ_DST" && ok "IQ tuning file installed (current)" \
  || bad "IQ tuning file missing or out of date ($IQ_DST) - run rock5t-camera/setup.sh"

step "Recorder service"
if [ ! -f "$UNIT_DST" ]; then
  bad "recorder.service not installed - run rock5t-camera/setup.sh"
else
  exec_path=$(sed -n 's/^ExecStart=[^ ]* \(.*\)$/\1/p' "$UNIT_DST" | head -1)
  if [ -n "$exec_path" ] && [ ! -f "$exec_path" ]; then
    bad "recorder.service points at $exec_path, which does not exist (checkout moved?) - re-run rock5t-camera/setup.sh"
  else
    ok "installed ($exec_path)"
  fi
  systemctl is-enabled --quiet recorder 2>/dev/null && ok "starts at boot" || bad "not enabled at boot (sudo systemctl enable recorder)"
  systemctl is-active --quiet recorder 2>/dev/null && ok "running" \
    || warn "not running (sudo systemctl start recorder) - normal if you stopped it to use the cameras by hand"
fi

step "Storage"
OWNER_HOME="$(getent passwd "$(stat -c %U "$REPO_DIR")" | cut -d: -f6)"
REC_DIR="${OWNER_HOME:-$HOME}/recordings"
if [ -d "$REC_DIR" ]; then
  ok "$REC_DIR ($(df -h "$REC_DIR" | awk 'NR==2{print $4}') free)"
else
  warn "$REC_DIR does not exist yet - the panel creates it on the first take"
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "All checks passed.  Recorder: http://<rock-ip>:8080   (logs: journalctl -u recorder -f)"
else
  echo "Some checks FAILED (MISS above) - recording will not work until they pass."
fi
exit "$fail"
