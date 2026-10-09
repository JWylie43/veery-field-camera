#!/usr/bin/env bash
#
# build-inside.sh - the image build itself. Runs INSIDE the Docker container that
# build-image.sh starts (debian:bookworm, arm64 - the Rock's CPU, so the image's own
# programs run natively in a chroot). Not meant to be run by hand.
#
# It does to Radxa's image what rock5t-camera/setup.sh does on a running Rock - kernel,
# overlay, tuning, 3A fix, recorder, packages, kernel hold - plus a copy of this repo
# and a one-time first-boot step, then compresses the result.
#
# Mounts:  /work  Radxa's unpacked image (radxa.img) + scratch     /kernel  the kernel .debs
#          /repo  this repo (read-only)                            /out     the finished image
set -euo pipefail
: "${KREL:?}" "${KREV:?}" "${IMAGE_REV:?}" "${REPO_NAME:?}" "${REPO_URL:?}" "${BASE_IMAGE:?}"
RIG_USER=radxa                       # Radxa's default user, created by its first boot
GROW_MB=2048                         # room for the kernel + packages (Radxa's root has ~1 GB free)
IMG=/work/build.img
OUT=/out/rock5t-camera-image-$IMAGE_REV.img.xz
HERE=/repo/rock5t-camera
R=/r                                 # the image's root filesystem, mounted

log() { printf '\n==> %s\n' "$*"; }
in_image() { chroot "$R" env DEBIAN_FRONTEND=noninteractive NEEDRESTART_SUSPEND=1 LC_ALL=C "$@"; }

LOOPS=(); MOUNTS=()
cleanup() {
  for ((i=${#MOUNTS[@]}-1; i>=0; i--)); do
    umount "${MOUNTS[i]}" 2>/dev/null || umount -l "${MOUNTS[i]}" 2>/dev/null || true
  done
  for l in "${LOOPS[@]}"; do losetup -d "$l" 2>/dev/null || true; done
}
trap cleanup EXIT
mnt() { mount "$@"; MOUNTS+=("${@: -1}"); }
part() {   # part N -> "start size" of partition N, in 512-byte sectors
  sfdisk -d "$IMG" | sed -nE "s#^$IMG$1 : start= *([0-9]+), size= *([0-9]+).*#\1 \2#p"
}
loop_of() { # loop_of N -> a loop device for partition N
  read -r s z < <(part "$1")
  local l; l=$(losetup -f --show -o $((s * 512)) --sizelimit $((z * 512)) "$IMG")
  LOOPS+=("$l"); echo "$l"
}

log "Build tools"
apt-get update -qq && apt-get install -y -qq fdisk gdisk e2fsprogs zerofree xz-utils git >/dev/null

# ------------------------------------------------------------------ the image, with room
log "Copy Radxa's image ($BASE_IMAGE) and grow its root partition by $GROW_MB MB"
cp --sparse=always /work/radxa.img "$IMG"
truncate -s +"${GROW_MB}M" "$IMG"
sgdisk -e "$IMG" >/dev/null                          # backup GPT header -> the new end
# The config partition (FAT, where camera-network.txt is edited) is typed "Linux
# filesystem" in Radxa's layout, so a Mac or PC won't mount it after flashing. Type it as
# a standard data partition instead; the Rock mounts it by filesystem UUID (fstab), so
# nothing changes there.
sgdisk -t 1:0700 "$IMG" >/dev/null
echo ", +" | sfdisk --no-reread --no-tell-kernel -N 3 "$IMG" >/dev/null 2>&1
ROOT=$(loop_of 3)
e2fsck -fy "$ROOT" >/dev/null || true
resize2fs "$ROOT" >/dev/null
mkdir -p "$R"
mnt "$ROOT" "$R"
mnt "$(loop_of 2)" "$R/boot/efi"
mnt "$(loop_of 1)" "$R/config"
for d in dev dev/pts proc sys; do mnt --bind "/$d" "$R/$d"; done
cp /etc/resolv.conf "$R/etc/resolv.conf"             # the image has none (NetworkManager makes it)

# ------------------------------------------------------------------ 1. packages
# Same list as rock5t-camera/setup.sh step 1 (most are in Radxa's image already).
log "1. Packages"
in_image apt-get update -q
in_image apt-get install -y -q --no-install-recommends \
  v4l-utils ffmpeg exfatprogs curl cpp device-tree-compiler \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad

# ------------------------------------------------------------------ 2. kernel
log "2. Camera kernel $KREL (kernel-$KREV)"
cp /kernel/linux-image-"${KREL}"_*.deb /kernel/linux-headers-"${KREL}"_*.deb "$R/tmp/"
in_image sh -c 'dpkg -i /tmp/linux-image-*.deb /tmp/linux-headers-*.deb'
rm -f "$R"/tmp/linux-*.deb
# a Radxa kernel update would add a newer stock kernel that boots instead of ours
in_image apt-mark hold 'linux-image*' 'linux-headers*' >/dev/null

# ------------------------------------------------------------------ 3. overlay
log "3. Camera overlay"
cp "$HERE/overlay/rock-5t-dual-rpi-hq-imx477.dts" "$R/tmp/overlay.dts"
in_image sh -c "cpp -nostdinc -I /usr/src/linux-headers-$KREL/include -undef -x assembler-with-cpp /tmp/overlay.dts \
  | dtc -q -I dts -O dtb -@ -o /boot/dtbo/rock-5t-dual-rpi-hq-imx477.dtbo -"
rm -f "$R/tmp/overlay.dts"

# ------------------------------------------------------------------ 4. IQ, 5. 3A fix
log "4. IQ tuning file, 5. 3A service fix"
install -D -m 644 "$HERE/iqfiles/imx477_RPI-HQ_default.json" "$R/etc/iqfiles/imx477_RPI-HQ_default.json"
install -D -m 644 "$HERE/system/rkaiq_3A-override.conf" "$R/etc/systemd/system/rkaiq_3A.service.d/override.conf"
chmod 644 "$R/lib/systemd/system/rkaiq_3A.service"   # shipped world-writable
in_image systemctl enable rkaiq_3A.service >/dev/null 2>&1

# ------------------------------------------------------------------ the repo
# A real git checkout (so the Rock can `git pull`), staged in /opt: the user it belongs
# to does not exist until the first boot, when rig-firstboot moves it into their home.
log "Repo copy ($REPO_NAME)"
git config --global --add safe.directory '*'
DEST="$R/opt/$REPO_NAME"
git clone -q /repo "$DEST"
# a test build of a tree with uncommitted changes carries them (build-image.sh only
# allows that with ALLOW_DIRTY=1)
(cd /repo && git ls-files -co --exclude-standard -z | tar --null -T - -cf -) | tar -C "$DEST" -xf -
(cd /repo && git ls-files -d -z) | (cd "$DEST" && xargs -0 -r rm -f)
git -C "$DEST" remote set-url origin "$REPO_URL"
COMMIT=$(git -C /repo rev-parse --short HEAD)
DIRTY=$([ -n "$(git -C /repo status --porcelain)" ] && echo " +uncommitted changes" || true)

# ------------------------------------------------------------------ 6. recorder + first boot
log "6. Recorder service + first-boot step"
sed -e "s#@REPO_DIR@#/home/$RIG_USER/$REPO_NAME#g" "$HERE/recorder/recorder.service" \
  > "$R/etc/systemd/system/recorder.service"
install -D -m 644 "$HERE/image-build/firstboot/recorder-after-firstboot.conf" \
  "$R/etc/systemd/system/recorder.service.d/after-firstboot.conf"
subst() { sed -e "s#@RIG_USER@#$RIG_USER#g" -e "s#@REPO_NAME@#$REPO_NAME#g" "$1"; }
subst "$HERE/image-build/firstboot/rig-firstboot.sh" > "$R/usr/local/sbin/rig-firstboot"
chmod 755 "$R/usr/local/sbin/rig-firstboot"
subst "$HERE/image-build/firstboot/rig-firstboot.service" > "$R/etc/systemd/system/rig-firstboot.service"
in_image systemctl enable recorder.service rig-firstboot.service >/dev/null 2>&1

# ------------------------------------------------------------------ network settings
log "Network settings (camera-network.txt on the config partition)"
install -m 755 "$HERE/system/camera-network" "$R/usr/local/sbin/camera-network"
install -m 644 "$HERE/system/camera-network.service" "$R/etc/systemd/system/camera-network.service"
in_image systemctl enable camera-network.service >/dev/null 2>&1
cp "$HERE/system/camera-network.txt" "$R/config/camera-network.txt"   # the blank template

if [ -n "${RIG_HOSTNAME:-}" ]; then
  log "Hostname $RIG_HOSTNAME"
  echo "$RIG_HOSTNAME" > "$R/etc/hostname"
  sed -i -E "s/^127\.0\.1\.1\s.*/127.0.1.1\t$RIG_HOSTNAME/" "$R/etc/hosts"
fi

cat > "$R/etc/camera-rig-image" <<EOF
IMAGE_REV=$IMAGE_REV
BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)
BASE_IMAGE=$BASE_IMAGE
KERNEL=$KREL (kernel-$KREV)
REPO=$REPO_URL @ $COMMIT$DIRTY
EOF

# ------------------------------------------------------------------ boot menu, checked
log "Boot menu"
in_image u-boot-update
EXT="$R/boot/extlinux/extlinux.conf"
first=$(grep -m1 -E '^\s*linux\s' "$EXT")
case "$first" in *"$KREL"*) echo "    default: $first" ;; *) echo "boot default is not the camera kernel: $first"; exit 1 ;; esac
grep -q "rock-5t-dual-rpi-hq-imx477.dtbo" "$EXT" || { echo "the camera overlay is not in the boot menu"; exit 1; }
grep -q "root=UUID=$(blkid -s UUID -o value "$ROOT")" "$EXT" || { echo "boot menu root= is not this image's root"; exit 1; }
echo "    camera overlay and root partition present"

# ------------------------------------------------------------------ clean + pack
log "Clean up"
in_image apt-get clean
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/log/journal/* "$R"/tmp/*
rm -f "$R/etc/resolv.conf" "$R/etc/machine-id" "$R/var/lib/dbus/machine-id"   # as Radxa ships it
cat "$R/etc/camera-rig-image"
cleanup; MOUNTS=(); LOOPS=()
ROOT=$(loop_of 3)
e2fsck -fy "$ROOT" >/dev/null || true
zerofree "$ROOT"                                       # free space -> zeros: compresses to nothing
losetup -d "$ROOT"; LOOPS=()

log "Compress -> $OUT"
xz -T0 -6 -c "$IMG" > "$OUT"
rm -f "$IMG"
(cd /out && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
ls -la /out
