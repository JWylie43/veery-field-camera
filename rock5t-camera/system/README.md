# system/

Files `setup.sh` installs into the Rock's system configuration.

- `rkaiq_3A-override.conf` → `/etc/systemd/system/rkaiq_3A.service.d/override.conf`.
  Radxa's `rkaiq_3A.service` is a oneshot wrapper that backgrounds `rkaiq_3A_server`;
  systemd sees the wrapper exit and runs `ExecStop` (`killall`), so the 3A daemon - the
  cameras' auto exposure / white balance, which loads the IQ tuning file - dies ~16 ms
  after it starts. This drop-in runs the daemon directly and keeps it running. (No
  comments inside it: setup.sh compares it byte for byte with the installed copy.)
