#pragma once

// Print-shop addition: remembers, per printer, which filament the last print sent from this
// slicer started with. Quick start compares the next print against it to decide whether the
// printer must run its full purge. Stored in data_dir()/shop/last_print_filament.json.

#include <optional>
#include <string>

namespace Slic3r { namespace GUI {

struct ShopFilamentRecord
{
    std::string preset; // filament preset name
    std::string type;   // filament_type
    std::string color;  // #RRGGBB
    std::string when;   // local time the print was sent

    // Same filament in the nozzle: same preset and same colour.
    bool same_filament(const ShopFilamentRecord& other) const;
};

namespace ShopPrintHistory {
std::optional<ShopFilamentRecord> last_filament(const std::string& printer_key);
void                              store_last_filament(const std::string& printer_key, const ShopFilamentRecord& record);
std::string                       path();
} // namespace ShopPrintHistory

}} // namespace Slic3r::GUI
