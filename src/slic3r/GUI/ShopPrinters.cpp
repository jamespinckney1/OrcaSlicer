#include "ShopPrinters.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
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
    return (boost::filesystem::path(data_dir()) / "shop" / "printers.json").string();
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
    const std::string file = path();
    if (!boost::filesystem::exists(file))
        return;
    try {
        boost::nowide::ifstream in(file);
        nlohmann::json j = nlohmann::json::parse(in);
        for (const auto& item : j.value("printers", nlohmann::json::array())) {
            ShopPrinter p;
            p.name             = item.value("name", "");
            p.printer_preset   = item.value("printer_preset", "");
            p.physical_printer = item.value("physical_printer", "");
            p.process_preset   = item.value("process_preset", "");
            p.notes            = item.value("notes", "");
            for (const auto& f : item.value("filament_presets", nlohmann::json::array()))
                if (f.is_string())
                    p.filament_presets.push_back(f.get<std::string>());
            if (!p.name.empty() && !p.printer_preset.empty())
                m_printers.push_back(std::move(p));
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ShopPrinters: cannot read " << file << ": " << e.what();
    }
}

bool ShopPrinters::save() const
{
    const std::string file = path();
    try {
        boost::filesystem::create_directories(boost::filesystem::path(file).parent_path());
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
        boost::nowide::ofstream out(file);
        out << j.dump(2);
        return bool(out);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ShopPrinters: cannot write " << file << ": " << e.what();
        return false;
    }
}

}} // namespace Slic3r::GUI
