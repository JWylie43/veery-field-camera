#!/bin/bash
# inject-driver.sh - add the IMX477 driver to a Radxa kernel source tree.
#
#     inject-driver.sh <kernel-src-dir> <driver-dir>
#
# Copies imx477.c into drivers/media/i2c/ and registers it in that directory's
# Makefile and Kconfig (next to the in-tree IMX415). Idempotent: safe to run on
# a tree that already has it. Shared by build-kernel.sh (on the Rock) and
# build-kernel-docker.sh (any machine), so both build the same kernel.

set -euo pipefail
SRC="$1"
DRV="$2"

cp "$DRV/imx477.c" "$SRC/drivers/media/i2c/imx477.c"

if ! grep -q "CONFIG_VIDEO_IMX477" "$SRC/drivers/media/i2c/Makefile"; then
    sed -i '/obj-$(CONFIG_VIDEO_IMX415) += imx415.o/a obj-$(CONFIG_VIDEO_IMX477) += imx477.o' \
        "$SRC/drivers/media/i2c/Makefile"
fi

if ! grep -q "config VIDEO_IMX477" "$SRC/drivers/media/i2c/Kconfig"; then
    # insert right before the VIDEO_IMX415 entry, mirroring its shape
    sed -i '/^config VIDEO_IMX415$/i config VIDEO_IMX477\n\ttristate "Sony IMX477 sensor support"\n\tdepends on I2C \&\& VIDEO_DEV\n\tdepends on MEDIA_CAMERA_SUPPORT\n\tselect MEDIA_CONTROLLER\n\tselect VIDEO_V4L2_SUBDEV_API\n\thelp\n\t  This is a Video4Linux2 sensor driver for the Sony\n\t  IMX477 camera (Raspberry Pi HQ Camera), with Rockchip\n\t  RKMODULE support and XVS trigger-mode genlock.\n' \
        "$SRC/drivers/media/i2c/Kconfig"
fi
