#include "BedMeshAnalysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

bool parse_moonraker_bed_mesh(const std::string& json, const std::string& profile, BedMesh& out, std::string& error)
{
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.contains("result") || !j["result"].contains("status")) {
        error = "unexpected response from the printer";
        return false;
    }
    const auto& st = j["result"]["status"];
    if (!st.contains("bed_mesh") || !st["bed_mesh"].contains("profiles") || !st["bed_mesh"]["profiles"].contains(profile)) {
        error = "the printer has no \"" + profile + "\" bed mesh saved";
        return false;
    }
    const auto& p  = st["bed_mesh"]["profiles"][profile];
    const auto& mp = p.value("mesh_params", nlohmann::json::object());
    out          = BedMesh{};
    out.profile  = profile;
    out.min_x    = mp.value("min_x", 0.0);
    out.max_x    = mp.value("max_x", 0.0);
    out.min_y    = mp.value("min_y", 0.0);
    out.max_y    = mp.value("max_y", 0.0);
    for (const auto& row : p.value("points", nlohmann::json::array())) {
        std::vector<double> r;
        for (const auto& v : row)
            if (v.is_number())
                r.push_back(v.get<double>());
        if (!r.empty())
            out.z.push_back(std::move(r));
    }
    const size_t c = out.cols();
    for (const auto& r : out.z)
        if (r.size() != c) {
            error = "the mesh rows have different lengths";
            return false;
        }
    if (st.contains("save_variables") && st["save_variables"].contains("variables")) {
        const auto& v = st["save_variables"]["variables"];
        if (v.contains("shop_full_mesh_ok")) {
            const auto& ok = v["shop_full_mesh_ok"];
            out.trusted = ok.is_number() ? ok.get<double>() == 1.0 : (ok.is_string() && ok.get<std::string>() == "1");
        }
    }
    if (!out.valid()) {
        error = "the mesh is too small to analyse";
        return false;
    }
    return true;
}

BedMeshStats analyze_bed_mesh(const BedMesh& m)
{
    BedMeshStats s;
    if (!m.valid())
        return s;
    const size_t R = m.rows(), C = m.cols();
    // extremes and mean
    s.zmin = s.zmax = m.z[0][0];
    s.high = s.low = {m.x_at(0), m.y_at(0), m.z[0][0]};
    double sum = 0;
    for (size_t r = 0; r < R; ++r)
        for (size_t c = 0; c < C; ++c) {
            const double z = m.z[r][c];
            sum += z;
            if (z > s.zmax) { s.zmax = z; s.high = {m.x_at(c), m.y_at(r), z}; }
            if (z < s.zmin) { s.zmin = z; s.low = {m.x_at(c), m.y_at(r), z}; }
        }
    s.range = s.zmax - s.zmin;
    s.mean  = sum / double(R * C);

    // least-squares plane z = a + b x + c y (normal equations, 3x3)
    double n = 0, sx = 0, sy = 0, sz = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0;
    for (size_t r = 0; r < R; ++r)
        for (size_t c = 0; c < C; ++c) {
            const double x = m.x_at(c), y = m.y_at(r), z = m.z[r][c];
            n += 1; sx += x; sy += y; sz += z; sxx += x * x; syy += y * y; sxy += x * y; sxz += x * z; syz += y * z;
        }
    double A[3][4] = {{n, sx, sy, sz}, {sx, sxx, sxy, sxz}, {sy, sxy, syy, syz}};
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::abs(A[r][col]) > std::abs(A[piv][col])) piv = r;
        for (int k = 0; k < 4; ++k) std::swap(A[col][k], A[piv][k]);
        if (std::abs(A[col][col]) < 1e-12) return s;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            const double f = A[r][col] / A[col][col];
            for (int k = 0; k < 4; ++k) A[r][k] -= f * A[col][k];
        }
    }
    s.plane_a = A[0][3] / A[0][0];
    s.plane_b = A[1][3] / A[1][1];
    s.plane_c = A[2][3] / A[2][2];
    s.tilt_x  = s.plane_b * (m.max_x - m.min_x);
    s.tilt_y  = s.plane_c * (m.max_y - m.min_y);

    // residual (warp)
    s.warp.assign(R, std::vector<double>(C, 0.0));
    double wmin = 1e9, wmax = -1e9;
    for (size_t r = 0; r < R; ++r)
        for (size_t c = 0; c < C; ++c) {
            const double w = m.z[r][c] - (s.plane_a + s.plane_b * m.x_at(c) + s.plane_c * m.y_at(r));
            s.warp[r][c] = w;
            wmin = std::min(wmin, w);
            wmax = std::max(wmax, w);
        }
    s.warp_range = wmax - wmin;

    // Interior bumps/dips: residual differs from the mean of its 8 neighbours by > 0.12 mm.
    // Edge points have fewer neighbours and naturally roll off, so they are reported as corners instead.
    struct Hit { size_t r, c; double dev; };
    std::vector<Hit> hits;
    for (size_t r = 1; r + 1 < R; ++r)
        for (size_t c = 1; c + 1 < C; ++c) {
            double acc = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc)
                    if (dr || dc) acc += s.warp[r + dr][c + dc];
            const double dev = s.warp[r][c] - acc / 8.0;
            if (std::abs(dev) > 0.12)
                hits.push_back({r, c, dev});
        }
    // Merge neighbouring hits into one spot (keep the strongest).
    std::vector<bool> used(hits.size(), false);
    for (size_t i = 0; i < hits.size(); ++i) {
        if (used[i]) continue;
        Hit best = hits[i];
        used[i] = true;
        for (size_t k = i + 1; k < hits.size(); ++k)
            if (!used[k] && std::abs(int(hits[k].r) - int(hits[i].r)) <= 1 && std::abs(int(hits[k].c) - int(hits[i].c)) <= 1) {
                used[k] = true;
                if (std::abs(hits[k].dev) > std::abs(best.dev)) best = hits[k];
            }
        s.spots.push_back({m.x_at(best.c), m.y_at(best.r), best.dev});
    }
    // Corners (row 0 = minimum Y = front).
    const BedMeshStats::Corner corners[4] = {
        {"front-left", {m.x_at(0), m.y_at(0), m.z[0][0]}, s.warp[0][0]},
        {"front-right", {m.x_at(C - 1), m.y_at(0), m.z[0][C - 1]}, s.warp[0][C - 1]},
        {"back-left", {m.x_at(0), m.y_at(R - 1), m.z[R - 1][0]}, s.warp[R - 1][0]},
        {"back-right", {m.x_at(C - 1), m.y_at(R - 1), m.z[R - 1][C - 1]}, s.warp[R - 1][C - 1]},
    };
    s.worst_corner = corners[0];
    for (const auto& k : corners)
        if (std::abs(k.warp) > std::abs(s.worst_corner.warp)) s.worst_corner = k;
    return s;
}

static std::string mm(double v, bool sign = false)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), sign ? "%+.2f mm" : "%.2f mm", v);
    return buf;
}

std::vector<std::string> bed_mesh_recommendations(const BedMesh& m, const BedMeshStats& s)
{
    std::vector<std::string> out;
    if (!m.valid())
        return out;
    if (!m.trusted)
        out.push_back("This mesh was not built by Rebuild full-bed mesh, so Quick start will not use it. "
                      "Rebuild it to enable quick leveling.");
    for (const auto& p : s.spots) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "A spot sits %s %s its surroundings near X %.0f, Y %.0f. That is often debris under "
                      "the plate: lift the plate, wipe both surfaces and rebuild the mesh.",
                      mm(std::abs(p.z)).c_str(), p.z > 0 ? "above" : "below", p.x, p.y);
        out.push_back(buf);
    }
    if (std::abs(s.worst_corner.warp) > 0.2)
        out.push_back(std::string("The ") + s.worst_corner.name + " corner sits " + mm(std::abs(s.worst_corner.warp)) +
                      (s.worst_corner.warp > 0 ? " above" : " below") + " the rest of the surface once tilt is removed. "
                      "Corners lift or droop when the plate or bed is warped or a mounting point has loosened.");
    if (m.trusted && std::abs(s.tilt_x) > 0.3)
        out.push_back("Left-to-right tilt of " + mm(s.tilt_x, true) + " remains even though this mesh was captured after "
                      "gantry leveling. Check that both Z axes move freely and nothing slipped.");
    if (std::abs(s.tilt_y) > 0.3)
        out.push_back("Front-to-back tilt of " + mm(s.tilt_y, true) + ". Gantry leveling only corrects left-to-right, so "
                      "this comes from the bed itself. The mesh compensates; if it keeps growing, re-level the bed "
                      "following QIDI's procedure.");
    if (s.warp_range > 0.3)
        out.push_back("After removing tilt the surface still varies by " + mm(s.warp_range) + " (warp or bow). Check that "
                      "the plate lies flat, the magnetic sheet is clean and nothing is under it. A warped plate or "
                      "heat bed shows the same shape on every rebuild.");
    if (s.range < 0.25)
        out.insert(out.begin(), "The bed is flat: " + mm(s.range) + " from lowest to highest. No maintenance needed.");
    else if (s.range <= 0.6)
        out.insert(out.begin(), "Moderate variation of " + mm(s.range) + ". The mesh compensates for this; keep an eye on it.");
    else
        out.insert(out.begin(), "Large variation of " + mm(s.range) + ". Mesh compensation works best below about "
                                "0.5 mm, so first layers may still vary across the bed.");
    return out;
}

}} // namespace Slic3r::GUI
