#pragma once

// Print-shop addition: QIDI Box slots for prints sent through the classic Print dialog.
//
// The printer loads each tool from the Box slot named by its value_t<n> saved variable. Box sync only copies
// filament types and colours into the slicer ("not including AMS slot information"), and the classic upload
// (the one with the failure check) never wrote value_t<n>, so a print used whatever map an earlier one left:
// a clear filament synced from slot 3 printed from the red slot. Before such an upload the slicer now asks
// which slot each filament the plate uses loads from, pre-filled from the sync window's choices, and writes
// the printer's tool map.

#include <map>
#include <string>
#include <vector>

class wxWindow;

namespace Slic3r {
struct AMSMapInfo;
namespace GUI {
namespace ShopBox {

// Remembers which Box slot each filament was synced from (the sync window's mapping; with "Overwriting",
// the trays in order), as the default for the next upload.
void remember_sync(const std::map<int, AMSMapInfo>& maps, bool overwrite);

struct UsedFilament
{
    int         index = 0; // 0-based filament (tool) index
    std::string colour;    // "#RRGGBB" or "#RRGGBBAA"
    std::string type;
};

// For a Moonraker printer with a QIDI Box: asks which slot each used filament loads from and writes the
// printer's tool map, or, when the plate's one filament is set to the external spool, turns the Box off for
// the print. Returns false when the user cancels the upload; true otherwise (also without a Box).
bool confirm_slots(wxWindow* parent, const std::string& print_host, const std::vector<UsedFilament>& used);

// Whether the selected printer is a QIDI one (the printers a Box can feed).
bool is_box_printer();
// Whether filament <filament_index> (0-based) feeds from the external spool with the Box off: the remembered
// upload choice, which the sidebar shows as "EXT" on the filament's badge. Always false on other printers.
bool is_external(int filament_index);
// Sets or clears that choice from the sidebar; the external spool goes to one filament at a time.
void set_external(int filament_index, bool external);

// SAVE_VARIABLE line for value_t<tool> = 'slot<slot>' in the quoting QIDI's Klipper accepts
// (VALUE='slot2' is rejected: Klipper strips the outer quotes and the bare word is not a literal).
std::string tool_map_gcode(int tool, int slot);

} // namespace ShopBox
}} // namespace Slic3r::GUI
