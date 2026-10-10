#include "ShopNaming.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <boost/algorithm/string.hpp>
#include <cctype>
#include <map>
#include <regex>

namespace Slic3r { namespace GUI { namespace ShopNaming {

namespace {

const std::map<std::string, std::string> PRINTER_ABBR = {
    {"Qidi X-Plus 4", "XP4"},
    {"Qidi X-Max 4", "XM4"},
    {"Qidi Q1 Pro", "Q1P"},
    {"Qidi Q2", "Q2"},
    {"Creality Ender-3 Pro", "E3P"},
    {"Creality Ender-3", "E3"},
    {"Creality CR-10 Smart", "CR10S"},
    {"Creality CR-10 Smart Pro", "CR10SP"},
    {"Bambu Lab A1", "A1"},
    {"Bambu Lab A1 mini", "A1M"},
    {"Bambu Lab P1S", "P1S"},
    {"Bambu Lab X1 Carbon", "X1C"},
};

const std::map<std::string, std::string> VENDOR_ABBR = {
    {"qidi", "Q"},        {"sunlu", "SUN"},     {"overture", "OV"},  {"polymaker", "PM"},
    {"fiberon", "PM"},    {"bambu", "BBL"},     {"bambu lab", "BBL"}, {"generic", "GEN"},
    {"creality", "CR"},   {"esun", "ESUN"},     {"hatchbox", "HB"},  {"prusament", "PRU"},
};

std::string sanitize(std::string s)
{
    // keep letters, digits and + - . ; collapse anything else
    std::string out;
    for (unsigned char c : s)
        out += (std::isalnum(c) || c == '+' || c == '-' || c == '.') ? char(c) : '\0';
    out.erase(std::remove(out.begin(), out.end(), '\0'), out.end());
    return out;
}

std::string initials(const std::string& s)
{
    std::string out;
    bool new_word = true;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            if (new_word || std::isdigit(c))
                out += char(std::toupper(c));
            new_word = false;
        } else {
            new_word = true;
        }
    }
    return out;
}

} // namespace

std::string printer_abbr(const std::string& printer_model)
{
    auto it = PRINTER_ABBR.find(printer_model);
    if (it != PRINTER_ABBR.end())
        return it->second;
    std::string s = initials(printer_model);
    return s.empty() ? "PRN" : s;
}

std::string filament_abbr(const std::string& filament_vendor, const std::string& preset_name)
{
    std::string vendor = boost::algorithm::to_lower_copy(filament_vendor);
    std::string vabbr;
    auto it = VENDOR_ABBR.find(vendor);
    if (it != VENDOR_ABBR.end())
        vabbr = it->second;
    else if (!vendor.empty())
        vabbr = boost::algorithm::to_upper_copy(sanitize(vendor).substr(0, 3));

    // Product part: preset name before " @", minus a leading vendor word.
    std::string product = preset_name.substr(0, preset_name.find(" @"));
    boost::algorithm::trim(product);
    if (!filament_vendor.empty() && boost::algorithm::istarts_with(product, filament_vendor))
        product = product.substr(filament_vendor.size());
    boost::algorithm::trim(product);
    product = sanitize(boost::algorithm::replace_all_copy(product, " ", ""));
    if (product.empty())
        product = "FIL";
    return vabbr.empty() ? product : vabbr + "-" + product;
}

std::string config_suffix(const PresetBundle& bundle)
{
    const Preset& printer = bundle.printers.get_edited_preset();
    const std::string model = printer.config.opt_string("printer_model");

    std::string nozzle;
    if (const auto* nd = printer.config.option<ConfigOptionFloats>("nozzle_diameter"); nd && !nd->values.empty()) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", nd->values.front());
        nozzle = buf;
    }

    std::string filament;
    {
        const Preset* fil = nullptr;
        if (!bundle.filament_presets.empty())
            fil = bundle.filaments.find_preset(bundle.filament_presets.front(), false);
        if (!fil)
            fil = &bundle.filaments.get_edited_preset();
        std::string vendor;
        if (const auto* v = fil->config.option<ConfigOptionStrings>("filament_vendor"); v && !v->values.empty())
            vendor = v->values.front();
        filament = filament_abbr(vendor, fil->name);
    }

    const bool support = bundle.prints.get_edited_preset().config.opt_bool("enable_support");

    std::string s = "_" + printer_abbr(model) + "_" + filament;
    if (!nozzle.empty())
        s += "_" + nozzle;
    s += support ? "_sup" : "_nosup";
    return s;
}

std::string strip_config_suffix(const std::string& stem)
{
    // _<PRINTER>_<FILAMENT>[_<d.d>]_(sup|nosup) at the end, possibly repeated
    static const std::regex re(R"(_[A-Za-z0-9]+_[A-Za-z0-9+.\-]+?(?:_\d\.\d)?_(?:sup|nosup)$)");
    std::string out = stem;
    for (int guard = 0; guard < 4; ++guard) {
        std::string next = std::regex_replace(out, re, "");
        if (next == out)
            break;
        out = next;
    }
    return out.empty() ? stem : out;
}

std::string suggest_project_stem(const std::string& stem, const PresetBundle& bundle)
{
    return strip_config_suffix(stem) + config_suffix(bundle);
}

}}} // namespace Slic3r::GUI::ShopNaming
