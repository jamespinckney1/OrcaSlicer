// Replays one failure-check comparison from a saved job folder and draws what the check saw.
//
// Build (from the repository root):
//   D=deps/build/arm64/OrcaSlicer_dep/usr/local/include
//   clang++ -std=c++17 -O2 -Isrc -I$D/eigen3 -I$D -Ideps_src scripts/shop_tests/failure_check_replay.cpp \
//       src/slic3r/GUI/FailureCheck.cpp -o /tmp/fc_replay
// Convert the photos first (macOS): sips -s format bmp cpN.jpg --out cpN.bmp
// Run: fc_replay job.gcode camera.json cp0.bmp <z0> cpN.bmp <zN> <N> out.ppm [prior dx dy ...]
// Overlay: red = unexpected, orange = missing, blue = changed and expected, green = expected outline.
#include "slic3r/GUI/FailureCheck.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace Slic3r::GUI::FailureCheck;

static bool read_bmp(const std::string& path, Image& img)
{
    std::ifstream        f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 54)
        return false;
    auto u32 = [&](size_t o) { return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24; };
    const uint32_t off = u32(10);
    const int      w   = int(u32(18));
    int            h   = int(int32_t(u32(22)));
    const int      bpp = d[28] | d[29] << 8;
    const bool     bu  = h > 0;
    h                  = std::abs(h);
    const int    by    = bpp / 8;
    const size_t st    = ((size_t(w) * by + 3) / 4) * 4;
    img.w              = w;
    img.h              = h;
    img.rgb.assign(size_t(w) * h * 3, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* r = &d[off + st * (bu ? h - 1 - y : y)];
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                img.rgb[(size_t(y) * w + x) * 3 + c] = r[x * by + 2 - c];
    }
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 9) {
        std::puts("usage: fc_replay job.gcode camera.json base.bmp z0 cur.bmp zc N out.ppm");
        return 2;
    }
    GCodeModel  m;
    std::string err;
    if (!parse_gcode(argv[1], m, err)) {
        std::puts(err.c_str());
        return 1;
    }
    std::ifstream     jf(argv[2]);
    std::stringstream ss;
    ss << jf.rdbuf();
    CameraModel cam;
    CameraModel::from_json(ss.str(), cam);
    Image base, cur;
    read_bmp(argv[3], base);
    read_bmp(argv[5], cur);
    const double      z0 = atof(argv[4]), zc = atof(argv[6]);
    const Checkpoint* cp = m.checkpoint(atoi(argv[7]));
    if (!cp) {
        std::puts("no such checkpoint");
        return 1;
    }
    const CheckResult r = compare(m, *cp, cam, base, z0, cur, zc);
    std::printf("cp%d %s: %s unexpected=%.0f mm2 missing=%.2f footprint=%d valid=%d/%d threshold=%.1f colour=%s%s\n", cp->index,
                checkpoint_label(*cp).c_str(), r.verdict == Verdict::Pass ? "PASS" : r.verdict == Verdict::Fail ? "FAIL" : "UNSURE",
                r.unexpected_mm2, r.missing_fraction, r.footprint_cells, r.valid_cells, r.bed_cells, r.threshold,
                m.first_layer_colour().c_str(), r.light_filament ? " (light: darker patches are shadows)" : "");
    for (const auto& a : r.alignment.anchors)
        std::printf("   part at (%.0f,%.0f) offset (%.0f,%.0f)\n", a.x, a.y, a.dx, a.dy);
    for (const auto& s : r.reasons)
        std::printf("   - %s\n", s.c_str());
    Image img = cur;
    auto  put = [&](double u, double v, uint8_t R, uint8_t G, uint8_t B) {
        const int x = int(u), y = int(v);
        if (x < 0 || y < 0 || x >= img.w || y >= img.h)
            return;
        uint8_t* p = &img.rgb[(size_t(y) * img.w + x) * 3];
        p[0] = R; p[1] = G; p[2] = B;
    };
    for (int gy = 0; gy < r.grid_h; ++gy)
        for (int gx = 0; gx < r.grid_w; ++gx) {
            const size_t  i = size_t(gy) * r.grid_w + gx;
            const uint8_t s = r.cells[i];
            double        u, v;
            if (!cam.project(r.grid_x0 + (gx + 0.5) * r.cell_mm, r.grid_y0 + (gy + 0.5) * r.cell_mm, -zc, u, v))
                continue;
            if (s == CellUnexpected) put(u, v, 255, 40, 40);
            else if (s == CellMissing) put(u, v, 255, 170, 0);
            else if (s == CellExpected) put(u, v, 60, 140, 255);
            const bool edge = !r.expected.empty() && r.expected[i] && gx > 0 && gy > 0 && gx + 1 < r.grid_w && gy + 1 < r.grid_h &&
                              !(r.expected[i - 1] && r.expected[i + 1] && r.expected[i - r.grid_w] && r.expected[i + r.grid_w]);
            if (edge)
                put(u, v, 60, 255, 90);
        }
    std::ofstream o(argv[8], std::ios::binary);
    o << "P6\n" << img.w << " " << img.h << "\n255\n";
    o.write(reinterpret_cast<const char*>(img.rgb.data()), std::streamsize(img.rgb.size()));
    return 0;
}
