#pragma once

// Print-shop addition: filament changes on QIDI Box printers unload hot.
//
// Sticky filaments (matte PETG above all) jam when QIDI's UNLOAD_T macro pulls them out at print
// temperature. The Plus 4 printer profile's change G-code waits for a hotter nozzle before UNLOAD_T and
// keeps it through the purge; this is the safety net that keeps that behaviour when a printer profile
// without it is used (an overwritten system profile, a user preset, a project's own settings). On G-code
// that already does it, it changes nothing.

#include <string>

namespace Slic3r {

// Unload temperature for a filament: print temperature + 30 C, at least its top temperature and at most
// 10 C above it (the same rule as the Plus 4 change G-code).
int shop_unload_temperature(int print_temp, int top_temp);

// For processed change G-code that calls UNLOAD_T: makes the nozzle wait for `unload_temp` before the
// first UNLOAD_T line, unless it already waits for at least that, and raises any extruder temperature set
// below it between the unload and the end of the purge (the last "; FLUSH_END", else the next T<n> line's
// own temperature wait). G-code without UNLOAD_T is returned unchanged.
std::string shop_hot_unload(const std::string& gcode, int unload_temp);

} // namespace Slic3r
