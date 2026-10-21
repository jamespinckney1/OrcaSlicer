// Standalone tests for the print-shop failure check core (src/slic3r/GUI/FailureCheck.cpp).
//
// Build and run from the repository root (after the deps are built):
//   D=deps/build/arm64/OrcaSlicer_dep/usr/local/include
//   clang++ -std=c++17 -O2 -Isrc -I$D/eigen3 -I$D -Ideps_src scripts/shop_tests/failure_check_test.cpp \
//       src/slic3r/GUI/FailureCheck.cpp -o /tmp/failure_check_test && /tmp/failure_check_test <data dir>
// The data dir may hold anttrap.gcode (any sliced job; section 1 expects that job's 140 layers) and
// cam_a.bmp / cam_b.bmp (two snapshots of the same scene); missing files skip those sections.
#include "slic3r/GUI/FailureCheck.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r::GUI::FailureCheck;
static constexpr double kPatchOk = 20.0;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } else { std::printf("  ok: " __VA_ARGS__); std::printf("\n"); } } while (0)

// ---------- helpers ----------
static uint32_t rng_state = 12345;
static double rnd() { rng_state = rng_state * 1664525u + 1013904223u; return (rng_state >> 8) / double(1 << 24); }
static double gauss() { double s = 0; for (int i = 0; i < 6; ++i) s += rnd(); return (s - 3.0) / std::sqrt(0.5); }

static bool read_bmp(const std::string& path, Image& img)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    auto u32 = [&](size_t o) { return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24; };
    auto u16 = [&](size_t o) { return uint16_t(d[o] | d[o + 1] << 8); };
    const uint32_t off = u32(10);
    const int w = int(u32(18)); int h = int(int32_t(u32(22)));
    const int bpp = u16(28);
    const bool bottom_up = h > 0; h = std::abs(h);
    const int bytes = bpp / 8; const size_t stride = ((size_t(w) * bytes + 3) / 4) * 4;
    img.w = w; img.h = h; img.rgb.assign(size_t(w) * h * 3, 0);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &d[off + stride * (bottom_up ? h - 1 - y : y)];
        for (int x = 0; x < w; ++x) {
            img.rgb[(size_t(y) * w + x) * 3 + 0] = row[x * bytes + 2];
            img.rgb[(size_t(y) * w + x) * 3 + 1] = row[x * bytes + 1];
            img.rgb[(size_t(y) * w + x) * 3 + 2] = row[x * bytes + 0];
        }
    }
    return true;
}

static void write_ppm(const std::string& path, const Image& img)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << img.w << " " << img.h << "\n255\n";
    f.write(reinterpret_cast<const char*>(img.rgb.data()), img.rgb.size());
}

// ---------- synthetic camera, placed like the Plus 4's (front-left, high, wide angle) ----------
struct TruthCam {
    Eigen::Vector3d C; Eigen::Matrix3d R; double f, k1; int w = 640, h = 480;
    CameraModel model() const {
        CameraModel m; m.width = w; m.height = h; m.k1 = k1; m.valid = true;
        Eigen::Matrix3d K; K << f, 0, w / 2.0, 0, f, h / 2.0, 0, 0, 1;
        Eigen::Matrix<double, 3, 4> Rt; Rt.block<3, 3>(0, 0) = R; Rt.col(3) = -R * C;
        Eigen::Matrix<double, 3, 4> P = K * Rt;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m.P[r * 4 + c] = P(r, c);
        return m;
    }
    // ray for an observed pixel (undistort first)
    Eigen::Vector3d ray(double u, double v) const {
        const double fn = w / 2.0, cx = w / 2.0, cy = h / 2.0;
        const double xd = (u - cx) / fn, yd = (v - cy) / fn, rd = std::hypot(xd, yd);
        double ru = rd; for (int i = 0; i < 30; ++i) ru = rd / (1 + k1 * ru * ru);
        const double s = rd > 1e-12 ? ru / rd : 1.0;
        const double uu = cx + fn * xd * s, vv = cy + fn * yd * s;
        Eigen::Vector3d dcam((uu - w / 2.0) / f, (vv - h / 2.0) / f, 1.0);
        return (R.transpose() * dcam).normalized();
    }
};

static TruthCam make_cam()
{
    TruthCam t; t.C = {-10, -20, 70}; t.f = 290; t.k1 = -0.16;
    const Eigen::Vector3d T(190, 200, -70), up(0, 0, 1);
    const Eigen::Vector3d d = (T - t.C).normalized();
    const Eigen::Vector3d r = d.cross(up).normalized();
    const Eigen::Vector3d dn = d.cross(r);
    t.R.row(0) = r; t.R.row(1) = dn; t.R.row(2) = d;
    return t;
}

struct Box { double x0, x1, y0, y1, height; uint8_t col[3]; };

// Ray-traced scene: plate (0..305) at frame height -Z, boxes standing on it, the parked toolhead.
static Image render(const TruthCam& cam, double Z, const std::vector<Box>& boxes, double gain = 1.0, double noise = 3.0)
{
    Image img; img.w = cam.w; img.h = cam.h; img.rgb.assign(size_t(img.w) * img.h * 3, 0);
    std::vector<Box> all = boxes;
    // parked toolhead (frame-fixed): x 95 +-25, y 324 -30..+10, h 0..70  -> as a "box" in bed coords at bed height Z..Z+70
    Box head{70, 120, 294, 334, 0, {200, 200, 205}};
    for (int v = 0; v < img.h; ++v)
        for (int u = 0; u < img.w; ++u) {
            const Eigen::Vector3d d = cam.ray(u + 0.5, v + 0.5);
            double best_t = 1e18; double col[3] = {18, 18, 20};
            // bed plane
            if (d.z() < -1e-9) {
                const double t = (-Z - cam.C.z()) / d.z();
                const Eigen::Vector3d p = cam.C + t * d;
                if (t > 0) {
                    best_t = t;
                    if (p.x() >= 0 && p.x() <= 305 && p.y() >= 0 && p.y() <= 305) {
                        const uint32_t hsh = uint32_t(int(p.x() * 3) * 73856093) ^ uint32_t(int(p.y() * 3) * 19349663);
                        const double sparkle = ((hsh >> 7) % 100) < 4 ? 60 : 0;
                        const double g = 58 + ((hsh >> 3) % 21) - 10 + sparkle;
                        col[0] = g; col[1] = g; col[2] = g + 3;
                    } else { col[0] = col[1] = col[2] = 30; }
                }
            }
            auto hit_box = [&](const Box& b, double zlo, double zhi, const uint8_t c[3]) {
                double tmin = 0, tmax = 1e18; int axis = -1;
                const double lo[3] = {b.x0, b.y0, zlo}, hi[3] = {b.x1, b.y1, zhi};
                for (int a = 0; a < 3; ++a) {
                    if (std::abs(d[a]) < 1e-12) { if (cam.C[a] < lo[a] || cam.C[a] > hi[a]) return; continue; }
                    double t1 = (lo[a] - cam.C[a]) / d[a], t2 = (hi[a] - cam.C[a]) / d[a];
                    if (t1 > t2) std::swap(t1, t2);
                    if (t1 > tmin) { tmin = t1; axis = a; }
                    tmax = std::min(tmax, t2);
                    if (tmin > tmax) return;
                }
                if (tmin > 0 && tmin < best_t) {
                    best_t = tmin;
                    const double shade = axis == 2 ? 1.0 : axis == 0 ? 0.82 : 0.9;
                    for (int k = 0; k < 3; ++k) col[k] = c[k] * shade;
                }
            };
            for (const Box& b : all) hit_box(b, -Z, -Z + b.height, b.col);
            hit_box(head, 0, 70, head.col);
            for (int k = 0; k < 3; ++k) {
                const double val = col[k] * gain + noise * gauss();
                img.rgb[(size_t(v) * img.w + u) * 3 + k] = uint8_t(std::clamp(val, 0.0, 255.0));
            }
        }
    return img;
}

// Synthetic G-code model: each box printed as 0.2 mm layers of raster lines 1 mm apart.
static GCodeModel make_model(const std::vector<Box>& boxes, double printed_height)
{
    GCodeModel m; m.bed_min_x = 0; m.bed_min_y = 0; m.bed_max_x = 305; m.bed_max_y = 305;
    const int layers = int(std::lround(printed_height / 0.2));
    for (int L = 0; L < std::max(layers, 1) + 1; ++L) {
        m.layer_first_segment.push_back(m.segments.size()); m.layer_line.push_back(L * 100); m.layer_z.push_back(0.2 * (L + 1));
        if (L >= layers) { m.layer_z.back() = 0.2 * (L + 1); }
        const float z = float(0.2 * (L + 1));
        for (const Box& b : boxes) {
            if (z > b.height + 1e-6) continue;
            for (double y = b.y0 + 0.5; y < b.y1; y += 1.0) {
                m.segments.push_back({float(b.x0 + 0.2), float(y), float(b.x1 - 0.2), float(y), z, 0.45f});
                m.segment_line.push_back(L * 100 + 1);
            }
        }
    }
    return m;
}

static void write_cells(const std::string& path, const CheckResult& r)
{
    Image img; img.w = r.grid_w * 3; img.h = r.grid_h * 3; img.rgb.assign(size_t(img.w) * img.h * 3, 0);
    const uint8_t pal[5][3] = {{0, 0, 0}, {70, 70, 70}, {60, 160, 220}, {240, 50, 50}, {250, 170, 30}};
    for (int gy = 0; gy < r.grid_h; ++gy) for (int gx = 0; gx < r.grid_w; ++gx) {
        const uint8_t* c = pal[r.cells[size_t(gy) * r.grid_w + gx]];
        for (int dy = 0; dy < 3; ++dy) for (int dx = 0; dx < 3; ++dx) {
            const int px = gx * 3 + dx, py = (r.grid_h - 1 - gy) * 3 + dy;
            std::memcpy(&img.rgb[(size_t(py) * img.w + px) * 3], c, 3); } }
    write_ppm(path, img);
}

static const char* vname(Verdict v) { return v == Verdict::Pass ? "PASS" : v == Verdict::Fail ? "FAIL" : "UNSURE"; }

static void report(const char* name, const CheckResult& r)
{
    std::printf("  [%s] %s unexpected=%.0f mm2 missing=%.2f (fp %d) valid=%d/%d thr=%.1f\n", name, vname(r.verdict), r.unexpected_mm2,
                r.missing_fraction, r.footprint_cells, r.valid_cells, r.bed_cells, r.threshold);
    for (auto& s : r.reasons) std::printf("      - %s\n", s.c_str());
}

int main(int argc, char** argv)
{
    const std::string data = argc > 1 ? argv[1] : ".";
    // ---------- 1. real G-code ----------
    std::printf("1. G-code parsing and checkpoint planning\n");
    std::string err;
    const std::string g = data + "/anttrap.gcode";
    if (!std::ifstream(g)) {
        std::printf("  (no %s, skipped)\n", g.c_str());
    } else {
    GCodeModel m;
    CHECK(parse_gcode(g, m, err), "parse %s %s", g.c_str(), err.c_str());
    std::printf("  layers=%zu segments=%zu bed=%.0f..%.0f x %.0f..%.0f first-layer z=%.2f last z=%.2f\n", m.layer_first_segment.size(),
                m.segments.size(), m.bed_min_x, m.bed_max_x, m.bed_min_y, m.bed_max_y, m.layer_z.front(), m.layer_z.back());
    for (auto& cp : m.checkpoints)
        std::printf("    N=%d %-18s line=%zu done=%zu layer=%zu\n", cp.index, checkpoint_label(cp).c_str(), cp.line, cp.segments_done, cp.layer);
    CHECK(m.checkpoints.size() == 8, "8 checkpoints planned (got %zu)", m.checkpoints.size());
    CHECK(m.layer_first_segment.size() == 140, "140 layers (got %zu)", m.layer_first_segment.size());
    const std::string out = data + "/anttrap_cp.gcode";
    CHECK(write_with_checkpoints(g, out, m.checkpoints, 45, err), "write with checkpoints %s", err.c_str());
    GCodeModel m2;
    CHECK(parse_gcode(out, m2, err), "reparse");
    bool same = m2.checkpoints.size() == m.checkpoints.size();
    for (size_t i = 0; same && i < m.checkpoints.size(); ++i)
        same = m2.checkpoints[i].line == m.checkpoints[i].line && m2.checkpoints[i].segments_done == m.checkpoints[i].segments_done;
    for (size_t i = 0; same && i < m.checkpoints.size(); ++i)
        same = m2.checkpoints[i].index == m.checkpoints[i].index && m2.checkpoints[i].kind == m.checkpoints[i].kind &&
               m2.checkpoints[i].layer == m.checkpoints[i].layer && m2.checkpoints[i].percent == m.checkpoints[i].percent;
    CHECK(same, "re-parsing the written file reads back the same checkpoints (index, kind, line, layer)");
    // write again (idempotent) and strip
    CHECK(write_with_checkpoints(out, out, m2.checkpoints, 45, err), "rewrite in place");
    const std::string stripped = data + "/anttrap_stripped.gcode";
    CHECK(write_with_checkpoints(out, stripped, {}, 45, err), "strip");
    {
        std::ifstream a(g), b(stripped); std::string la, lb; size_t n = 0, diffs = 0;
        while (true) { bool ga = bool(std::getline(a, la)), gb = bool(std::getline(b, lb)); if (!ga && !gb) break; if (ga != gb || la != lb) ++diffs; ++n; }
        CHECK(diffs == 0, "stripped copy equals the original (%zu lines, %zu differ)", n, diffs);
        std::ifstream c(out); size_t count = 0; std::string l;
        while (std::getline(c, l)) if (l.rfind("SHOP_CHECKPOINT", 0) == 0) { ++count; if (count <= 8) std::printf("    %s\n", l.c_str()); }
        CHECK(count == 8, "exactly 8 SHOP_CHECKPOINT lines after rewriting twice (got %zu)", count);
    }

    }

    // ---------- 2. calibration ----------
    std::printf("2. Camera calibration from clicked points\n");
    const TruthCam truth = make_cam();
    const CameraModel tm = truth.model();
    std::vector<Correspondence> pts;
    // The dialog's procedure: plate corners at bed heights 5 and 170 (the real plate is larger than the
    // printable area, so the corners start from a guess and are refined), then the nozzle tip at a 3 x 3 grid.
    const double true_c[4][2] = {{-3, -6}, {309, -6}, {309, 312}, {-3, 312}};
    const double guess_c[4][2] = {{0, 0}, {305, 0}, {305, 305}, {0, 305}};
    for (double Z : {5.0, 170.0}) {
        int vis = 0;
        for (int c = 0; c < 4; ++c) {
            double u, v;
            if (tm.project(true_c[c][0], true_c[c][1], -Z, u, v) && u >= 0 && v >= 0 && u < 640 && v < 480) {
                Correspondence k{guess_c[c][0], guess_c[c][1], -Z, u + 1.5 * gauss(), v + 1.5 * gauss()};
                k.group = c;
                pts.push_back(k);
                ++vis;
            }
        }
        std::printf("    Z=%.0f: %d plate corners visible\n", Z, vis);
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double x = 20 + 132.5 * i, y = 20 + 132.5 * j;
            double u, v;
            if (tm.project(x, y, 0, u, v) && u >= 0 && v >= 0 && u < 640 && v < 480)
                pts.push_back({x, y, 0, u + 1.5 * gauss(), v + 1.5 * gauss()});
        }
    CameraModel cal;
    CHECK(calibrate_camera(pts, 640, 480, cal, err), "calibrate with %zu points %s", pts.size(), err.c_str());
    std::printf("    rms=%.2f px  k1 found %.3f (truth %.3f)\n", cal.rms_px, cal.k1, truth.k1);
    double worst = 0;
    for (double Z : {5.0, 40.0, 120.0, 200.0})
        for (double x = 10; x <= 295; x += 20)
            for (double y = 10; y <= 295; y += 20) {
                double u0, v0, u1, v1;
                if (!tm.project(x, y, -Z, u0, v0) || u0 < 0 || v0 < 0 || u0 >= 640 || v0 >= 480) continue;
                if (!cal.project(x, y, -Z, u1, v1)) { worst = 1e9; continue; }
                worst = std::max(worst, std::hypot(u1 - u0, v1 - v0));
            }
    CHECK(worst < 12.0, "worst projection error over the visible bed at 4 heights: %.2f px", worst);
    {
        CameraModel back; CHECK(CameraModel::from_json(cal.to_json(), back) && std::abs(back.P[5] - cal.P[5]) < 1e-9, "json round trip");
    }

    // ---------- 3. comparisons on rendered scenes ----------
    std::printf("3. Comparisons (calibrated model, rendered photos)\n");
    const double Z0 = 6.0; // reference photo: after the start G-code (Z1) + 5 mm lift
    const Image base = render(truth, Z0, {});
    write_ppm(data + "/syn_base.ppm", base);
    const uint8_t white[3] = {225, 225, 220}, teal[3] = {40, 170, 160}, plate[3] = {60, 60, 63};
    {
        Box part{130, 170, 130, 170, 0.2, {}}; std::memcpy(part.col, white, 3);
        GCodeModel sm = make_model({part}, 0.2);
        Checkpoint cp; cp.kind = CheckpointKind::FirstLayer; cp.layer = 1; cp.segments_done = sm.layer_first_segment[1];
        const Image cur = render(truth, 5.2, {part});
        auto r = compare(sm, cp, cal, base, Z0, cur, 5.2); report("A first layer printed", r);
        CHECK(r.verdict == Verdict::Pass, "A passes");
        write_cells(data + "/cells_a.ppm", r);
        const Image empty = render(truth, 5.2, {});
        r = compare(sm, cp, cal, base, Z0, empty, 5.2); report("B first layer missing, colour unknown", r);
        CHECK(r.verdict == Verdict::Unsure, "B is flagged as unsure (colour unknown)");
        sm.filament_colours = {"#F5F5F0"};
        r = compare(sm, cp, cal, base, Z0, empty, 5.2); report("B2 first layer missing, white filament", r);
        CHECK(r.verdict == Verdict::Fail, "B2 fails (white filament should be visible)");
        sm.filament_colours = {"#26A69A"};
        r = compare(sm, cp, cal, base, Z0, empty, 5.2); report("B3 first layer missing, default colour", r);
        CHECK(r.verdict == Verdict::Unsure, "B3 is unsure (default colour says nothing)");
        // A torn first layer: the part's G-code is solid, but a 12 x 10 mm patch of it is bare plate.
        {
            Box l{130, 144, 130, 170, 0.2, {}}, r{156, 170, 130, 170, 0.2, {}}, f{144, 156, 130, 145, 0.2, {}}, b{144, 156, 155, 170, 0.2, {}};
            for (Box* q : {&l, &r, &f, &b}) std::memcpy(q->col, white, 3);
            GCodeModel wm = sm;
            wm.filament_colours = {"#F5F5F0"};
            for (Segment& sg : wm.segments) sg.w = 1.0f; // lines 1 mm apart: a solid first layer, as real ones are
            Checkpoint fl = cp; fl.kind = CheckpointKind::FirstLayer;
            auto rt = compare(wm, fl, cal, base, Z0, render(truth, 5.2, {l, r, f, b}), 5.2); report("J torn first layer (white)", rt);
            CHECK(rt.verdict == Verdict::Fail, "J fails (a dark patch inside the part's first layer: %.0f mm2)", rt.first_layer_patch_mm2);
            rt = compare(wm, fl, cal, base, Z0, render(truth, 5.2, {part}), 5.2); report("J2 whole first layer (white)", rt);
            CHECK(rt.verdict == Verdict::Pass && rt.first_layer_patch_mm2 < kPatchOk, "J2 passes (patch %.0f mm2)", rt.first_layer_patch_mm2);
        }
        Box dark = part; std::memcpy(dark.col, plate, 3);
        sm.filament_colours = {"#3A3A3D"};
        const Image darkimg = render(truth, 5.2, {dark});
        r = compare(sm, cp, cal, base, Z0, darkimg, 5.2); report("G plate-coloured first layer", r);
        CHECK(r.verdict != Verdict::Fail, "G does not fail on a filament the colour of the plate");
    }
    {
        Box part{130, 170, 130, 170, 12, {}}; std::memcpy(part.col, teal, 3);
        Box tower{112, 118, 147, 153, 12, {}}; std::memcpy(tower.col, teal, 3); // support tower in the G-code
        GCodeModel sm = make_model({part, tower}, 12);
        Checkpoint cp; cp.kind = CheckpointKind::Progress; cp.percent = 40; cp.layer = sm.layer_first_segment.size() - 1;
        cp.segments_done = sm.segments.size();
        const double Zc = 12 + 5;
        const Image cur = render(truth, Zc, {part, tower});
        write_ppm(data + "/syn_c.ppm", cur);
        auto r = compare(sm, cp, cal, base, Z0, cur, Zc); report("C 40% with support tower", r);
        CHECK(r.verdict == Verdict::Pass, "C passes (supports are in the G-code)");
        Box blob{50, 80, 60, 85, 3, {}}; std::memcpy(blob.col, teal, 3);
        const Image spag = render(truth, Zc, {part, tower, blob});
        write_ppm(data + "/syn_d.ppm", spag);
        r = compare(sm, cp, cal, base, Z0, spag, Zc); report("D spaghetti blob", r);
        CHECK(r.verdict == Verdict::Fail, "D fails (material where the G-code has none)");
        Box moved = part; moved.x0 += 45; moved.x1 += 45; moved.y0 -= 40; moved.y1 -= 40; moved.height = 4;
        const Image mv = render(truth, Zc, {moved, tower});
        r = compare(sm, cp, cal, base, Z0, mv, Zc); report("E part knocked loose, colour unknown", r);
        CHECK(r.verdict != Verdict::Pass, "E is not passed");
        GCodeModel teal_model = sm;
        teal_model.filament_colours = {"#28AAA0"};
        r = compare(teal_model, cp, cal, base, Z0, mv, Zc); report("E2 part knocked loose, filament colour set", r);
        CHECK(r.verdict == Verdict::Fail, "E2 fails (the part is not where the G-code put it)");
        const Image bright = render(truth, Zc, {part, tower}, 1.15, 6.0);
        r = compare(sm, cp, cal, base, Z0, bright, Zc); report("F brighter + noisier", r);
        CHECK(r.verdict == Verdict::Pass, "F passes despite exposure change");
    }

    {
        // White filament. A shadow on the plate (the gantry's or a part's, which moves as the bed drops)
        // darkens it and hides the glitter: white filament cannot do that, so it is not a failure. White
        // spaghetti still is.
        Box part{130, 170, 130, 170, 12, {}}; std::memcpy(part.col, white, 3);
        GCodeModel sm = make_model({part}, 12);
        Checkpoint cp; cp.kind = CheckpointKind::Progress; cp.percent = 60; cp.layer = sm.layer_first_segment.size() - 1;
        cp.segments_done = sm.segments.size();
        const double Zc = 12 + 5;
        auto shade = [&](Image img, double x0, double x1, double y0, double y1, double factor) {
            for (int v = 0; v < img.h; ++v)
                for (int u = 0; u < img.w; ++u) {
                    const Eigen::Vector3d d = truth.ray(u + 0.5, v + 0.5);
                    if (d.z() >= -1e-9)
                        continue;
                    const Eigen::Vector3d q = truth.C + ((-Zc - truth.C.z()) / d.z()) * d;
                    if (q.x() < x0 || q.x() > x1 || q.y() < y0 || q.y() > y1)
                        continue;
                    for (int c = 0; c < 3; ++c) {
                        uint8_t& px = img.rgb[(size_t(v) * img.w + u) * 3 + c];
                        px = uint8_t(std::lround(px * factor));
                    }
                }
            return img;
        };
        const Image shadow = shade(render(truth, Zc, {part}), 20, 70, 40, 110, 0.45);
        write_ppm(data + "/syn_i.ppm", shadow);
        GCodeModel unknown = sm;
        auto r = compare(unknown, cp, cal, base, Z0, shadow, Zc); report("I0 shadow on the plate, colour unknown", r);
        CHECK(r.unexpected_mm2 >= 100, "I0 the shadow reads as material when the filament colour is unknown");
        sm.filament_colours = {"#F5F5F0"};
        r = compare(sm, cp, cal, base, Z0, shadow, Zc); report("I shadow on the plate, white filament", r);
        CHECK(r.verdict == Verdict::Pass && r.light_filament, "I passes (white filament cannot darken the plate)");
        Box blob{50, 80, 60, 85, 3, {}}; std::memcpy(blob.col, white, 3);
        r = compare(sm, cp, cal, base, Z0, render(truth, Zc, {part, blob}), Zc); report("H white spaghetti blob", r);
        CHECK(r.verdict == Verdict::Fail, "H fails (white material where the G-code has none)");
        r = compare(sm, cp, cal, base, Z0, shade(render(truth, Zc, {part, blob}), 20, 70, 40, 110, 0.45), Zc);
        report("H2 white blob inside a shadow", r);
        CHECK(r.verdict == Verdict::Fail, "H2 fails (a blob in a shadow is still brighter than the plate)");
    }

    {
        // A hand in the reference photo: it covers part of the frame around the plate, and the light it bounces
        // brightens a patch of plate, which reads as dark material once the hand is gone. The frame shows the
        // reference no longer matches, so the check stays unsure instead of failing.
        const uint8_t black[3] = {22, 22, 24};
        Box part{130, 170, 130, 170, 12, {}}; std::memcpy(part.col, black, 3);
        GCodeModel sm = make_model({part}, 12);
        sm.filament_colours = {"#161616"};
        Checkpoint cp; cp.kind = CheckpointKind::Progress; cp.percent = 60; cp.layer = sm.layer_first_segment.size() - 1;
        cp.segments_done = sm.segments.size();
        const double Zc = 12 + 5;
        Image lit = base; // the bounced light: plate x 20..70, y 40..110 brighter
        for (int v = 0; v < lit.h; ++v)
            for (int u = 0; u < lit.w; ++u) {
                const Eigen::Vector3d d = truth.ray(u + 0.5, v + 0.5);
                if (d.z() >= -1e-9)
                    continue;
                const Eigen::Vector3d q = truth.C + ((-Z0 - truth.C.z()) / d.z()) * d;
                if (q.x() >= 20 && q.x() <= 70 && q.y() >= 40 && q.y() <= 110)
                    for (int c = 0; c < 3; ++c) {
                        uint8_t& px = lit.rgb[(size_t(v) * lit.w + u) * 3 + c];
                        px = uint8_t(std::min(255L, std::lround(px * 1.6)));
                    }
            }
        const Image cur = render(truth, Zc, {part});
        auto r = compare(sm, cp, cal, lit, Z0, cur, Zc); report("K0 bounced light in the reference, no hand in view", r);
        CHECK(r.verdict == Verdict::Fail && !r.reference_mismatch, "K0 the brightened patch reads as dark material");
        Image hand = lit; // the hand itself: a skin-coloured patch on the frame, top left
        int   off_plate = 0;
        for (int v = 0; v < 120; ++v)
            for (int u = 20; u < 200; ++u) {
                if (truth.ray(u + 0.5, v + 0.5).z() < -1e-9)
                    continue; // only where the plate is not
                ++off_plate;
                uint8_t* px = &hand.rgb[(size_t(v) * hand.w + u) * 3];
                px[0] = 190; px[1] = 150; px[2] = 130;
            }
        r = compare(sm, cp, cal, hand, Z0, cur, Zc); report("K hand in the reference photo", r);
        CHECK(off_plate > 5000 && r.verdict == Verdict::Unsure && r.reference_mismatch,
              "K is unsure (the reference no longer matches the frame around the plate)");
        Box blob{50, 80, 60, 85, 3, {}}; std::memcpy(blob.col, black, 3);
        r = compare(sm, cp, cal, base, Z0, render(truth, Zc, {part, blob}), Zc); report("K2 dark blob, clean reference", r);
        CHECK(r.verdict == Verdict::Fail && !r.reference_mismatch, "K2 fails (dark material where the G-code has none)");
    }

    // ---------- 5. calibration from a test print ----------
    std::printf("5. Camera calibration from a test print of dots\n");
    {
        std::vector<Box> dots;
        for (double y : {25.0, 110.0, 195.0, 280.0})
            for (double x : {25.0, 110.0, 195.0, 280.0}) {
                Box b{x - 6, x + 6, y - 6, y + 6, 0.4, {}};
                std::memcpy(b.col, white, 3);
                dots.push_back(b);
            }
        Box centre{146.5, 158.5, 146.5, 158.5, 0.4, {}};
        std::memcpy(centre.col, white, 3);
        dots.push_back(centre);
        GCodeModel dm = make_model(dots, 0.4);
        // A purge line along the front edge, as start G-code prints one: not a dot.
        for (int k = 0; k < 2; ++k)
            dm.segments.insert(dm.segments.begin() + dm.layer_first_segment[0], {5.f, float(2 + k * 0.5), 150.f, float(2 + k * 0.5), 0.2f, 0.5f});
        for (size_t L = 1; L < dm.layer_first_segment.size(); ++L)
            dm.layer_first_segment[L] += 2;
        const std::vector<PrintedDot> found = find_printed_dots(dm);
        // In the photos, a skirt 3 mm outside the dots (a thin white ring), which must not pull their centres.
        std::vector<Box> printed = dots;
        for (const Box& e : {Box{16, 289, 16, 16.45, 0.2, {}}, Box{16, 289, 288.55, 289, 0.2, {}}, Box{16, 16.45, 16, 289, 0.2, {}},
                             Box{288.55, 289, 16, 289, 0.2, {}}}) {
            printed.push_back(e);
            std::memcpy(printed.back().col, white, 3);
        }
        double worst_c = 0;
        for (const Box& b : dots) {
            double best = 1e9;
            for (const PrintedDot& d : found)
                best = std::min(best, std::hypot(d.x - 0.5 * (b.x0 + b.x1), d.y - 0.5 * (b.y0 + b.y1)));
            worst_c = std::max(worst_c, best);
        }
        CHECK(found.size() == dots.size() && worst_c < 0.5, "found %zu of %zu dots in the G-code (worst centre %.2f mm), purge line left out",
              found.size(), dots.size(), worst_c);
        std::vector<Correspondence> dpts;
        int located = 0, tried = 0;
        for (double Z : {10.0, 60.0, 120.0, 170.0}) {
            const Image before = render(truth, Z, {}), after = render(truth, Z, printed);
            for (size_t k = 0; k < found.size(); ++k) {
                const PrintedDot& d = found[k];
                double u0, v0, u, v, radius, area;
                if (!dot_search(cal, found, k, -Z + d.top, 40, u0, v0, radius, area))
                    continue;
                ++tried;
                if (locate_dot(before, after, u0, v0, radius, area, u, v)) {
                    double ut, vt;
                    tm.project(d.x, d.y, -Z + d.top, ut, vt);
                    if (std::getenv("FC_VERBOSE") && std::hypot(u - ut, v - vt) > 2)
                        std::printf("    Z=%.0f dot (%.0f,%.0f): found (%.1f,%.1f) truth (%.1f,%.1f) predicted (%.1f,%.1f)\n", Z, d.x, d.y, u, v, ut, vt, u0, v0);
                    dpts.push_back({d.x, d.y, -Z + d.top, u, v});
                    ++located;
                }
            }
        }
        CHECK(located >= tried * 8 / 10, "located %d of %d dots in the photos", located, tried);
        CameraModel dcal;
        CHECK(calibrate_camera(dpts, 640, 480, dcal, err), "calibrate from %zu dot points: rms %.2f px (clicked: %.2f px on these points)",
              dpts.size(), dcal.rms_px, reprojection_rms(cal, dpts));
        double dworst = 0;
        for (double Z : {5.0, 40.0, 120.0, 200.0})
            for (double x = 10; x <= 295; x += 20)
                for (double y = 10; y <= 295; y += 20) {
                    double u0, v0, u1, v1;
                    if (!tm.project(x, y, -Z, u0, v0) || u0 < 0 || v0 < 0 || u0 >= 640 || v0 >= 480) continue;
                    if (!dcal.project(x, y, -Z, u1, v1)) { dworst = 1e9; continue; }
                    dworst = std::max(dworst, std::hypot(u1 - u0, v1 - v0));
                }
        CHECK(dworst < worst * 0.5, "worst projection error over the visible bed: %.2f px from dots vs %.2f px from clicks", dworst, worst);
    }

    // ---------- 4. real camera noise ----------
    std::printf("4. Real snapshots a minute apart (same bed height): false-alarm check\n");
    Image a, b;
    if (read_bmp(data + "/cam_a.bmp", a) && read_bmp(data + "/cam_b.bmp", b)) {
        GCodeModel em; em.bed_max_x = 305; em.bed_max_y = 305;
        Checkpoint cp; cp.kind = CheckpointKind::Progress; cp.layer = 0; cp.segments_done = 0;
        auto r = compare(em, cp, cal, a, 35.16, b, 35.16); report("real noise", r);
        CHECK(r.verdict == Verdict::Pass && r.unexpected_mm2 < 100, "no false alarm from camera noise");
    } else {
        std::printf("  (snapshots not found)\n");
    }
    std::printf("\n%s (%d failures)\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
