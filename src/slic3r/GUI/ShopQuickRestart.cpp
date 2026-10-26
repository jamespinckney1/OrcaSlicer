#include "ShopQuickRestart.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdio>
#include <iterator>
#include <regex>
#include <sstream>

namespace Slic3r { namespace GUI { namespace QuickRestart {

namespace {

bool starts_with(const std::string& s, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// The command of a line: leading blanks and any comment removed.
std::string command(const std::string& line)
{
    const size_t b = line.find_first_not_of(" \t");
    if (b == std::string::npos || line[b] == ';')
        return {};
    std::string c = line.substr(b, line.find(';', b) - b);
    while (!c.empty() && (c.back() == ' ' || c.back() == '\t'))
        c.pop_back();
    return c;
}

bool word_is(const std::string& cmd, const char* word)
{
    const size_t n = std::char_traits<char>::length(word);
    return starts_with(cmd, word) && (cmd.size() == n || cmd[n] == ' ' || cmd[n] == '\t');
}

// Tool select "T<n>"; -1 for anything else.
int tool_of(const std::string& cmd)
{
    static const std::regex re(R"(^T(\d{1,2})$)");
    std::smatch m;
    return std::regex_match(cmd, m, re) ? std::stoi(m[1].str()) : -1;
}

// Commands that start changing or unloading the filament in the nozzle (Orca's generic ones and the QIDI Box's).
bool starts_filament_change(const std::string& cmd)
{
    for (const char* w : {"M600", "M701", "M702", "TOOL_CHANGE_START", "CUT_FILAMENT", "UNLOAD_FILAMENT", "EXTRUDER_UNLOAD"})
        if (word_is(cmd, w))
            return true;
    return starts_with(cmd, "UNLOAD_T");
}

} // namespace

bool is_marker(const std::string& line)
{
    const std::string c = command(line);
    return word_is(c, "SHOP_RESTART_POINT") || word_is(c, "SHOP_RESTART_LIMIT");
}

std::string point_marker(size_t pos)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "SHOP_RESTART_POINT POS=%012zu ; shop quick restart: layer 1 starts here", pos);
    return buf;
}

Plan plan(const std::vector<std::string>& lines)
{
    Plan p;
    std::vector<size_t> layer_lines;
    for (size_t i = 0; i < lines.size(); ++i)
        if (starts_with(lines[i], ";LAYER_CHANGE"))
            layer_lines.push_back(i);
    p.layers = layer_lines.size();
    if (layer_lines.empty()) {
        p.reason = "the G-code has no layer change markers";
        return p;
    }
    // Before layer 1, or before the failure check's reference photo right above it.
    p.point_line = layer_lines.front();
    for (size_t i = p.point_line; i-- > 0;) {
        const std::string c = command(lines[i]);
        if (word_is(c, "SHOP_CHECKPOINT"))
            p.point_line = i;
        else if (!c.empty())
            break;
    }
    // The start G-code must leave relative extrusion and name the tool layer 1 prints with (when it names one).
    bool relative_e = false;
    for (size_t i = 0; i < p.point_line; ++i) {
        const std::string c = command(lines[i]);
        if (word_is(c, "M83"))
            relative_e = true;
        else if (word_is(c, "M82"))
            relative_e = false;
        else if (int t = tool_of(c); t >= 0)
            p.tool = t;
    }
    if (!relative_e) {
        p.reason = "layer 1 does not start with relative extrusion (M83)";
        return p;
    }
    // A restart puts the same filament down again, so it is offered only until the filament in the nozzle changes.
    const size_t end = layer_lines.size() > size_t(kMaxLayers) ? layer_lines[kMaxLayers] : lines.size();
    for (size_t i = p.point_line; i < end; ++i) {
        const std::string c = command(lines[i]);
        const int         t = tool_of(c);
        if (starts_filament_change(c) || (t >= 0 && t != p.tool)) {
            p.limit_line           = i;
            p.limit_is_tool_change = true;
            break;
        }
    }
    if (!p.limit_is_tool_change && end < lines.size())
        p.limit_line = end;
    p.ok = true;
    return p;
}

Blocker blocker(const PrinterState& st)
{
    if (!st.has_macros)
        return Blocker::NoMacros;
    if (st.state != "paused" || !st.paused)
        return Blocker::NotPaused;
    if (st.pos < 0 || st.size != st.file_size)
        return Blocker::NotMarked; // a job uploaded without markers, or the marker of an earlier job
    if (st.file_position <= st.pos)
        return Blocker::BeforeLayer1;
    if (st.ok != 1 || st.current_layer > kMaxLayers)
        return Blocker::PastLimit;
    for (char axis : {'x', 'y', 'z'})
        if (st.homed_axes.find(axis) == std::string::npos)
            return Blocker::NotHomed;
    return Blocker::None;
}

bool write_with_markers(const std::string& in_path, const std::string& out_path, Plan& plan_out, std::string& error)
{
    std::string data;
    {
        boost::nowide::ifstream in(in_path, std::ios::binary);
        if (!in) {
            error = "cannot read " + in_path;
            return false;
        }
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (data.find('\r') != std::string::npos) {
        // Klipper counts file positions in bytes of '\n'-separated lines; keep the arithmetic exact.
        error = "the G-code has CR line endings";
        return false;
    }
    std::vector<std::string> lines;
    const bool               final_newline = !data.empty() && data.back() == '\n';
    for (size_t b = 0; b < data.size();) {
        size_t e = data.find('\n', b);
        if (e == std::string::npos)
            e = data.size();
        std::string line = data.substr(b, e - b);
        if (!is_marker(line))
            lines.push_back(std::move(line));
        b = e + 1;
    }
    const size_t size = data.size();
    data.clear();
    plan_out = plan(lines);
    if (!plan_out.ok) {
        error = plan_out.reason;
        return false;
    }
    std::string out;
    out.reserve(size + 256);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == plan_out.point_line)
            out += point_marker(out.size()) + "\n";
        if (i == plan_out.limit_line)
            out += plan_out.limit_is_tool_change ? "SHOP_RESTART_LIMIT ; shop quick restart: not past a filament change\n" :
                                                   "SHOP_RESTART_LIMIT ; shop quick restart: not past layer 3\n";
        out += lines[i];
        if (i + 1 < lines.size() || final_newline)
            out += '\n';
    }
    const std::string tmp = out_path + ".restart.tmp";
    {
        boost::nowide::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(out.data(), std::streamsize(out.size()));
        if (!f.good()) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    boost::system::error_code ec;
    boost::filesystem::rename(tmp, out_path, ec);
    if (ec) {
        boost::filesystem::remove(tmp, ec);
        error = "cannot replace " + out_path;
        return false;
    }
    return true;
}

}}} // namespace Slic3r::GUI::QuickRestart
