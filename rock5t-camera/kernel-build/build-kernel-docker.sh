#!/bin/bash
# build-kernel-docker.sh - build the IMX477 kernel packages on ANY machine with
# Docker: Apple Silicon Mac, arm64 Linux, x86-64 Linux, Windows (Docker Desktop
# + WSL2). No toolchain on the host; everything runs in a Debian container.
#
#     REV=7 ./build-kernel-docker.sh
#
# REV is the package revision - bump it every build (6 = the kernel released
# as kernel-6). The two .debs land in kernel-build/out/; install them on the
# Rock as described in README.md.
#
# What makes this the SAME kernel as the one on the Rock:
#   - Radxa's source pinned to one commit (KERNEL_COMMIT below), not a branch
#     tip that moves
#   - the driver from ../driver/imx477.c, injected by inject-driver.sh
#   - the exact kernel settings in kernel.config (the Rock's own config plus
#     CONFIG_VIDEO_IMX477=y and the -imx477 release suffix)
#
# The kernel source and build objects live in a Docker volume (imx477-kernel-src),
# so only the first build is slow (~30-60 min, all modules); after a driver
# change, rebuilds take minutes. `docker volume rm imx477-kernel-src` starts over.
#
# On an arm64 host (Apple Silicon, arm64 Linux) the container builds natively;
# on x86-64 it cross-compiles with Debian's aarch64 toolchain.

set -euo pipefail

KERNEL_REPO=https://github.com/radxa/kernel.git
KERNEL_BRANCH=linux-6.1-stan-rkr4.1                     # for reference
KERNEL_COMMIT=34337a9c76fd2fa10d8201ce32d3b782afd38098  # 6.1.84, the Rock's kernel
IMAGE=debian:bookworm
VOLUME=imx477-kernel-src

# ---------------------------------------------------------------- in container
if [ "${1:-}" = "--inside" ]; then
    export DEBIAN_FRONTEND=noninteractive
    echo "==> installing build tools"
    apt-get update -qq
    apt-get install -y -qq build-essential bc bison flex libssl-dev libelf-dev \
        debhelper rsync kmod cpio libncurses-dev dwarves git python3 >/dev/null
    CROSS=""
    if [ "$(dpkg --print-architecture)" != "arm64" ]; then
        apt-get install -y -qq crossbuild-essential-arm64 >/dev/null
        CROSS=aarch64-linux-gnu-
        echo "==> x86 host: cross-compiling for arm64"
    fi
    MK=(make ARCH=arm64 ${CROSS:+CROSS_COMPILE=$CROSS})

    cd /src
    if [ ! -d .git ]; then
        git init -q
        git remote add origin "$KERNEL_REPO"
    fi
    if ! git cat-file -e "$KERNEL_COMMIT^{commit}" 2>/dev/null; then
        echo "==> fetching Radxa kernel $KERNEL_COMMIT"
        git fetch -q --depth 1 origin "$KERNEL_COMMIT"
    fi
    # restore the pristine tracked files (untracked build objects are kept,
    # which is what makes rebuilds incremental), then add the driver
    git -c advice.detachedHead=false checkout -q -f "$KERNEL_COMMIT"
    bash /repo/kernel-build/inject-driver.sh /src /repo/driver

    cp /repo/kernel-build/kernel.config .config
    "${MK[@]}" olddefconfig >/dev/null
    grep -E "^CONFIG_VIDEO_IMX477=|^CONFIG_LOCALVERSION=" .config

    # LOCALVERSION="" stops the build appending a '+' for the modified tree;
    # the release suffix comes from CONFIG_LOCALVERSION in kernel.config
    KREL=$("${MK[@]}" -s LOCALVERSION="" kernelrelease)
    echo "==> building $KREL, package revision $REV (all cores)"
    "${MK[@]}" -j"$(nproc)" LOCALVERSION="" KDEB_PKGVERSION="$KREL-$REV" bindeb-pkg

    cp "../linux-image-${KREL}_${KREL}-${REV}_arm64.deb" \
       "../linux-headers-${KREL}_${KREL}-${REV}_arm64.deb" /out/
    rm -f ../*.deb ../*.buildinfo ../*.changes
    echo "==> done: $(cd /out && ls linux-*-"$REV"_arm64.deb | tr '\n' ' ')"
    exit 0
fi

# ---------------------------------------------------------------------- host
: "${REV:?set the package revision, e.g.  REV=7 $0}"
HERE="$(cd "$(dirname "$0")" && pwd)"     # kernel-build/
REPO="$(cd "$HERE/.." && pwd)"            # rock5t-camera/
OUT="${OUT:-$HERE/out}"
mkdir -p "$OUT"

command -v docker >/dev/null || { echo "Docker is required: https://docs.docker.com/get-docker/" >&2; exit 1; }

docker run --rm \
    -e REV="$REV" \
    -v "$VOLUME:/src" \
    -v "$REPO:/repo:ro" \
    -v "$OUT:/out" \
    "$IMAGE" bash /repo/kernel-build/build-kernel-docker.sh --inside
