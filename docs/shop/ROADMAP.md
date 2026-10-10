# Shop roadmap

Planned work for the print-shop fork. Shipped features are described in `SHOP_FEATURES.md`.

## Planned

- **Failure check, next steps:** tune the thresholds on real checkpoint photos (the per-job folders
  keep them); offer it from the Bambu/agent Print dialog too; mask the printed part's own shadow.

## Open items

- **Shutdown hang:** the Moonraker printer agent's status thread uses blocking socket reads, which
  ignore Boost.Beast timeouts, so quitting while the printer link is down can hang. Fix: close the
  socket from the stopping thread, real read timeouts, interruptible reconnect back-off, bounded
  wait on quit.
- **Plus 4 Wi-Fi:** largely fixed 2026-10-10. The AICSemi AIC8800 USB adapter (a69c:88dc, driver
  `aic8800_fdrv`) had driver power saving on; `options aic8800_fdrv ps_on=0` in
  `/etc/modprobe.d/aic8800-no-powersave.conf` turned it off. One-minute ping from the Mac: before,
  11 of 60 lost, 474 ms average, 1481 ms worst; after, 1 lost, 48 ms average, 660 ms worst (the
  router hops are about 4 ms, so some Wi-Fi latency remains). Note: `sudo reboot` left the
  touchscreen dead; a power cycle recovered it. Ethernet remains the most reliable option.
- **Hardware checks pending:** QIDI Box Load / Unload / Eject / Read RFID buttons, the Box slot
  mapping push, 3D Envision send into an already-open slicer, the Device tab mesh button, the
  failure-check macros (install `docs/shop/plus4/failure_check.cfg`), camera calibration and a
  first checked print.
- **3D Envision:** merge its `send-to-slicer` branch into `main`.
- **Purge strip placement:** keep the stock L-shaped purge near the part, widen the gap, or move it
  to a fixed front-edge strip (user's choice).
