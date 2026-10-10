#pragma once

// Print-shop addition: named physical machines that bundle a printer preset (optionally a
// physical printer with its host), a process preset and filament presets, so one pick in the
// sidebar applies the whole configuration. Persisted as JSON under data_dir()/shop/printers.json.

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct ShopPrinter
{
    std::string              name;             // e.g. "Plus 4 #1"
    std::string              printer_preset;   // Orca printer preset name
    std::string              physical_printer; // full physical printer name ("<name> * <preset>"), or empty
    std::string              process_preset;   // may be empty
    std::vector<std::string> filament_presets; // per extruder, may be empty
    std::string              notes;
};

class ShopPrinters
{
public:
    static ShopPrinters& get();

    const std::vector<ShopPrinter>& list() const { return m_printers; }
    const ShopPrinter* find(const std::string& name) const;
    void add_or_replace(ShopPrinter printer);
    bool remove(const std::string& name);

    void load();
    bool save() const;
    static std::string path();

private:
    ShopPrinters() = default;
    std::vector<ShopPrinter> m_printers;
    bool m_loaded = false;
};

}} // namespace Slic3r::GUI
