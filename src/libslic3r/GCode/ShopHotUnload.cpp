#include "ShopHotUnload.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <vector>

namespace Slic3r {

int shop_unload_temperature(int print_temp, int top_temp)
{
    return std::min(std::max(print_temp + 30, top_temp), top_temp + 10);
}

namespace {

std::string trimmed(const std::string& line)
{
    size_t b = 0;
    while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b])))
        ++b;
    return line.substr(b);
}

bool starts_with(const std::string& s, const char* prefix) { return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0; }

// An extruder temperature command: its target and whether it waits. M104/M109 with T other than T0 are
// left alone (QIDI has one nozzle; a T parameter means another heater on other printers).
bool extruder_temp(const std::string& line, int& target, bool& waits, size_t& value_pos, size_t& value_len)
{
    static const std::regex m10x(R"(^(M10[49])((?:\s+[A-Z][-0-9.]*)*?)\s+S(\d+))", std::regex::icase);
    static const std::regex set_heater(R"(^SET_HEATER_TEMPERATURE\s+HEATER=extruder\s+TARGET=(\d+)(.*)$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(line, m, set_heater)) {
        target    = std::atoi(m[1].str().c_str());
        waits     = std::regex_search(m[2].str(), std::regex(R"(WAIT=1)", std::regex::icase));
        value_pos = size_t(m.position(1));
        value_len = size_t(m.length(1));
        return true;
    }
    if (std::regex_search(line, m, m10x)) {
        if (std::regex_search(m[2].str(), std::regex(R"(\bT[1-9])", std::regex::icase)))
            return false;
        target    = std::atoi(m[3].str().c_str());
        waits     = toupper(static_cast<unsigned char>(m[1].str()[3])) == '9';
        value_pos = size_t(m.position(3));
        value_len = size_t(m.length(3));
        return true;
    }
    return false;
}

} // namespace

std::string shop_hot_unload(const std::string& gcode, int unload_temp)
{
    if (gcode.find("UNLOAD_T") == std::string::npos || unload_temp <= 0)
        return gcode;
    std::vector<std::string> lines;
    {
        std::istringstream in(gcode);
        for (std::string l; std::getline(in, l);)
            lines.push_back(l);
    }
    size_t unload = lines.size(), flush_end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trimmed(lines[i]);
        if (unload == lines.size() && starts_with(t, "UNLOAD_T"))
            unload = i;
        if (starts_with(t, "; FLUSH_END"))
            flush_end = i;
    }
    if (unload == lines.size())
        return gcode;

    // Already waiting for a hot enough nozzle since the last temperature change before the unload?
    bool hot = false;
    for (size_t i = 0; i < unload; ++i) {
        int target; bool waits; size_t pos, len;
        if (extruder_temp(trimmed(lines[i]), target, waits, pos, len))
            hot = waits && target >= unload_temp;
    }
    // Keep it at least that hot until the purge is done. Without FLUSH_END markers: up to and including the
    // first temperature wait after the next T<n> line.
    size_t end = flush_end != lines.size() && flush_end > unload ? flush_end : lines.size();
    if (end == lines.size()) {
        bool after_t = false;
        for (size_t i = unload + 1; i < lines.size(); ++i) {
            const std::string t = trimmed(lines[i]);
            if (t.size() >= 2 && (t[0] == 'T' || t[0] == 't') && std::isdigit(static_cast<unsigned char>(t[1])))
                after_t = true;
            int target; bool waits; size_t pos, len;
            if (after_t && extruder_temp(t, target, waits, pos, len) && waits) {
                end = i + 1;
                break;
            }
        }
        if (end == lines.size())
            end = unload + 1;
    }
    for (size_t i = unload + 1; i < end; ++i) {
        std::string& line = lines[i];
        const size_t lead = line.size() - trimmed(line).size();
        int target; bool waits; size_t pos, len;
        if (extruder_temp(line.substr(lead), target, waits, pos, len) && target < unload_temp)
            line.replace(lead + pos, len, std::to_string(unload_temp));
    }
    std::string out;
    out.reserve(gcode.size() + 96);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == unload && !hot)
            out += "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=" + std::to_string(unload_temp) + " WAIT=1 ; shop: unload hot\n";
        out += lines[i];
        if (i + 1 < lines.size() || (!gcode.empty() && gcode.back() == '\n'))
            out += '\n';
    }
    return out;
}

} // namespace Slic3r
