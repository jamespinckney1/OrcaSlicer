#include "ShopPrintHistory.hpp"
#include "ShopFiles.hpp"

#include <algorithm>
#include <boost/algorithm/string.hpp>
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
    return shop_path("last_print_filament.json").string();
}

static nlohmann::json load_all()
{
    const std::optional<nlohmann::json> j = read_json_file(path());
    if (j && j->is_object()) {
        auto printers = j->find("printers");
        if (printers != j->end() && printers->is_object())
            return *printers;
    }
    return nlohmann::json::object();
}

std::optional<ShopFilamentRecord> last_filament(const std::string& printer_key)
{
    if (printer_key.empty())
        return std::nullopt;
    const nlohmann::json all = load_all();
    auto it = all.find(printer_key);
    if (it == all.end() || !it->is_object())
        return std::nullopt;
    ShopFilamentRecord r;
    r.preset = json_get(*it, "preset", "");
    r.type   = json_get(*it, "type", "");
    r.color  = json_get(*it, "color", "");
    r.when   = json_get(*it, "when", "");
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
    nlohmann::json root;
    root["version"]  = 1;
    root["printers"] = all;
    write_json_file(path(), root);
}

} // namespace ShopPrintHistory
}} // namespace Slic3r::GUI
