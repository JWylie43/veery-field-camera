# system/

Files `setup.sh` installs into the Rock's system configuration.

- `camera-network.txt` → `/config/camera-network.txt` (the template, on the image only),
  with `camera-network` → `/usr/local/sbin/` and `camera-network.service`: at the next
  boot it creates the Wi-Fi connection (only with both name and password; priority 100,
  autoconnect) and the hotspot (a 2.4 GHz access point at 10.43.0.1, priority -10, open
  if it has no password, and NOT autoconnect), then renames the file
  `camera-network.applied.txt` with the passwords blanked - so it is read once.
  Re-applying replaces those two connections; others are left alone.
- `wifi-fallback` → `/usr/local/sbin/` and `wifi-fallback.service`, every boot: scans
  once; connects to the best known Wi-Fi in range, and starts the hotspot if that fails
  (wrong password) or if no known Wi-Fi is in range (or set up at all) - no fixed waits. The hotspot must not autoconnect:
  it would come up before the radio's first scan has seen the home network, and an
  access point can't scan, so it would stay up even at home.
- `rkaiq_3A-override.conf` → `/etc/systemd/system/rkaiq_3A.service.d/override.conf`.
  Radxa's `rkaiq_3A.service` is a oneshot wrapper that backgrounds `rkaiq_3A_server`;
  systemd sees the wrapper exit and runs `ExecStop` (`killall`), so the 3A daemon - the
  cameras' auto exposure / white balance, which loads the IQ tuning file - dies ~16 ms
  after it starts. This drop-in runs the daemon directly and keeps it running. (No
  comments inside it: setup.sh compares it byte for byte with the installed copy.)
