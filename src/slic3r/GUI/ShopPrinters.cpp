#include "ShopPrinters.hpp"
#include "ShopFiles.hpp"

#include <algorithm>
#include <optional>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

ShopPrinters& ShopPrinters::get()
{
    static ShopPrinters instance;
    if (!instance.m_loaded)
        instance.load();
    return instance;
}

std::string ShopPrinters::path()
{
    return shop_path("printers.json").string();
}

const ShopPrinter* ShopPrinters::find(const std::string& name) const
{
    for (const auto& p : m_printers)
        if (p.name == name)
            return &p;
    return nullptr;
}

void ShopPrinters::add_or_replace(ShopPrinter printer)
{
    for (auto& p : m_printers)
        if (p.name == printer.name) {
            p = std::move(printer);
            return;
        }
    m_printers.push_back(std::move(printer));
    std::sort(m_printers.begin(), m_printers.end(), [](const ShopPrinter& a, const ShopPrinter& b) { return a.name < b.name; });
}

bool ShopPrinters::remove(const std::string& name)
{
    for (auto it = m_printers.begin(); it != m_printers.end(); ++it)
        if (it->name == name) {
            m_printers.erase(it);
            return true;
        }
    return false;
}

void ShopPrinters::load()
{
    m_loaded = true;
    m_printers.clear();
    const std::optional<nlohmann::json> j = read_json_file(path());
    if (!j || !j->is_object())
        return;
    auto printers = j->find("printers");
    if (printers == j->end() || !printers->is_array())
        return;
    for (const auto& item : *printers) {
        if (!item.is_object())
            continue;
        ShopPrinter p;
        p.name             = json_get(item, "name", "");
        p.printer_preset   = json_get(item, "printer_preset", "");
        p.physical_printer = json_get(item, "physical_printer", "");
        p.process_preset   = json_get(item, "process_preset", "");
        p.notes            = json_get(item, "notes", "");
        auto filaments     = item.find("filament_presets");
        if (filaments != item.end() && filaments->is_array())
            for (const auto& f : *filaments)
                if (f.is_string())
                    p.filament_presets.push_back(f.get<std::string>());
        if (!p.name.empty() && !p.printer_preset.empty())
            m_printers.push_back(std::move(p));
    }
}

bool ShopPrinters::save() const
{
    nlohmann::json j;
    j["version"]  = 1;
    j["printers"] = nlohmann::json::array();
    for (const auto& p : m_printers)
        j["printers"].push_back({{"name", p.name},
                                 {"printer_preset", p.printer_preset},
                                 {"physical_printer", p.physical_printer},
                                 {"process_preset", p.process_preset},
                                 {"filament_presets", p.filament_presets},
                                 {"notes", p.notes}});
    return write_json_file(path(), j);
}

}} // namespace Slic3r::GUI
