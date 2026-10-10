# Print-shop features in this fork

This fork of OrcaSlicer carries a set of shop-specific additions on top of upstream. Everything
lives behind clearly named files so upstream merges stay manageable. Shop data files are stored
under the app data directory in a `shop/` folder (`~/Library/Application Support/OrcaSlicer/shop/`
on macOS) and survive upgrades.

## Look

`scripts/apply_qidi_theme.py` recolors the Orca teal accent family to the Qidi Studio blue palette
across GUI sources, SVG icons and embedded web pages, and the splash/about/app icons are Qidi
Studio's. Re-run the script after merging upstream to recolor new icons; it is idempotent.

## Filament profiles

`scripts/shop_profiles/gen_filaments.py` generates per-printer filament presets from manufacturer
catalog data in `scripts/shop_profiles/data/*.json` (QIDI, Fiberon/Polymaker, SUNLU, Overture; the
sibling `.md` files hold the research notes and caveats). Shop printers: Qidi X-Plus 4, Creality
Ender-3 Pro, Creality CR-10 Smart, Bambu Lab A1. Each preset inherits the vendor bundle's existing
printer-specific generic for its material and overrides manufacturer temperatures, bed, density,
fan and notes. After editing data, run the generator then, per touched vendor:

    python3 scripts/orca_profile_tool.py normalize    --vendor <Vendor>
    python3 scripts/orca_profile_tool.py update-index --vendor <Vendor>
    python3 scripts/orca_profile_tool.py generate-id  --vendor <Vendor>
    python3 scripts/orca_profile_tool.py check        --vendor <Vendor>

**Always bump the vendor version after any profile change.** OrcaSlicer runs from an installed copy
of each vendor bundle in the user's settings folder (`system/<Vendor>`) and only replaces it when the
bundle's `version` in `resources/profiles/<Vendor>.json` is higher. Shop builds use a `.90.` third
component (Qidi `02.04.90.x`, BBL `02.08.90.x`, Creality `02.03.90.x`) so the online profile updater,
which serves upstream `.00.`-level versions, never replaces the shop bundles. Increase the last
component on every profile change, and keep the `.90.` level after merging upstream.

Speed and temperature: filament presets carry speed as max volumetric speed. On 0.4 mm profiles the
manufacturer's max print speed (converted at 0.45 mm line width x 0.2 mm layers) sets it, up to the
highest value upstream already ships on that printer for that material; on other nozzle sizes it can
only lower the printer profile's value. Where the manufacturer publishes speed-to-temperature bands
(`speed_temp_bands` in the data files, currently SUNLU), the nozzle temperature comes from the band
matching the speed that printer's default process actually runs at (Plus 4 / A1 about 250 mm/s,
Ender-3 Pro about 45, CR-10 Smart about 40, limited by the max volumetric speed), and the first
layer temperature from the band for the first layer speed. Bands that sit entirely below the
product's rated range are treated as copy errors and ignored. OrcaSlicer has no in-print
temperature-by-flow control, so printing much faster or slower than the printer's default process
means adjusting temperature along the bands listed in each preset's notes.

Machine-specific tweaks: the Ender-3 Pro presets assume the Creality Sprite Extruder (direct
drive, 0.8 mm retraction) and a CR Touch (`M420 S1` after homing); the Plus 4 presets note the
Phaetus Conch hotend.

## Start G-code library (Printer settings > Machine G-code)

A "Saved start G-code" row above the start G-code box lists named snippets saved per printer
model. "Save current as..." stores the current start G-code under a name; picking a snippet
replaces the start G-code and marks the preset modified. Stored in `shop/start_gcode_library.json`.
Code: `src/slic3r/GUI/StartGCodeLibrary.*`, hooks in `Tab.cpp` (`TabPrinter::*start_gcode*`).

## Project auto-naming (Preferences > General > Project)

"Suggest configuration-based project names" proposes
`<file>_<printer>_<filament>_<nozzle>_<sup|nosup>.3mf`, e.g. `bracket_XP4_SUN-PLA+_0.4_sup.3mf`,
when saving a project; an earlier suffix is stripped so names do not stack. "Project folder" is the
folder offered for projects that have no file yet. Code: `src/slic3r/GUI/ShopNaming.*`; app config
keys `shop_project_autoname`, `shop_project_dir`.

## Shop machine switcher (sidebar, "Machine" row)

Save the current printer + process + filament selection under a machine name; picking the name
applies all three (and the physical printer/host if one was saved). Stored in
`shop/printers.json`. Code: `src/slic3r/GUI/ShopPrinters.*`, `Sidebar::*shop_printer*` in
`Plater.cpp`.

## QIDI Box

* Filament sync: `MoonrakerPrinterAgent::fetch_qidi_box_filament_info` reads the printer's
  `multi_color_controller` object (live schema captured in
  `scripts/tests/fixtures/qidi_box_multi_color_controller.json`; legacy `boxes[]` shape kept as a
  fallback) and feeds the slots into Orca's AMS mapping. Slot states: 0 empty, 1 ready, 2 loaded.
* Mapping: before `SDCARD_PRINT_FILE`, the agent writes `SAVE_VARIABLE VARIABLE=value_t<i>
  VALUE='slot<k>'` for each filament mapped in the send dialog; QIDI's `T<n>` macros read those.
* Controls: the "Box" button on the Machine row opens `QidiBoxDialog` with Load
  (`EXTRUDER_LOAD SLOT=slotN`), Unload (`EXTRUDER_UNLOAD`), Eject (`MULTI_COLOR_BOX_UNLOAD`) and
  Read RFID (`MULTI_COLOR_READ_RFID`) per slot, sent through the connected device.

Setup: add the Plus 4 as a physical printer with host type Moonraker, connect it on the Device
tab, then use the sidebar filament sync.

## Quick start (Process settings > Others > Print start)

"Quick start (skip Box purge)" is a process option, so it can live in a dedicated "Quick" process
preset. The Plus 4 start G-code passes it as `PRINT_START ... QUICK=1`.

Purge decision (slicer side): every print sent from this slicer, by either send path, records the
filament preset and colour it starts with, per printer (`shop/last_print_filament.json`, keyed by
the physical printer host or device IP). With Quick start on, Print plate compares the new print's
first filament with that record. Same filament: `FLUSH=0`, no prompt. Different filament: a warning
naming both, with Full purge (default) / Quick purge anyway / Cancel. No record yet: `FLUSH=1`.
The decision is written into the sliced G-code's `PRINT_START` line before it is sent. Whether the
Box unloaded at the end of the last print does not matter. Prints started outside this slicer do
not update the record.

Printer side (`docs/shop/plus4/quick_start.patch`, full file `gcode_macro.cfg.quickstart`): with
`QUICK=1`, `FLUSH=0` skips the 5-round Box flush (the nozzle-clean purge and wipe still run) and
`FLUSH=1` or no FLUSH runs it. Leveling keeps `Z_TILT_ADJUST` and the nozzle height measurement,
skips the `Z_VIBRATE` bed shake, and loads the full-bed `default` mesh (never the part-scoped
`kamp` mesh, which only covers the previous part). `SHOP_FULL_MESH BED=80` rebuilds that full-bed
mesh; run it after a plate change or every few weeks, from the printer console or from the
Device tab's shop toolbar (Rebuild full-bed mesh, which checks the printer is idle, asks for the
bed temperature and runs the macro in the background). QIDI firmware updates replace
`gcode_macro.cfg`; re-apply the patch after updating. The original file is kept on the printer as
`gcode_macro.cfg.bak-quickstart-2026-10-10`.

## 3D Envision hand-off (Send to Slicer)

3D Envision's File > Send to Slicer (Shift-Cmd-P) and Send to Slicer On > <machine> write the visible
bodies as a 3MF into `shop/inbox/` in the slicer's settings folder, plus a sidecar
`<same name>.json` with `name` and an optional shop `machine`, then ask macOS to open the 3MF in the
slicer. A model that arrives from that folder (at launch or while running) opens as a new project
named after the part, geometry only, after the usual save prompt for the current project; with a
machine it also applies that shop machine's presets. Files from anywhere else open as before, so
web links or other apps cannot trigger this path. Code: `src/slic3r/GUI/EnvisionInbox.*`,
`Plater::open_envision_send`, hooks in `GUI_App::MacOpenFiles` and the startup file loading.
3D Envision prunes sends older than a day.

## Sliced G-code editor (File > Export > Edit sliced G-code)

Opens the current plate's temp G-code, the same file Export G-code and Send use, with find and
go-to-layer. Save keeps a one-time `.orig` backup, writes the file, re-runs the G-code processor
and reloads the preview. Re-slicing overwrites the edit. Code: `src/slic3r/GUI/GCodeEditDialog.*`,
`Plater::edit_sliced_gcode`.

## Shop calibration log (Calibration > Shop calibration log)

A per-printer, per-filament log of calibration results (pressure advance, flow ratio, max
volumetric speed, temperature, notes). "New from current presets" seeds an entry from the active
printer and filament; "Apply to filament preset" writes the values into the current filament
preset (unsaved; save the preset to keep it). Stored in `shop/calibration_results.json`. Code:
`src/slic3r/GUI/ShopCalibration.*`, `CalibrationLogDialog.*`.

## Not yet verified on hardware

Box sync in the UI, the mapping push, and the slot-state semantics were built from the live
Moonraker queries of one Plus 4 but have not been exercised end to end with a print.
