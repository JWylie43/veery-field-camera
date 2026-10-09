#!/bin/bash
# rig-firstboot - the image's one first-boot step (installed by image-build/).
# Radxa's own first boot (rsetup, /config/before.txt) creates the @RIG_USER@ user; this
# waits for it, moves the repo checkout the image carries from /opt into that user's
# home and hands it over, then switches itself off. The recorder service is ordered
# after this, so it starts with the checkout in place (its takes go to that user's home).
set -u
for _ in $(seq 1 300); do
  id @RIG_USER@ >/dev/null 2>&1 && [ -d /home/@RIG_USER@ ] && break
  sleep 1
done
id @RIG_USER@ >/dev/null 2>&1 || { echo "rig-firstboot: user @RIG_USER@ never appeared"; exit 1; }
if [ -d /opt/@REPO_NAME@ ] && [ ! -e /home/@RIG_USER@/@REPO_NAME@ ]; then
  mv /opt/@REPO_NAME@ /home/@RIG_USER@/@REPO_NAME@
fi
chown -R @RIG_USER@:@RIG_USER@ /home/@RIG_USER@/@REPO_NAME@
systemctl disable rig-firstboot.service
echo "rig-firstboot: checkout in /home/@RIG_USER@/@REPO_NAME@"
