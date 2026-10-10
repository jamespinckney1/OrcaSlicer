#include "ShopPrintHistory.hpp"

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <ctime>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

static std::string norm_color(std::string c)
{
    c.erase(std::remove(c.begin(), c.end(), '#'), c.end());
    if (c.size() > 6)
        c = c.substr(0, 6);
    boost::algorithm::to_upper(c);
    return c;
}

bool ShopFilamentRecord::same_filament(const ShopFilamentRecord& other) const
{
    return !preset.empty() && preset == other.preset && norm_color(color) == norm_color(other.color);
}

namespace ShopPrintHistory {

std::string path()
{
    return (boost::filesystem::path(data_dir()) / "shop" / "last_print_filament.json").string();
}

static nlohmann::json load_all()
{
    const std::string file = path();
    if (!boost::filesystem::exists(file))
        return nlohmann::json::object();
    try {
        boost::nowide::ifstream in(file);
        nlohmann::json j = nlohmann::json::parse(in);
        if (j.is_object() && j.contains("printers") && j["printers"].is_object())
            return j["printers"];
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "ShopPrintHistory: cannot read " << file << ": " << e.what();
    }
    return nlohmann::json::object();
}

std::optional<ShopFilamentRecord> last_filament(const std::string& printer_key)
{
    const nlohmann::json all = load_all();
    if (printer_key.empty() || !all.contains(printer_key) || !all[printer_key].is_object())
        return std::nullopt;
    const auto& j = all[printer_key];
    ShopFilamentRecord r;
    r.preset = j.value("preset", "");
    r.type   = j.value("type", "");
    r.color  = j.value("color", "");
    r.when   = j.value("when", "");
    return r;
}

void store_last_filament(const std::string& printer_key, const ShopFilamentRecord& record)
{
    if (printer_key.empty())
        return;
    nlohmann::json all = load_all();
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    all[printer_key] = {{"preset", record.preset}, {"type", record.type}, {"color", record.color}, {"when", buf}};
    const std::string file = path();
    try {
        boost::filesystem::create_directories(boost::filesystem::path(file).parent_path());
        nlohmann::json root;
        root["version"]  = 1;
        root["printers"] = all;
        boost::nowide::ofstream out(file);
        out << root.dump(2);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "ShopPrintHistory: cannot write " << file << ": " << e.what();
    }
}

} // namespace ShopPrintHistory
}} // namespace Slic3r::GUI
