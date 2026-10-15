# Print-shop fork

This branch is OrcaSlicer customised for the shop's printers: Qidi X-Plus 4 with the QIDI Box,
Creality Ender-3 Pro, Creality CR-10 Smart and Bambu Lab A1. `main` tracks upstream OrcaSlicer;
the shop work lives on top of it.

- [SHOP_FEATURES.md](SHOP_FEATURES.md): what each shop feature does and where it lives.
- [ROADMAP.md](ROADMAP.md): planned work and open items.
- [plus4/](plus4/): Klipper macros for the Plus 4 (below).

## Build and install (macOS)

    scripts/shop_build_mac.sh               # incremental build, then install to /Applications
    scripts/shop_build_mac.sh --no-install  # build only: build/arm64/OrcaSlicer/OrcaSlicer.app

The build script pins `SDKROOT` to Xcode's own SDK and reconfigures from scratch when Xcode's SDK
changed since the last configure (about an hour). `scripts/shop_install_mac.sh` signs the app with
the first "Apple Development" identity (or `SIGN_IDENTITY`) so macOS privacy grants survive
rebuilds, and swaps it into `/Applications` by rename: a failed install leaves the previous app in
place. Quit OrcaSlicer before installing.

Check a build by launching it with the real settings folder (no `--datadir`): a scratch folder
starts in first-run state and skips code that only runs once a printer is selected.

## Settings backup

The source holds no personal values: printer addresses, the ntfy topic, camera calibrations, Box
slot choices and user presets live in the app data folder (`~/Library/Application Support/OrcaSlicer`).
A build from this branch therefore matches the shop's own, but that folder needs its own backup:

    scripts/shop_backup_settings.sh <private folder>             # copy the settings there
    scripts/shop_backup_settings.sh --restore <private folder>   # copy them back (OrcaSlicer closed)

`--with-jobs` also copies the failure-check job folders (G-code and photos, hundreds of MB). Keep the
backup private: it contains the ntfy topic and any printer access codes.

## Filament profiles

    python3 scripts/shop_profiles/regen.py           # regenerate, validate, bump vendor versions
    python3 scripts/shop_profiles/regen.py --check   # fail if the committed profiles are stale

Edit the manufacturer data in `scripts/shop_profiles/data/*.json`, then run `regen.py`. It runs the
generator, the upstream `orca_profile_tool.py` steps and `check_load_order.py` for each shop vendor,
and bumps the vendor `version` when that vendor's files changed (installs only pick up profiles
from a higher version). The generator reports presets it made earlier but no longer makes;
`regen.py --prune` deletes them. Check first that no saved user preset inherits one. Details are
in [SHOP_FEATURES.md](SHOP_FEATURES.md#filament-profiles).

## Tests

    ctest --test-dir build/arm64/tests/fff_print -C Release -R shop   # hot-unload G-code rewrite

The failure-check image tests are standalone programs; the build command is in the header of
`scripts/shop_tests/failure_check_test.cpp`.

## Plus 4 printer macros

Nothing in `plus4/` is installed by the build. The printer runs Qidi's Klipper fork, and Qidi
firmware updates overwrite `gcode_macro.cfg`, so these are re-applied by hand:

- `quick_start.patch`: quick start (skip the Box flush when the filament is unchanged, full-bed
  mesh reuse). Preferred over replacing the whole file with `gcode_macro.cfg.quickstart`, which
  is based on the stock "QIDI Plus4 V1.0.0" file.
- `failure_check.cfg`: the checkpoint macros the print failure check needs.
- `retired/`: kept for reference only. Do not install.

Back up `gcode_macro.cfg` on the printer before changing it, and restart Klipper only while the
printer is idle.
