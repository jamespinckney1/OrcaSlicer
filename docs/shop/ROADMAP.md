# Shop roadmap

Planned work for the print-shop fork. Shipped features are described in `SHOP_FEATURES.md`.

## Planned

### G-code-aware print failure check

Camera-based spaghetti detection guesses what a healthy print looks like, so prints with large
tree supports trigger false alarms. The slicer knows exactly what should be on the bed at every
point of the G-code, so it can make a real comparison.

- **Opt-in** from the Print dialog (checkbox), per print.
- **Checkpoints** inserted at slice time: a few moves into the first layer (bad starts are the
  most common failure), at the end of the first layer, then every 20% of the print.
- **At a checkpoint** the printer runs a `SHOP_CHECKPOINT` macro: retract, lift, park the toolhead
  out of the camera's view, wait briefly. The slicer grabs a snapshot (the Plus 4's chamber camera
  is available at `/webcam/?action=snapshot`, 640 x 480, wide-angle), renders what the G-code says
  should exist at that point (part, supports, purge line) from the camera's viewpoint, compares
  the two, and resumes the print or keeps it paused and alerts.
- **Fail-open:** if the slicer does not answer within a timeout (network drop, slicer closed), the
  printer resumes on its own; a check must never strand a print.
- **Camera calibration** once per printer: map the wide-angle image to bed coordinates (for example
  by photographing the nozzle at known positions).
- **Phases:** (1) checkpoints + snapshot capture with a side-by-side "expected vs camera" view for
  the user to judge; (2) automatic first-layer comparison (footprint against the bed reference
  image); (3) full-height comparison every 20%.
- **Depends on** a stable printer connection: the Plus 4's Wi-Fi link currently loses about 1 in 6
  packets (see Open items).

### Bed mesh health view

- Expandable panel (collapsed by default) in the Device tab's shop toolbar showing the printer's
  full-bed mesh as an angled 3D surface, highs and lows coloured.
- Separates the mesh into **tilt** (a plane fit: what `Z_TILT_ADJUST` and bed screws correct) and
  **warp** (what remains), with the range, the high and low points, and the mesh's age.
- **Recommendations** from simple thresholds, for example: range under 0.3 mm fine; tilt dominant:
  check `Z_TILT_ADJUST` / bed screws; one corner far off: debris under the plate or that corner's
  screw; mesh captured without Z-tilt or older than a few weeks: rebuild it.
- Example from the Plus 4 on 2026-10-10: range 1.01 mm, tilt +0.64 mm left-to-right and +0.29 mm
  front-to-back, warp 0.37 mm after removing tilt, lowest front-left (-0.42), highest back-right
  (+0.59); the tilt along the Z-tilt probe line showed the stock mesh predated levelling.

## Open items

- **Shutdown hang:** the Moonraker printer agent's status thread uses blocking socket reads, which
  ignore Boost.Beast timeouts, so quitting while the printer link is down can hang. Fix: close the
  socket from the stopping thread, real read timeouts, interruptible reconnect back-off, bounded
  wait on quit.
- **Plus 4 Wi-Fi drops:** about 18% packet loss and 0.5 s average latency from the Mac to the
  printer while the router hops are clean; signal is fair (-62 dBm). Suspect the USB Wi-Fi
  adapter's power saving; the printer has no `iw`, `iwconfig` or `nmcli`. Ethernet is the reliable
  fix.
- **Hardware checks pending:** QIDI Box Load / Unload / Eject / Read RFID buttons, the Box slot
  mapping push, 3D Envision send into an already-open slicer, the Device tab mesh button.
- **3D Envision:** merge its `send-to-slicer` branch into `main`.
- **Purge strip placement:** keep the stock L-shaped purge near the part, widen the gap, or move it
  to a fixed front-edge strip (user's choice).
