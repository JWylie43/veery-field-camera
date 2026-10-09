#!/usr/bin/env bash
#
# build-image.sh - build the ready-to-flash ROCK 5T image. Run on the Mac (or any
# machine with Docker), from anywhere in the checkout:
#     rock5t-camera/image-build/build-image.sh 1                    # -> out/rock5t-camera-image-1.img.xz
#     RIG_HOSTNAME=myrig rock5t-camera/image-build/build-image.sh 1  # also set the Rock's hostname
# The number is the image revision (the GitHub release is image-<N>). See README.md here.
#
# It takes Radxa's stock image and this repo's camera kernel - both from this repo's own
# GitHub releases (radxa-rsdk-r7, a mirror of Radxa's file checked against Radxa's
# checksum; kernel-N, named by KREL/KREV in rock5t-camera/setup.sh) - and
# builds inside Docker (build-inside.sh). The repo must be committed - the image carries
# a git checkout of it - unless ALLOW_DIRTY=1 (test builds only).
set -euo pipefail
REV="${1:?usage: build-image.sh <image revision, e.g. 1>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="$HERE/work"; OUT="$HERE/out"
mkdir -p "$WORK" "$OUT"

# ---- the base image: Radxa's ROCK 5T Debian 12 KDE build rsdk-r7 (2026-07-06), the
#      one the rig was developed and tested on (see "Tested setup" in docs/rock.md)
RADXA_TAG=rsdk-r7
RADXA_FILE=rock-5t_bookworm_kde_r7.output_512.img.xz
RADXA_SHA512=0dbb83e55c2afb6225e39502794f02cc53257baa2f4703bbc25e59879f7995b7af9da88ac7b35cbad6d761b14eaba95fc089c0e29b3ddd2fdd61c835ee908516
# mirrored as this repo's release radxa-$RADXA_TAG (Radxa's original:
# https://github.com/radxa-build/rock-5t/releases/tag/rsdk-r7); URL set below, from the remote

# ---- the camera kernel: the same release setup.sh installs
eval "$(grep -E '^(KREL|KREV)=' "$REPO/rock5t-camera/setup.sh")"

command -v docker >/dev/null && docker info >/dev/null 2>&1 || { echo "Docker is not running"; exit 1; }
if [ -n "$(git -C "$REPO" status --porcelain)" ] && [ "${ALLOW_DIRTY:-0}" != 1 ]; then
  echo "The repo has uncommitted changes - commit them first (the image carries a checkout)."
  echo "(ALLOW_DIRTY=1 builds anyway, for testing.)"; exit 1
fi
REPO_URL="$(git -C "$REPO" remote get-url origin)"
SLUG="$(printf '%s' "$REPO_URL" | sed -E 's#^(https://github.com/|git@github.com:)##; s#\.git$##')"
RADXA_URL="https://github.com/$SLUG/releases/download/radxa-$RADXA_TAG/$RADXA_FILE"

echo "==> Radxa image $RADXA_FILE"
if [ ! -f "$WORK/radxa.img" ]; then
  [ -f "$WORK/$RADXA_FILE" ] || curl -fL --progress-bar -o "$WORK/$RADXA_FILE" "$RADXA_URL"
  echo "$RADXA_SHA512  $WORK/$RADXA_FILE" | shasum -a 512 -c - >/dev/null \
    || { echo "checksum mismatch: $WORK/$RADXA_FILE"; exit 1; }
  xz -dc -T0 "$WORK/$RADXA_FILE" > "$WORK/radxa.img.part" && mv "$WORK/radxa.img.part" "$WORK/radxa.img"
fi

KDIR="$WORK/kernel-$KREV"
echo "==> Camera kernel $KREL (kernel-$KREV)"
mkdir -p "$KDIR"
for kind in image headers; do
  f="linux-$kind-${KREL}_${KREL}-${KREV}_arm64.deb"
  [ -f "$KDIR/$f" ] || curl -fL --progress-bar -o "$KDIR/$f" \
    "https://github.com/$SLUG/releases/download/kernel-$KREV/$f"
done

echo "==> Building in Docker (10-20 minutes)"
docker run --rm --privileged --platform linux/arm64 \
  -v "$WORK":/work -v "$KDIR":/kernel:ro -v "$REPO":/repo:ro -v "$OUT":/out \
  -e KREL="$KREL" -e KREV="$KREV" -e IMAGE_REV="$REV" -e REPO_URL="$REPO_URL" \
  -e REPO_NAME="$(basename "$SLUG")" -e BASE_IMAGE="Radxa $RADXA_TAG ($RADXA_FILE)" \
  -e RIG_HOSTNAME="${RIG_HOSTNAME:-}" \
  debian:bookworm bash /repo/rock5t-camera/image-build/build-inside.sh

echo
echo "Done: $OUT/rock5t-camera-image-$REV.img.xz"
echo "Flash it to an SD card / NVMe with any image flasher (balenaEtcher, Raspberry Pi Imager),"
echo "or publish it - see rock5t-camera/image-build/README.md."
