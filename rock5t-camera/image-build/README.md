# Ready-to-flash Rock image

Builds one image file that turns a ROCK 5T into the camera rig with nothing to run
on the Rock: flash it, boot it, and the recorder is up at `http://<rock-ip>:8080`.

It is Radxa's stock image — the exact build the rig was developed on — with what
`rock5t-camera/setup.sh` would install already in it:

| In the image | |
|---|---|
| Radxa ROCK 5T Debian 12 KDE, release `rsdk-r7` (2026-07-06) | the base: OS, Radxa's camera software (rkaiq 3A, MPP encoder), boot tools |
| the camera kernel (`kernel-N` release, the one `setup.sh` names) | the IMX477 driver built in; boots by default, Radxa's kernel kept as a fallback; kernel updates held |
| the camera overlay, in `/boot/dtbo/` | both cameras + genlock roles |
| the IQ tuning file, in `/etc/iqfiles/` | |
| the 3A service fix (`../system/rkaiq_3A-override.conf`) | |
| ffmpeg, exfatprogs and the other packages `setup.sh` installs | |
| a git checkout of this repo, ending up in `/home/radxa/<repo>` | so the Rock can `git pull` |
| the recorder service, enabled | starts at every boot |
| `camera-network.txt` on the `config` partition + the service that applies it | Wi-Fi and hotspot (default `VeeryCam` / `changeme123`), filled in before the first boot, read once |
| the hostname, if built with `RIG_HOSTNAME=` | |

What Radxa's own first boot does is unchanged: it creates the `radxa` user (password
`radxa`; change it), generates the Rock's own SSH keys, grows the root partition to fill
the card, and enables SSH when no screen is connected. One extra first-boot step
(`firstboot/`) then moves the repo checkout into `/home/radxa` and hands it over, and
the recorder starts after it. `/etc/camera-rig-image` records what an image was built
from.

## Build

Needs Docker (running) and ~20 GB free. From the repo root, with everything committed
(the image carries a checkout of the current commit):

```bash
RIG_HOSTNAME=veery rock5t-camera/image-build/build-image.sh 1
```

The number is the image revision. The first run downloads Radxa's image (1.4 GB,
checked against Radxa's checksum) and the kernel release into `work/`; later builds
reuse them. The result is `out/rock5t-camera-image-1.img.xz` (~1.5 GB) plus a
`.sha256`. `work/` and `out/` are not committed. `ALLOW_DIRTY=1` builds from
uncommitted changes, for testing only.

The build fails rather than produce an image whose boot menu doesn't start the camera
kernel with the overlay.

## Publish

```bash
gh release create image-1 rock5t-camera/image-build/out/rock5t-camera-image-1.img.xz \
    rock5t-camera/image-build/out/rock5t-camera-image-1.img.xz.sha256 \
    --title "Rock image 1" --notes "what changed"
```

GitHub allows 2 GB per file; the image is ~1.5 GB.

Keep the `kernel-N` release the image was built from: the build downloads it, and
`setup.sh` uses it to update a Rock that is already running.

## When to build a new image

After changing anything the image carries — the kernel (bump `KREL`/`KREV` in
`rock5t-camera/setup.sh` first), the overlay, the tuning file, the 3A fix, the
recorder. A Rock that is already running doesn't need reflashing: `git pull` and
`rock5t-camera/setup.sh` bring it up to date.
