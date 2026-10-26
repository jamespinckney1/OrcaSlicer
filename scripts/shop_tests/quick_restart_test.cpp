// Standalone tests for the print-shop quick restart markers (src/slic3r/GUI/ShopQuickRestart.cpp).
//
// Build and run from the repository root (after the deps are built):
//   D=deps/build/arm64/OrcaSlicer_dep/usr/local
//   clang++ -std=c++17 -O2 -Isrc -I$D/include scripts/shop_tests/quick_restart_test.cpp src/slic3r/GUI/ShopQuickRestart.cpp \
//       -L$D/lib -lboost_filesystem -lboost_nowide -o /tmp/quick_restart_test && /tmp/quick_restart_test <job.gcode>...
// Real jobs: ~/Library/Application Support/OrcaSlicer/shop/failure_check/jobs/*/job.gcode. Synthetic jobs (refusals,
// a filament change in layer 1) always run. Every job is checked the way Klipper's virtual_sdcard reads it.
#include "slic3r/GUI/ShopQuickRestart.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r::GUI;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } else { std::printf("  ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

static std::string read_all(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static void write_all(const std::string& path, const std::string& data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

// Scratch files go to $TMPDIR (or /tmp).
static std::string tmp(const std::string& name)
{
    const char* d = std::getenv("TMPDIR");
    std::string dir = d && *d ? d : "/tmp";
    if (dir.back() != '/')
        dir += '/';
    return dir + name;
}

static bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// Like virtual_sdcard.work_handler: split on '\n', the position of a line is where it starts (what a macro on that
// line sees as printer.virtual_sdcard.file_position).
struct SdLine { size_t pos; std::string text; };
static std::vector<SdLine> sd_lines(const std::string& data)
{
    std::vector<SdLine> out;
    size_t              pos = 0;
    std::stringstream   ss(data);
    std::string         line;
    while (std::getline(ss, line)) {
        out.push_back({pos, line});
        pos += line.size() + 1;
    }
    return out;
}

static bool is_tool_change(const std::string& l)
{
    for (const char* w : {"TOOL_CHANGE_START", "CUT_FILAMENT", "UNLOAD_T", "UNLOAD_FILAMENT", "M600"})
        if (starts_with(l, w))
            return true;
    return false;
}

static void check_marked(const std::string& name, const std::string& in, const std::string& out)
{
    const auto lines = sd_lines(out);
    size_t     point = SIZE_MAX, limit = SIZE_MAX, points = 0, limits = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (starts_with(lines[i].text, "SHOP_RESTART_POINT")) { point = i; ++points; }
        if (starts_with(lines[i].text, "SHOP_RESTART_LIMIT")) { limit = i; ++limits; }
    }
    CHECK(points == 1, "%s: one restart point (%zu)", name.c_str(), points);
    if (point == SIZE_MAX)
        return;
    const size_t pos = std::stoull(lines[point].text.substr(lines[point].text.find("POS=") + 4));
    CHECK(pos == lines[point].pos, "%s: POS=%zu is the marker's own file position (%zu)", name.c_str(), pos, lines[point].pos);
    const std::string& next = lines[point + 1].text;
    CHECK(starts_with(next, "SHOP_CHECKPOINT N=0") || starts_with(next, ";LAYER_CHANGE"),
          "%s: the point is just before the reference photo or layer 1 (next: %.40s)", name.c_str(), next.c_str());
    size_t first_layer = SIZE_MAX;
    for (size_t i = 0; i < lines.size() && first_layer == SIZE_MAX; ++i)
        if (starts_with(lines[i].text, ";LAYER_CHANGE"))
            first_layer = i;
    CHECK(point < first_layer, "%s: the point is before the first layer change", name.c_str());
    CHECK(limits <= 1, "%s: at most one limit (%zu)", name.c_str(), limits);
    if (limit != SIZE_MAX) {
        int  layers = 0;
        bool change = false;
        for (size_t i = point; i < limit; ++i) {
            layers += starts_with(lines[i].text, ";LAYER_CHANGE");
            change |= is_tool_change(lines[i].text);
        }
        const bool by_tool = lines[limit].text.find("filament change") != std::string::npos;
        CHECK(!change, "%s: no filament change between the point and the limit", name.c_str());
        CHECK(by_tool ? layers <= 3 : layers == 3, "%s: limit after %d layer(s) (%s)", name.c_str(), layers, by_tool ? "filament change" : "layer 4");
        if (by_tool)
            CHECK(is_tool_change(lines[limit + 1].text) || starts_with(lines[limit + 1].text, "T"), "%s: the limit is just before the change (%.40s)",
                  name.c_str(), lines[limit + 1].text.c_str());
        else
            CHECK(starts_with(lines[limit + 1].text, ";LAYER_CHANGE"), "%s: the limit is just before layer 4", name.c_str());
    }
    // Removing the two markers gives back the input.
    std::string stripped;
    for (const auto& l : lines)
        if (!starts_with(l.text, "SHOP_RESTART_"))
            stripped += l.text + "\n";
    if (!in.empty() && in.back() != '\n')
        stripped.pop_back();
    CHECK(stripped == in, "%s: only the markers were added", name.c_str());
}

static void run_file(const std::string& name, const std::string& in_path, const std::string& out_path)
{
    const std::string in = read_all(in_path);
    QuickRestart::Plan p;
    std::string        err;
    if (!QuickRestart::write_with_markers(in_path, out_path, p, err)) {
        CHECK(false, "%s: marked (%s)", name.c_str(), err.c_str());
        return;
    }
    const std::string out = read_all(out_path);
    std::printf("  %s: %zu layers, tool %d, point at line %zu, limit %s\n", name.c_str(), p.layers, p.tool, p.point_line,
                p.limit_line == std::string::npos ? "none" : p.limit_is_tool_change ? "at a filament change" : "at layer 4");
    check_marked(name, in, out);
    // Marking a marked job again changes nothing (a re-upload of the same file).
    const std::string tmp2 = out_path + "2";
    CHECK(QuickRestart::write_with_markers(out_path, tmp2, p, err) && read_all(tmp2) == out, "%s: marking again is a no-op", name.c_str());
    std::remove(tmp2.c_str());
}

int main(int argc, char** argv)
{
    const std::string out = tmp("quick_restart_test.gcode");
    const std::string start =
        "; EXECUTABLE_BLOCK_START\nPRINT_START BED=60 HOTEND=210\nM83\nT1\nG1 X2 Y2 Z0.2 E3 F3000\nM106 S0\n";
    auto layer = [](double z, const std::string& body) {
        return ";LAYER_CHANGE\n;Z:" + std::to_string(z) + "\nG1 Z" + std::to_string(z) + "\n" + body;
    };
    std::printf("synthetic jobs\n");
    {
        std::string g = start + "SHOP_CHECKPOINT N=0 TIMEOUT=45 ; reference\n";
        for (int i = 1; i <= 6; ++i)
            g += layer(0.2 * i, "G1 X10 Y10 E1\nG1 X20 Y10 E1\n");
        write_all(tmp("qr_plain.gcode"), g);
        run_file("single filament", tmp("qr_plain.gcode"), out);
    }
    {
        std::string g = start;
        g += layer(0.2, "G1 X10 Y10 E1\nG1 Z15 F1200\nTOOL_CHANGE_START F=1 T=2\nCUT_FILAMENT T=1\nUNLOAD_T1\nT2\nG1 X20 Y10 E1\n");
        for (int i = 2; i <= 5; ++i)
            g += layer(0.2 * i, "G1 X10 Y10 E1\n");
        write_all(tmp("qr_change.gcode"), g);
        run_file("filament change in layer 1", tmp("qr_change.gcode"), out);
    }
    {
        std::string g = start + layer(0.2, "G1 X10 E1\n") + layer(0.4, "G1 X20 E1\n");
        write_all(tmp("qr_short.gcode"), g);
        QuickRestart::Plan p;
        std::string        err;
        CHECK(QuickRestart::write_with_markers(tmp("qr_short.gcode"), out, p, err) && p.limit_line == std::string::npos,
              "two-layer job: a point and no limit");
    }
    auto refused = [&](const char* what, const std::string& g) {
        write_all(tmp("qr_bad.gcode"), g);
        QuickRestart::Plan p;
        std::string        err;
        const bool         ok = QuickRestart::write_with_markers(tmp("qr_bad.gcode"), tmp("qr_bad.gcode"), p, err);
        CHECK(!ok && read_all(tmp("qr_bad.gcode")) == g, "%s: refused, file untouched (%s)", what, err.c_str());
    };
    refused("absolute extrusion", "M82\nT0\n" + layer(0.2, "G1 X1 E1\n"));
    refused("no layer markers", start + "G1 X1 E1\n");
    refused("CR line endings", "M83\r\n;LAYER_CHANGE\r\nG1 X1 E1\r\n");

    std::printf("when a restart is offered\n");
    {
        using QuickRestart::Blocker;
        QuickRestart::PrinterState st;
        st.has_macros = true; st.state = "paused"; st.paused = true; st.current_layer = 2;
        st.file_position = 50000; st.file_size = 900000; st.pos = 18929; st.size = 900000; st.ok = 1; st.homed_axes = "xyz";
        CHECK(QuickRestart::blocker(st) == Blocker::None, "paused in layer 2 of a marked job");
        auto with = [&](auto change, Blocker expect, const char* what) {
            QuickRestart::PrinterState s2 = st;
            change(s2);
            CHECK(QuickRestart::blocker(s2) == expect, "%s", what);
        };
        with([](auto& s) { s.has_macros = false; }, Blocker::NoMacros, "macros not installed");
        with([](auto& s) { s.state = "printing"; s.paused = false; }, Blocker::NotPaused, "printing");
        with([](auto& s) { s.size = 123; }, Blocker::NotMarked, "marker from another job");
        with([](auto& s) { s.pos = -1; }, Blocker::NotMarked, "job without markers");
        with([](auto& s) { s.file_position = 1000; }, Blocker::BeforeLayer1, "paused in the start G-code");
        with([](auto& s) { s.ok = 0; }, Blocker::PastLimit, "past the limit marker");
        with([](auto& s) { s.current_layer = 4; }, Blocker::PastLimit, "layer 4");
        with([](auto& s) { s.homed_axes = "xy"; }, Blocker::NotHomed, "Z no longer homed");
    }

    for (int i = 1; i < argc; ++i) {
        std::printf("%s\n", argv[i]);
        run_file("job " + std::to_string(i), argv[i], out);
    }
    std::remove(out.c_str());
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
