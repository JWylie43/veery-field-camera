# Kernel build: IMX477 built in (`CONFIG_VIDEO_IMX477=y`)

The Radxa/Rockchip vendor kernel assumes camera sensor drivers are built
into the kernel: the builtin pipeline (csi2-dphy → mipi-csi2 → rkcif →
rkisp) purges any sensor that hasn't async-registered by the end of kernel
init (`late_initcall`, i.e. before `/init` runs) — permanently. A loadable
`imx477.ko` therefore can never join the media graph on a stock kernel, no
matter how it's ordered (softdep, modules-load.d, initramfs: all tried,
all lose by design; full autopsy in `../../ROCK5T_CAMERA.md`, bring-up log
2026-09-11 — a partial runtime-overlay workaround lives in git history at
df6b0ec). Building the driver in — exactly like every in-tree Rockchip
sensor — makes the problem not exist.

## What defines the kernel (all in this repo)

| Piece | Where |
|---|---|
| Radxa kernel source, pinned to one commit | `KERNEL_COMMIT` in `build-kernel-docker.sh` (`34337a9c76fd`, branch `linux-6.1-stan-rkr4.1`, 6.1.84) |
| The IMX477 driver | `../driver/imx477.c`, added by `inject-driver.sh` |
| Kernel settings | `kernel.config` — the Rock's own config + `CONFIG_VIDEO_IMX477=y` + release suffix `-8-rk2410-imx477` |

Built kernels are published as **GitHub Releases** (`kernel-N`, with the two
`.deb` files attached), not committed - so nobody has to rebuild to install.

## Install a released kernel (no build)

On the Mac, download the two `.deb` files from the repo's Releases page
(e.g. `kernel-6`), or with the GitHub CLI:

```
gh release download kernel-6 --repo JWylie43/veery-field-camera --dir ~/Desktop/veery-kernel-6
scp ~/Desktop/veery-kernel-6/*.deb veery:~/
```

Then on the Rock:

```
sudo dpkg -i ~/linux-image-*-imx477-6_arm64.deb ~/linux-headers-*-imx477-6_arm64.deb
sudo u-boot-update && sudo reboot
```

Check with `uname -v` (build number) and `cat /sys/module/imx477/parameters/genlock` (`Y`).

## Build on any machine (Docker) - recommended

Mac (Apple Silicon or Intel), Linux (arm64 or x86-64), or Windows with
Docker Desktop. Nothing to install but Docker:

```
cd rock5t-camera/kernel-build
REV=7 ./build-kernel-docker.sh        # bump REV every build
```

The packages land in `kernel-build/out/` (ignored by git). The first build
takes ~30-60 min (the whole kernel and its modules); the source and objects
are kept in the Docker volume `veery-kernel-src`, so a rebuild after a
driver change takes minutes. On arm64 hosts it builds natively; on x86-64 it
cross-compiles. `docker volume rm veery-kernel-src` frees the space (~15 GB).

## Build + install on the Rock (~1–2 h)

```
cd ~/veery-field-camera/rock5t-camera/kernel-build
./build-kernel.sh              # clone radxa/kernel, inject driver, build debs
./build-kernel.sh install      # dpkg -i + u-boot-update
sudo reboot
```

This one takes its settings from the running kernel instead of
`kernel.config`, so run it from the stock kernel, or prefer the Docker build.

The new kernel installs alongside the stock one (release string suffix
`-imx477`); the stock kernel remains in the u-boot menu as a fallback.
No reflash. Use the normal full-enable boot overlay
(`../overlay/rock-5t-dual-rpi-hq-imx477.dts`) — rebuild it and the boot
dtbo installs unchanged.

## Publishing a new kernel release

After building and testing a new revision N on the Rock:

```
gh release create kernel-N rock5t-camera/kernel-build/out/linux-image-*-imx477-N_arm64.deb \
    rock5t-camera/kernel-build/out/linux-headers-*-imx477-N_arm64.deb \
    --title "Kernel N" --notes "what changed in the driver"
```

(or on github.com: Releases → Draft a new release → tag `kernel-N` → attach
the two `.deb` files → Publish).

## After the new kernel boots

Remove the stock-kernel workaround stack (harmless but obsolete):

```
sudo rm -f /etc/modules-load.d/imx477.conf /etc/modprobe.d/imx477-order.conf
sudo rm -f /lib/firmware/rock5t-cam-enable*.dtbo
sudo rm -f /lib/modules/6.1.84-8-rk2410/extra/imx477.ko \
           /lib/modules/6.1.84-8-rk2410/extra/rk_cam_defer_enable.ko
```

(Those files belong to the old kernel's module tree anyway — the new
kernel never loads them.)

## Radxa kernel updates

A Radxa kernel update does NOT carry our driver: either hold the kernel
(`sudo apt-mark hold 'linux-image*' 'linux-headers*'`) or re-run this
script against the new source (`rm -rf ~/radxa-kernel`, adjust `BRANCH`
if Radxa moved on, run both steps again).

## Known-broken vendor bits (independent of kernel choice)

- `rkaiq_3A.service` kills its own daemon at start (oneshot wrapper +
  `ExecStop=killall` — the wrapper backgrounds the server, systemd sees it
  exit and runs stop). **Fixed on the Rock 2026-09-11** with a drop-in at
  `/etc/systemd/system/rkaiq_3A.service.d/override.conf`:

  ```
  [Service]
  Type=simple
  ExecStart=
  ExecStart=/usr/bin/rkaiq_3A_server
  ExecStop=
  ```

  (plus `chmod 644` on the world-writable unit). Any fresh install needs
  this drop-in too.
