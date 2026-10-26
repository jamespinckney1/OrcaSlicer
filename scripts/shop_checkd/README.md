# Failure check service

An always-on companion of the slicer's print failure check (`docs/shop/SHOP_FEATURES.md`, "Print failure
check"). The slicer adds the checkpoints to a job and, with Preferences > "Failure check service" set,
hands the job here. The service watches the printer, photographs each checkpoint, compares it with the
G-code using the slicer's own engine and resumes or holds the print, so checks and phone notifications
go on while the slicer is closed. A slicer that is open shows the service's status line and its Hold
window; it never answers a checkpoint of a job the service has. A service that cannot be reached at
upload leaves the job to the slicer, as before.

A quick restart (the printer rewinds the paused job to layer 1, `docs/shop/plus4/quick_restart.cfg`) raises the
printer's `SHOP_RESTART_POINT.restarts` count; the service then moves the job's photos to `attemptN/` and checks
it again from a new reference photo, like the slicer does.

- `shop_check.cpp`: the comparison (`src/slic3r/GUI/FailureCheck.*`) as a command-line tool.
- `shop_checkd.py`: the service (Python standard library only): Moonraker polling, checkpoint
  handling, ntfy notifications and the HTTP API the slicer uses (documented at the top of the file).
- `shop-checkd.service`, `install.sh`, `deploy.sh`: systemd unit, on-host install, deploy over SSH.

## Install

On a Debian/Ubuntu host that reaches the printer (Moonraker on 7125, the webcam on 80) and ntfy:

    sudo apt install build-essential cmake pkg-config libboost-filesystem-dev libeigen3-dev \
        nlohmann-json3-dev libturbojpeg0-dev python3 curl

Then from the Mac:

    scripts/shop_checkd/deploy.sh <ssh host>

It copies this folder and the engine, builds `shop_check`, installs into `/opt/shop-checkd`, and runs
the service as the `shop-checkd` system user with its data in `/var/lib/shop-checkd` (job folders:
G-code, photos and results, the newest 30 kept). Rerun it after changing the engine so the service
judges like the slicer. In the slicer, set Preferences > "Failure check service" to
`http://<host>:8765` and "Failure check service token" to the token the install prints (also in
`/var/lib/shop-checkd/token`, created at first start).

Every request but `/health` needs that token (`Authorization: Bearer <token>`): a job names the printer
the service will hold, resume and restart the camera of. Allow only the slicer's networks to reach port
8765 as well. Job settings include the ntfy topic, so keep the data folder private.

## Testing without a printer

`shop_check` can be built on the Mac against the slicer's dependencies and the service pointed at a fake
Moonraker that replays a saved job folder; `info` and `compare` print JSON, so results can be compared
with the slicer's replay tool (`scripts/shop_tests/failure_check_replay.cpp`).
