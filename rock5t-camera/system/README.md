# system/

Files `setup.sh` installs into the Rock's system configuration.

- `camera-network.txt` → `/config/camera-network.txt` (the template, on the image only),
  with `camera-network` → `/usr/local/sbin/` and `camera-network.service`: at the next
  boot it creates the Wi-Fi connection (only with both name and password; priority 100,
  joined when in range) and the hotspot (priority -10, a 2.4 GHz access point at
  10.43.0.1 the Rock starts otherwise; open if it has no password), then renames the file
  `camera-network.applied.txt` with the passwords blanked - so it is read once. Re-applying
  replaces those two connections; others are left alone.
- `rkaiq_3A-override.conf` → `/etc/systemd/system/rkaiq_3A.service.d/override.conf`.
  Radxa's `rkaiq_3A.service` is a oneshot wrapper that backgrounds `rkaiq_3A_server`;
  systemd sees the wrapper exit and runs `ExecStop` (`killall`), so the 3A daemon - the
  cameras' auto exposure / white balance, which loads the IQ tuning file - dies ~16 ms
  after it starts. This drop-in runs the daemon directly and keeps it running. (No
  comments inside it: setup.sh compares it byte for byte with the installed copy.)
