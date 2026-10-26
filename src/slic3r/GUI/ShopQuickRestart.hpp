#pragma once

// Print-shop addition: Quick restart, core logic (no GUI dependencies, so it can be tested on its own).
//
// A print that fails in its first layers is paused, the plate is cleared while it stays on the bed, and the
// same print starts again from layer 1 without homing, leveling or a new start G-code. The printer does it
// by rewinding the paused job's file position (Klipper's M26 works while a virtual-SD print is paused), so
// homing, the loaded bed mesh, the measured nozzle height, the Box tool map and the end G-code all stay as
// they are. See docs/shop/SHOP_FEATURES.md ("Quick restart") and docs/shop/plus4/quick_restart.cfg.
//
// At upload the slicer marks the job:
//   SHOP_RESTART_POINT POS=<byte offset of this line>   just before layer 1 (before the failure check's
//                                                        reference photo, so a restart takes a new one)
//   SHOP_RESTART_LIMIT                                   where a restart stops being offered: the start of
//                                                        layer 4, or the first filament change if earlier
// POS has a fixed width, so the marker's own length never depends on its value; the printer checks it
// against its own file position before trusting it.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace QuickRestart {

constexpr int kMaxLayers = 3; // a restart is offered while the print is within its first 3 layers

struct Plan
{
    bool        ok = false;
    std::string reason;                    // why the job cannot be restarted (English), when !ok
    size_t      point_line = 0;            // the restart point goes before this line (0-based, unmarked file)
    size_t      limit_line = std::string::npos; // the limit goes before this line; npos: the job ends first
    bool        limit_is_tool_change = false;
    int         tool       = -1;           // tool selected at layer 1 (-1: none named)
    size_t      layers     = 0;            // layer changes in the job
};

// Plans the markers on a job's lines (without '\n' and without markers).
Plan plan(const std::vector<std::string>& lines);

// Copies in_path to out_path (which may be the same file) with any old markers removed and new ones inserted.
// On false the output is untouched and `error` says why (no layers, absolute extrusion, CR line endings).
bool write_with_markers(const std::string& in_path, const std::string& out_path, Plan& plan_out, std::string& error);

bool is_marker(const std::string& line);

// The marker line for a byte offset.
std::string point_marker(size_t pos);

// What the printer reports, for deciding whether a restart can be offered now.
struct PrinterState
{
    bool        has_macros    = false; // gcode_macro SHOP_RESTART_POINT exists
    std::string state;                 // print_stats.state
    bool        paused        = false; // pause_resume.is_paused
    int         current_layer = -1;    // print_stats.info.current_layer (-1: not reported)
    long long   file_position = 0, file_size = 0; // virtual_sdcard
    long long   pos = -1, size = -1;   // recorded by SHOP_RESTART_POINT
    int         ok            = 0;     // SHOP_RESTART_POINT.ok: marker passed, limit not yet
    std::string homed_axes;            // toolhead.homed_axes
};

enum class Blocker { None, NoMacros, NotPaused, NotMarked, BeforeLayer1, PastLimit, NotHomed };

// Why a restart cannot be offered now (None: it can). Mirrors the checks of SHOP_QUICK_RESTART on the printer.
Blocker blocker(const PrinterState& st);

}}} // namespace Slic3r::GUI::QuickRestart
