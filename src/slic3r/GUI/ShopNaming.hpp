#pragma once

// Print-shop addition: suggested project file names that encode the configuration,
// e.g. "bracket_XP4_SUN-PLA+_0.4_sup.3mf" (file, printer, filament, nozzle, support).

#include <string>

namespace Slic3r {
class PresetBundle;
namespace GUI {

namespace ShopNaming {

// app_config keys
constexpr const char* kAutoNameKey   = "shop_project_autoname"; // "1" to suggest names
constexpr const char* kProjectDirKey = "shop_project_dir";      // default folder for project files

std::string printer_abbr(const std::string& printer_model);
std::string filament_abbr(const std::string& filament_vendor, const std::string& preset_name);

// The suffix for the current presets: "_XP4_SUN-PLA+_0.4_sup".
std::string config_suffix(const PresetBundle& bundle);

// Strip a suffix this module previously appended, so re-saving does not stack them.
std::string strip_config_suffix(const std::string& stem);

// "<stem>_<suffix>" with any earlier suffix removed first.
std::string suggest_project_stem(const std::string& stem, const PresetBundle& bundle);

} // namespace ShopNaming
}} // namespace Slic3r::GUI
