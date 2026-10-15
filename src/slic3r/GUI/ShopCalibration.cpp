#include "ShopCalibration.hpp"
#include "ShopFiles.hpp"

#include <ctime>
#include <optional>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

ShopCalibration& ShopCalibration::get()
{
    static ShopCalibration instance;
    if (!instance.m_loaded)
        instance.load();
    return instance;
}

std::string ShopCalibration::path()
{
    return shop_path("calibration_results.json").string();
}

std::string ShopCalibration::today()
{
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
    return buf;
}

void ShopCalibration::load()
{
    m_loaded = true;
    m_results.clear();
    const std::optional<nlohmann::json> j = read_json_file(path());
    if (!j || !j->is_object())
        return;
    auto results = j->find("results");
    if (results == j->end() || !results->is_array())
        return;
    for (const auto& item : *results) {
        if (!item.is_object())
            continue;
        CalibResult r;
        r.printer_preset   = json_get(item, "printer_preset", "");
        r.filament_preset  = json_get(item, "filament_preset", "");
        r.machine          = json_get(item, "machine", "");
        r.pressure_advance = json_get(item, "pressure_advance", -1.0);
        r.flow_ratio       = json_get(item, "flow_ratio", -1.0);
        r.max_volumetric   = json_get(item, "max_volumetric", -1.0);
        r.temperature      = json_get(item, "temperature", -1);
        r.notes            = json_get(item, "notes", "");
        r.date             = json_get(item, "date", "");
        m_results.push_back(std::move(r));
    }
}

bool ShopCalibration::save() const
{
    nlohmann::json j;
    j["version"] = 1;
    j["results"] = nlohmann::json::array();
    for (const auto& r : m_results)
        j["results"].push_back({{"printer_preset", r.printer_preset}, {"filament_preset", r.filament_preset},
                                {"machine", r.machine}, {"pressure_advance", r.pressure_advance},
                                {"flow_ratio", r.flow_ratio}, {"max_volumetric", r.max_volumetric},
                                {"temperature", r.temperature}, {"notes", r.notes}, {"date", r.date}});
    return write_json_file(path(), j);
}

}} // namespace Slic3r::GUI
