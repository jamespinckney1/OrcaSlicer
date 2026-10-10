#include "ShopCalibration.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <ctime>
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
    return (boost::filesystem::path(data_dir()) / "shop" / "calibration_results.json").string();
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
    const std::string file = path();
    if (!boost::filesystem::exists(file))
        return;
    try {
        boost::nowide::ifstream in(file);
        nlohmann::json j = nlohmann::json::parse(in);
        for (const auto& item : j.value("results", nlohmann::json::array())) {
            CalibResult r;
            r.printer_preset   = item.value("printer_preset", "");
            r.filament_preset  = item.value("filament_preset", "");
            r.machine          = item.value("machine", "");
            r.pressure_advance = item.value("pressure_advance", -1.0);
            r.flow_ratio       = item.value("flow_ratio", -1.0);
            r.max_volumetric   = item.value("max_volumetric", -1.0);
            r.temperature      = item.value("temperature", -1);
            r.notes            = item.value("notes", "");
            r.date             = item.value("date", "");
            m_results.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ShopCalibration: cannot read " << file << ": " << e.what();
    }
}

bool ShopCalibration::save() const
{
    const std::string file = path();
    try {
        boost::filesystem::create_directories(boost::filesystem::path(file).parent_path());
        nlohmann::json j;
        j["version"] = 1;
        j["results"] = nlohmann::json::array();
        for (const auto& r : m_results)
            j["results"].push_back({{"printer_preset", r.printer_preset}, {"filament_preset", r.filament_preset},
                                    {"machine", r.machine}, {"pressure_advance", r.pressure_advance},
                                    {"flow_ratio", r.flow_ratio}, {"max_volumetric", r.max_volumetric},
                                    {"temperature", r.temperature}, {"notes", r.notes}, {"date", r.date}});
        boost::nowide::ofstream out(file);
        out << j.dump(2);
        return bool(out);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ShopCalibration: cannot write " << file << ": " << e.what();
        return false;
    }
}

}} // namespace Slic3r::GUI
