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
catalog data in `scripts/shop_profiles/data/*.json` (QIDI, Fiberon/Polymaker, SUNLU, Overture,
Creality, YOOPAI; the sibling `.md` files hold the research notes and caveats). YOOPAI also resells
Creality filament; only YOOPAI-branded products are in its file. The generator updates presets it
generated earlier and never touches upstream ones. Shop printers: Qidi X-Plus 4, Creality
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

Data fields beyond the manufacturer's numbers: `bed_temp_recommended_c` overrides the top of the
bed range when the manufacturer names a starting value (a wide 60-100 C range would otherwise put
PETG on a 100 C bed); `shop_overrides` holds tuning measured on shop printers, per printer and
nozzle, with a note that lands in the preset.

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
`kamp` mesh, which only covers the previous part), but only once `SHOP_FULL_MESH` has built it
(saved variable `shop_full_mesh_ok`); until then quick start runs normal leveling. A mesh must be
captured after `Z_TILT_ADJUST`, like a normal start, or the gantry tilt is applied twice; QIDI's
stock `default` mesh on this printer was not (0.68 mm left-right tilt along the Z-tilt probe line).
`SHOP_FULL_MESH BED=80` homes, runs `Z_TILT_ADJUST`, measures the nozzle and probes 81 points;
run it run it after a plate change or every few weeks, from the printer console or from the
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

## Device tab shop toolbar

- **Rebuild full-bed mesh...** runs `SHOP_FULL_MESH` on an idle printer (home, heat, Z-tilt, probe
  the whole bed, save it as the `default` mesh Quick start loads).
- **Bed mesh health** (collapsed by default): the printer's mesh as an angled 3D surface, with
  Front / Back / Left / Right labels as seen standing at the door (left is the viewer's left, X
  minimum). It separates tilt (a plane fit) from warp and lists maintenance suggestions. Drag to
  rotate. Code: `BedMeshAnalysis.*` (no GUI), `BedMeshView.*`.
- **Camera calibration...** and the failure-check status line: see below.

## Print failure check (Print dialog > "Failure check")

Camera spaghetti detectors guess what a healthy print looks like, so big tree supports set them off.
This check compares the camera with what the G-code says has been printed, so supports, brims and
purge lines count as expected material.

- **Checkpoints.** On upload the slicer adds `SHOP_CHECKPOINT N=<n>` lines to the upload's own copy
  of the G-code (an exported file never carries them): a reference photo just before the first
  layer, one a fifth of the way into the first layer, one after it, one about 1 mm up (catches a
  body that starts above the bed), then at 20 / 40 / 60 / 80 % of the layers. A job that already
  has checkpoint lines keeps them, so a slicer restarted mid-print reads the same numbering.
- **Printer side** (`docs/shop/plus4/failure_check.cfg`): at a checkpoint the printer retracts,
  lifts 5 mm, parks at the purge chute with Qidi's own pause moves, and waits. `SHOP_CHECK_OK`
  resumes, `SHOP_CHECK_HOLD` stays paused and beeps (hotend off after 15 minutes; Resume reheats).
  With no answer in 45 s the printer resumes on its own, so a closed slicer or a Wi-Fi drop never
  strands a print. The checkpoint saves the same states as Qidi's PAUSE, so Resume on the
  touchscreen works during a checkpoint or a hold.
- **Comparison** (`FailureCheck.*`, no GUI): both photos are resampled onto a 2 mm bed grid using
  the camera calibration and the toolhead Z at each photo (the bed moves in Z, the camera does not),
  staying 10 mm clear of the plate edge.
  - Exposure: bright parts make the camera re-expose and change contrast. A tone curve (brightness now
    to brightness in the reference) is learnt from plate at least 16 mm from anything the G-code
    prints and applied by brightness, never by position, so a failure cannot teach it to hide itself.
  - Change: colour difference, plus texture (dark matte filament hides the plate's glitter) while the
    exposure barely moved. The threshold is the empty plate's noise level, raised where the reference
    photo's glitter is strong.
  - Alignment: the calibration is only good to about a centimetre and its error varies across the bed,
    so each separate part (with its supports and brim) is lined up with what changed, within 16 mm on
    the first layer; later checkpoints start from those offsets and may move a part only 6 mm.
  - Unexpected material: solid patches (most of each cell's neighbours changed) beyond a margin of
    8 mm plus the printed height (tall parts cast shadows and reflect light), counted only where at
    least 32 mm2 lies a further 16 mm out; 3 cm2 or more fails the check.
  - Shadows: as the bed drops, the gantry's and the parts' shadows move across the plate and hide its
    glitter, which reads like dark filament. When every filament of the job is much lighter than the
    plate (60 levels brighter than its median in the reference photo), only patches that got brighter
    count as unexpected material; a darker patch is a shadow. Dark or unset colours count both ways.
  - Missing material: when at least 60% of the latest layer shows no change, the check fails if the
    job's filament colour is set and clearly differs from the plate, and is uncertain otherwise.
- **Replaying a checkpoint.** `scripts/shop_tests/failure_check_replay.cpp` reruns one comparison from
  a job folder and draws what the check saw.
- **Verdicts.** Pass: the print resumes silently. Fail: the printer holds and beeps, and a window shows
  the photo (red = unexplained material, orange = expected but not seen, green = outline of the
  lined-up G-code) next to a top view of the G-code, with Resume / Cancel print. Uncertain: the print
  resumes with no popup; the Device tab status line and the job folder record it. Without a camera
  calibration every checkpoint photo is shown for review and the print resumes after 25 s unless you
  press Keep paused.
- **Phone notifications** (Preferences > General > Phone notifications, `ShopNotify.*`). With an
  [ntfy](https://ntfy.sh) topic set (`shop_notify_topic`; empty = off, nothing is ever sent), a
  failed check also sends a high-priority notification with the job name, the checkpoint, the first
  reason and the checkpoint photo (`cpN.jpg`, uploaded as an attachment; if the server refuses the
  attachment the text is sent alone). Resume print / Cancel print in that window send a short
  follow-up. The server is `https://ntfy.sh` unless `shop_notify_server` names another one. Install
  the ntfy app on the phone and subscribe to the same topic; anyone who knows the topic can read
  it, so pick one that is hard to guess. "Send test notification" saves what is typed and sends one
  message to it, with the latest checkpoint photo attached when there is one. Pass, uncertain and review checkpoints never notify.
- **Camera calibration** (Device tab, printer idle, empty plate): the printer moves to 11 poses
  (`SHOP_CAMERA_POSE`); you click the plate corners at bed heights 10 and 170 mm and the nozzle tip
  at a 3 x 3 grid. The plate's exact size is solved for, so the printable area is only a first
  guess. Saved per printer in `shop/camera/<host>.json`.
- **Files.** Watched jobs: `shop/failure_check/jobs.json` (a restarted slicer resumes watching).
  Per job: `shop/failure_check/jobs/<time>_<name>/` with the G-code copy, every checkpoint photo
  (`cpN.jpg`) and its result (`cpN.json`); the last 15 jobs are kept.
- **Tests.** `scripts/shop_tests/failure_check_test.cpp` (build line at the top) checks the G-code
  planner on a real job, the calibration on a simulated wide-angle camera, and the comparison on
  rendered scenes (good first layer, missing first layer, support tower, spaghetti, a part knocked
  loose, an exposure change) plus two real snapshots for camera noise.
- **Limits.** Moonraker printers whose bed moves in Z (the Plus 4); uploads through the classic
  Print dialog, not `.gcode.3mf`. The bed strip under the park position (back, around X 95) is not
  checked. Material the same colour as the plate cannot be seen.

## Not yet verified on hardware

Box sync in the UI, the mapping push, and the slot-state semantics were built from the live
Moonraker queries of one Plus 4 but have not been exercised end to end with a print. The failure
check's printer macros, camera calibration and live checkpoints have not run on the printer yet;
the comparison has only been tested on simulated photos and two real snapshots.
