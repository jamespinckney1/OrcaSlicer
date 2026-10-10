#include "FailureCheck.hpp"

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace Slic3r { namespace GUI { namespace FailureCheck {

namespace {

constexpr const char* kCheckpointCmd = "SHOP_CHECKPOINT";

bool starts_with(const std::string& s, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string ltrim(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

bool is_checkpoint_line(const std::string& line) { return starts_with(ltrim(line), kCheckpointCmd); }

// Value of a G-code word such as X12.5 in a comment-free command line.
bool word(const std::string& cmd, char letter, double& value)
{
    for (size_t i = 0; i < cmd.size(); ++i) {
        if ((cmd[i] == letter || cmd[i] == letter + 32) && (i == 0 || cmd[i - 1] == ' ' || cmd[i - 1] == '\t')) {
            const char* begin = cmd.c_str() + i + 1;
            char*       end   = nullptr;
            const double v    = std::strtod(begin, &end);
            if (end != begin) {
                value = v;
                return true;
            }
        }
    }
    return false;
}

void plan_checkpoints(GCodeModel& m)
{
    m.checkpoints.clear();
    const size_t layers = m.layer_first_segment.size();
    if (layers == 0)
        return;
    int  next = 0;
    auto add  = [&](CheckpointKind kind, int percent, size_t line, size_t done, size_t layer) {
        Checkpoint cp;
        cp.index         = next++;
        cp.kind          = kind;
        cp.percent       = percent;
        cp.line          = line;
        cp.segments_done = done;
        cp.layer         = layer;
        m.checkpoints.push_back(cp);
    };
    add(CheckpointKind::Baseline, 0, m.layer_line[0], m.layer_first_segment[0], 0);

    // A few moves into the first layer: about a fifth of its extrusion length (at least 150 mm when the
    // layer is long enough), never past half of it.
    const size_t s0 = m.layer_first_segment[0];
    const size_t s1 = layers > 1 ? m.layer_first_segment[1] : m.segments.size();
    double       total = 0;
    for (size_t i = s0; i < s1; ++i)
        total += std::hypot(m.segments[i].x1 - m.segments[i].x0, m.segments[i].y1 - m.segments[i].y0);
    if (total > 0) {
        const double target = std::min(std::max(0.2 * total, std::min(150.0, 0.5 * total)), 0.5 * total);
        double       acc    = 0;
        for (size_t i = s0; i < s1; ++i) {
            acc += std::hypot(m.segments[i].x1 - m.segments[i].x0, m.segments[i].y1 - m.segments[i].y0);
            if (acc >= target) {
                size_t end = i + 1; // arcs produce several segments per line
                while (end < s1 && m.segment_line[end] == m.segment_line[i])
                    ++end;
                if (end < s1)
                    add(CheckpointKind::Early, 0, m.segment_line[i] + 1, end, 0);
                break;
            }
        }
    }
    size_t last_layer = 0;
    if (layers >= 2) {
        add(CheckpointKind::FirstLayer, 0, m.layer_line[1], m.layer_first_segment[1], 1);
        last_layer = 1;
    }
    for (int percent : {20, 40, 60, 80}) {
        const size_t idx = size_t(std::lround(percent / 100.0 * double(layers)));
        if (idx < 2 || idx >= layers || idx <= last_layer)
            continue;
        add(CheckpointKind::Progress, percent, m.layer_line[idx], m.layer_first_segment[idx], idx);
        last_layer = idx;
    }
}

} // namespace

const Checkpoint* GCodeModel::checkpoint(int index) const
{
    for (const Checkpoint& cp : checkpoints)
        if (cp.index == index)
            return &cp;
    return nullptr;
}

std::string GCodeModel::first_layer_colour() const
{
    if (first_tool < 0 || size_t(first_tool) >= filament_colours.size())
        return {};
    std::string c = filament_colours[size_t(first_tool)];
    for (char& ch : c)
        ch = char(std::toupper(static_cast<unsigned char>(ch)));
    // Orca's default first filament colour: most likely never set, so it says nothing about the spool.
    if (c.size() < 7 || c[0] != '#' || c.compare(0, 7, "#26A69A") == 0)
        return {};
    return c.substr(0, 7);
}

std::string checkpoint_label(const Checkpoint& cp)
{
    switch (cp.kind) {
    case CheckpointKind::Baseline: return "reference photo";
    case CheckpointKind::Early: return "early first layer";
    case CheckpointKind::FirstLayer: return "first layer done";
    case CheckpointKind::Progress: return std::to_string(cp.percent) + "%";
    }
    return "checkpoint";
}

bool parse_gcode(const std::string& path, GCodeModel& out, std::string& error)
{
    out = GCodeModel();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    // Slicers mark layer changes; without markers, a layer starts whenever extrusion moves up.
    bool has_markers_in_file = false;
    {
        std::string l;
        while (std::getline(in, l))
            if (l.rfind(";LAYER_CHANGE", 0) == 0 || l.rfind("; CHANGE_LAYER", 0) == 0) {
                has_markers_in_file = true;
                break;
            }
        in.clear();
        in.seekg(0);
    }
    bool   abs_xyz = true, abs_e = true;
    double x = 0, y = 0, z = 0, e = 0, width = 0.45;
    const bool have_markers = has_markers_in_file;
    size_t line_no      = 0;
    double fallback_layer_z = -1;
    std::string line;

    auto new_layer = [&](size_t at_line) {
        out.layer_first_segment.push_back(out.segments.size());
        out.layer_line.push_back(at_line);
        out.layer_z.push_back(-1);
    };
    auto add_segment = [&](double ax, double ay, double bx, double by, double sz) {
        if (!have_markers && sz > fallback_layer_z + 1e-4) {
            // No layer markers in this file: start a layer whenever extrusion moves up.
            new_layer(line_no);
            fallback_layer_z = sz;
        }
        if (out.layer_first_segment.empty())
            return; // the start G-code purge before any layer marker is part of the reference photo
        out.segments.push_back({float(ax), float(ay), float(bx), float(by), float(sz), float(width)});
        out.segment_line.push_back(line_no);
        if (out.layer_z.back() < 0)
            out.layer_z.back() = sz;
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (is_checkpoint_line(line))
            continue; // not counted, so line numbers match a stripped copy
        const std::string t = ltrim(line);
        if (!t.empty() && t[0] == ';') {
            const std::string c = t.substr(1);
            if (c == "LAYER_CHANGE" || c == " CHANGE_LAYER") {
                new_layer(line_no);
            } else if (starts_with(c, "WIDTH:")) {
                width = std::atof(c.c_str() + 6);
            } else if (starts_with(c, " LINE_WIDTH:")) {
                width = std::atof(c.c_str() + 12);
            } else if (starts_with(c, " filament_colour = ")) {
                out.filament_colours.clear();
                const std::string v = c.substr(19);
                size_t            pos = 0;
                while (pos <= v.size()) {
                    const size_t semi = v.find(';', pos);
                    out.filament_colours.push_back(v.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
                    if (semi == std::string::npos)
                        break;
                    pos = semi + 1;
                }
            } else if (starts_with(c, " printable_area = ")) {
                double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
                const std::string v = c.substr(18);
                size_t            pos = 0;
                while (pos < v.size()) {
                    // Points are "XxY" pairs; split on 'x' by hand, since strtod would read "0x0" as hex.
                    const size_t comma = v.find(',', pos);
                    const std::string pt = v.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    const size_t sep = pt.find('x');
                    if (sep != std::string::npos) {
                        const double px = std::atof(pt.substr(0, sep).c_str()), py = std::atof(pt.substr(sep + 1).c_str());
                        minx = std::min(minx, px); maxx = std::max(maxx, px);
                        miny = std::min(miny, py); maxy = std::max(maxy, py);
                    }
                    if (comma == std::string::npos)
                        break;
                    pos = comma + 1;
                }
                if (maxx > minx && maxy > miny) {
                    out.bed_min_x = minx; out.bed_max_x = maxx;
                    out.bed_min_y = miny; out.bed_max_y = maxy;
                }
            }
            ++line_no;
            continue;
        }
        std::string cmd = t.substr(0, t.find(';'));
        if (cmd.size() >= 2 && (cmd[0] == 'G' || cmd[0] == 'g')) {
            const int g = std::atoi(cmd.c_str() + 1);
            if (g == 90) abs_xyz = true;
            else if (g == 91) abs_xyz = false;
            else if (g == 92) {
                double v;
                if (word(cmd, 'E', v)) e = v;
                if (word(cmd, 'X', v)) x = v;
                if (word(cmd, 'Y', v)) y = v;
                if (word(cmd, 'Z', v)) z = v;
            } else if (g >= 0 && g <= 3 && (cmd[1] >= '0' && cmd[1] <= '3') && (cmd.size() == 2 || cmd[2] == ' ' || cmd[2] == '\t')) {
                double nx = x, ny = y, nz = z, v;
                if (word(cmd, 'X', v)) nx = abs_xyz ? v : x + v;
                if (word(cmd, 'Y', v)) ny = abs_xyz ? v : y + v;
                if (word(cmd, 'Z', v)) nz = abs_xyz ? v : z + v;
                double de = 0;
                if (word(cmd, 'E', v)) {
                    de = abs_e ? v - e : v;
                    e  = abs_e ? v : e + v;
                }
                const bool extruding = de > 1e-6;
                if (g <= 1) {
                    if (extruding && std::hypot(nx - x, ny - y) > 1e-4)
                        add_segment(x, y, nx, ny, std::max(z, nz));
                } else {
                    double i = 0, j = 0;
                    word(cmd, 'I', i);
                    word(cmd, 'J', j);
                    const double cx = x + i, cy = y + j, r = std::hypot(i, j);
                    if (r > 1e-6) {
                        const double a0 = std::atan2(y - cy, x - cx);
                        double       a1 = std::atan2(ny - cy, nx - cx);
                        if (g == 2) { if (a1 >= a0 - 1e-9) a1 -= 2 * M_PI; }
                        else        { if (a1 <= a0 + 1e-9) a1 += 2 * M_PI; }
                        const int n = std::max(1, int(std::ceil(std::abs(a1 - a0) / (M_PI / 18))));
                        double px = x, py = y;
                        for (int k = 1; k <= n; ++k) {
                            const double a  = a0 + (a1 - a0) * k / n;
                            const double qx = k == n ? nx : cx + r * std::cos(a);
                            const double qy = k == n ? ny : cy + r * std::sin(a);
                            if (extruding)
                                add_segment(px, py, qx, qy, std::max(z, nz));
                            px = qx; py = qy;
                        }
                    }
                }
                x = nx; y = ny; z = nz;
            }
        } else if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit(static_cast<unsigned char>(cmd[1])) && out.segments.empty()) {
            out.first_tool = std::atoi(cmd.c_str() + 1);
        } else if (cmd.size() >= 3 && (cmd[0] == 'M' || cmd[0] == 'm')) {
            const int mcode = std::atoi(cmd.c_str() + 1);
            if (mcode == 82) abs_e = true;
            else if (mcode == 83) abs_e = false;
        }
        ++line_no;
    }
    // Drop layer markers that never extruded anything at the end (e.g. a trailing marker).
    while (!out.layer_first_segment.empty() && out.layer_first_segment.back() >= out.segments.size()) {
        out.layer_first_segment.pop_back();
        out.layer_line.pop_back();
        out.layer_z.pop_back();
    }
    if (out.layer_first_segment.empty()) {
        error = "no printed layers found in the G-code";
        return false;
    }
    plan_checkpoints(out);
    return true;
}

bool write_with_checkpoints(const std::string& in_path, const std::string& out_path, const std::vector<Checkpoint>& checkpoints,
                            int timeout_s, std::string& error)
{
    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
        error = "cannot open " + in_path;
        return false;
    }
    const std::string tmp = out_path + ".shopcheck.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
        std::vector<Checkpoint> sorted = checkpoints;
        std::sort(sorted.begin(), sorted.end(), [](const Checkpoint& a, const Checkpoint& b) { return a.line < b.line; });
        size_t      next = 0, line_no = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (is_checkpoint_line(line))
                continue;
            while (next < sorted.size() && sorted[next].line == line_no) {
                out << kCheckpointCmd << " N=" << sorted[next].index << " TIMEOUT=" << timeout_s << " ; shop failure check: "
                    << checkpoint_label(sorted[next]) << '\n';
                ++next;
            }
            out << line << '\n';
            ++line_no;
        }
        if (!out.good()) {
            error = "write failed for " + tmp;
            return false;
        }
    }
    in.close();
    std::remove(out_path.c_str());
    if (std::rename(tmp.c_str(), out_path.c_str()) != 0) {
        error = "cannot replace " + out_path;
        return false;
    }
    return true;
}

// ---- camera -------------------------------------------------------------------------------------

namespace {

void distort(const CameraModel& c, double& u, double& v)
{
    const double f = c.width / 2.0, cx = c.width / 2.0, cy = c.height / 2.0;
    const double xn = (u - cx) / f, yn = (v - cy) / f;
    const double s  = 1.0 + c.k1 * (xn * xn + yn * yn);
    u = cx + f * xn * s;
    v = cy + f * yn * s;
}

void undistort(int width, int height, double k1, double& u, double& v)
{
    const double f = width / 2.0, cx = width / 2.0, cy = height / 2.0;
    const double xd = (u - cx) / f, yd = (v - cy) / f;
    const double rd = std::hypot(xd, yd);
    if (rd < 1e-12)
        return;
    double ru = rd;
    for (int it = 0; it < 30; ++it)
        ru = rd / (1.0 + k1 * ru * ru);
    const double s = ru / rd;
    u = cx + f * xd * s;
    v = cy + f * yd * s;
}

// Normalised DLT: P (3x4) from 3D-2D pairs, undistorted pixels.
bool dlt(const std::vector<Correspondence>& pts, const std::vector<std::array<double, 2>>& uv, Eigen::Matrix<double, 3, 4>& P,
         double& conditioning)
{
    const size_t n = pts.size();
    Eigen::Vector3d c3 = Eigen::Vector3d::Zero();
    Eigen::Vector2d c2 = Eigen::Vector2d::Zero();
    for (size_t i = 0; i < n; ++i) {
        c3 += Eigen::Vector3d(pts[i].x, pts[i].y, pts[i].h);
        c2 += Eigen::Vector2d(uv[i][0], uv[i][1]);
    }
    c3 /= double(n);
    c2 /= double(n);
    double d3 = 0, d2 = 0;
    for (size_t i = 0; i < n; ++i) {
        d3 += (Eigen::Vector3d(pts[i].x, pts[i].y, pts[i].h) - c3).norm();
        d2 += (Eigen::Vector2d(uv[i][0], uv[i][1]) - c2).norm();
    }
    const double s3 = std::sqrt(3.0) * double(n) / std::max(d3, 1e-9);
    const double s2 = std::sqrt(2.0) * double(n) / std::max(d2, 1e-9);
    Eigen::Matrix4d T3 = Eigen::Matrix4d::Identity();
    T3.block<3, 3>(0, 0) *= s3;
    T3.block<3, 1>(0, 3) = -s3 * c3;
    Eigen::Matrix3d T2 = Eigen::Matrix3d::Identity();
    T2.block<2, 2>(0, 0) *= s2;
    T2.block<2, 1>(0, 2) = -s2 * c2;

    Eigen::MatrixXd A(2 * n, 12);
    for (size_t i = 0; i < n; ++i) {
        const Eigen::Vector4d X = T3 * Eigen::Vector4d(pts[i].x, pts[i].y, pts[i].h, 1.0);
        const Eigen::Vector3d u = T2 * Eigen::Vector3d(uv[i][0], uv[i][1], 1.0);
        A.row(2 * i) << X.transpose(), Eigen::RowVector4d::Zero(), -u.x() * X.transpose();
        A.row(2 * i + 1) << Eigen::RowVector4d::Zero(), X.transpose(), -u.y() * X.transpose();
    }
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
    const auto& sv = svd.singularValues();
    if (sv.size() < 12)
        return false;
    conditioning = sv(10) / std::max(sv(0), 1e-12); // second smallest; ~0 for degenerate (planar) sets
    const Eigen::VectorXd p = svd.matrixV().col(11);
    Eigen::Matrix<double, 3, 4> Pn;
    Pn << p(0), p(1), p(2), p(3), p(4), p(5), p(6), p(7), p(8), p(9), p(10), p(11);
    P = T2.inverse() * Pn * T3;
    // Points in front of the camera must have w > 0.
    int positive = 0;
    for (size_t i = 0; i < n; ++i)
        if (P.row(2).dot(Eigen::Vector4d(pts[i].x, pts[i].y, pts[i].h, 1.0)) > 0)
            ++positive;
    if (positive * 2 < int(n))
        P = -P;
    P /= P.row(2).head<3>().norm();
    return true;
}

} // namespace

bool CameraModel::project(double x, double y, double h, double& u, double& v) const
{
    const double X = P[0] * x + P[1] * y + P[2] * h + P[3];
    const double Y = P[4] * x + P[5] * y + P[6] * h + P[7];
    const double W = P[8] * x + P[9] * y + P[10] * h + P[11];
    if (W <= 1e-9)
        return false;
    u = X / W;
    v = Y / W;
    distort(*this, u, v);
    return true;
}

std::string CameraModel::to_json() const
{
    nlohmann::json j;
    j["width"]  = width;
    j["height"] = height;
    j["P"]      = std::vector<double>(P, P + 12);
    j["k1"]     = k1;
    j["rms_px"] = rms_px;
    j["points"] = points;
    j["park_x"] = park_x;
    j["park_y"] = park_y;
    j["when"]   = when;
    return j.dump(2);
}

bool CameraModel::from_json(const std::string& text, CameraModel& out)
{
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    try {
        CameraModel c;
        c.width  = j.value("width", 640);
        c.height = j.value("height", 480);
        const auto p = j.at("P").get<std::vector<double>>();
        if (p.size() != 12)
            return false;
        std::copy(p.begin(), p.end(), c.P);
        c.k1     = j.value("k1", 0.0);
        c.rms_px = j.value("rms_px", 0.0);
        c.points = j.value("points", 0);
        c.park_x = j.value("park_x", 95.0);
        c.park_y = j.value("park_y", 324.0);
        c.when   = j.value("when", std::string());
        c.valid  = true;
        out      = c;
        return true;
    } catch (...) {
        return false;
    }
}

static bool calibrate_fixed(const std::vector<Correspondence>& pts, int width, int height, CameraModel& out, std::string& error);

bool calibrate_camera(std::vector<Correspondence> pts, int width, int height, CameraModel& out, std::string& error)
{
    int groups = 0;
    for (const auto& p : pts)
        groups = std::max(groups, p.group + 1);
    CameraModel cam;
    if (!calibrate_fixed(pts, width, height, cam, error))
        return false;
    // Alternate: with the camera fixed, move each group's x/y to where its rays meet their bed planes;
    // then recalibrate with the new positions.
    for (int iter = 0; iter < 8 && groups > 0; ++iter) {
        Eigen::Matrix<double, 3, 4> P;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                P(r, c) = cam.P[r * 4 + c];
        std::vector<Eigen::Vector2d> sum(groups, Eigen::Vector2d::Zero());
        std::vector<int>             cnt(groups, 0);
        for (const auto& p : pts) {
            if (p.group < 0)
                continue;
            double u = p.u, v = p.v;
            undistort(width, height, cam.k1, u, v);
            Eigen::Matrix3d H;
            H.col(0) = P.col(0);
            H.col(1) = P.col(1);
            H.col(2) = P.col(2) * p.h + P.col(3);
            if (std::abs(H.determinant()) < 1e-12)
                continue;
            const Eigen::Vector3d q = H.inverse() * Eigen::Vector3d(u, v, 1.0);
            if (std::abs(q.z()) < 1e-12)
                continue;
            sum[p.group] += Eigen::Vector2d(q.x() / q.z(), q.y() / q.z());
            ++cnt[p.group];
        }
        for (auto& p : pts)
            if (p.group >= 0 && cnt[p.group] > 0) {
                p.x = sum[p.group].x() / cnt[p.group];
                p.y = sum[p.group].y() / cnt[p.group];
            }
        CameraModel next;
        if (!calibrate_fixed(pts, width, height, next, error))
            break;
        cam = next;
    }
    out = cam;
    return true;
}

static bool calibrate_fixed(const std::vector<Correspondence>& pts, int width, int height, CameraModel& out, std::string& error)
{
    if (pts.size() < 6) {
        error = "at least 6 points are needed";
        return false;
    }
    double hmin = 1e9, hmax = -1e9;
    for (const auto& p : pts) {
        hmin = std::min(hmin, p.h);
        hmax = std::max(hmax, p.h);
    }
    if (hmax - hmin < 10) {
        error = "the points must come from at least two bed heights";
        return false;
    }
    auto evaluate = [&](double k1, CameraModel& cam) -> bool {
        std::vector<std::array<double, 2>> uv(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            double u = pts[i].u, v = pts[i].v;
            undistort(width, height, k1, u, v);
            uv[i] = {u, v};
        }
        Eigen::Matrix<double, 3, 4> P;
        double cond = 0;
        if (!dlt(pts, uv, P, cond) || cond < 1e-7)
            return false;
        cam        = CameraModel();
        cam.width  = width;
        cam.height = height;
        cam.k1     = k1;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                cam.P[r * 4 + c] = P(r, c);
        double sum = 0;
        for (const auto& p : pts) {
            double u, v;
            if (!cam.project(p.x, p.y, p.h, u, v))
                return false;
            sum += (u - p.u) * (u - p.u) + (v - p.v) * (v - p.v);
        }
        cam.rms_px = std::sqrt(sum / double(pts.size()));
        cam.points = int(pts.size());
        cam.valid  = true;
        return true;
    };
    CameraModel best;
    double      best_k = 0;
    for (double k = -0.40; k <= 0.2001; k += 0.02) {
        CameraModel cam;
        if (evaluate(k, cam) && (!best.valid || cam.rms_px < best.rms_px)) {
            best   = cam;
            best_k = k;
        }
    }
    if (!best.valid) {
        error = "the points do not determine a camera (are they all in one plane?)";
        return false;
    }
    for (double k = best_k - 0.02; k <= best_k + 0.02001; k += 0.002) {
        CameraModel cam;
        if (evaluate(k, cam) && cam.rms_px < best.rms_px)
            best = cam;
    }
    out = best;
    return true;
}

// ---- comparison ---------------------------------------------------------------------------------

namespace {

bool sample(const Image& img, double u, double v, float rgb[3])
{
    if (u < 0 || v < 0 || u > img.w - 1.001 || v > img.h - 1.001)
        return false;
    const int    x0 = int(u), y0 = int(v);
    const double fx = u - x0, fy = v - y0;
    const uint8_t* p00 = &img.rgb[(size_t(y0) * img.w + x0) * 3];
    const uint8_t* p10 = p00 + 3;
    const uint8_t* p01 = p00 + size_t(img.w) * 3;
    const uint8_t* p11 = p01 + 3;
    for (int c = 0; c < 3; ++c)
        rgb[c] = float((1 - fy) * ((1 - fx) * p00[c] + fx * p10[c]) + fy * ((1 - fx) * p01[c] + fx * p11[c]));
    return true;
}

double cross(const std::array<double, 2>& o, const std::array<double, 2>& a, const std::array<double, 2>& b)
{
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

std::vector<std::array<double, 2>> convex_hull(std::vector<std::array<double, 2>> p)
{
    std::sort(p.begin(), p.end());
    if (p.size() < 3)
        return p;
    std::vector<std::array<double, 2>> h(2 * p.size());
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k;
        h[k++] = p[i - 1];
    }
    h.resize(k - 1);
    return h;
}

bool inside_convex(const std::vector<std::array<double, 2>>& hull, double u, double v)
{
    if (hull.size() < 3)
        return false;
    for (size_t i = 0; i < hull.size(); ++i)
        if (cross(hull[i], hull[(i + 1) % hull.size()], {u, v}) < 0)
            return false;
    return true;
}

struct Grid
{
    int    w = 0, h = 0;
    double x0 = 0, y0 = 0, cell = 2.0;
    std::vector<uint8_t> v;
    void   init(int gw, int gh) { w = gw; h = gh; v.assign(size_t(gw) * gh, 0); }
    uint8_t& at(int cx, int cy) { return v[size_t(cy) * w + cx]; }
    uint8_t  get(int cx, int cy) const { return (cx < 0 || cy < 0 || cx >= w || cy >= h) ? 0 : v[size_t(cy) * w + cx]; }
};

// Maps a frame point onto the bed plane as the camera sees it at toolhead height zc: the ray through the
// point hits the bed plane at (bx, by). Material at height z then shows up at (bx, by) in the rectified view.
struct PlaneMapper
{
    Eigen::Matrix<double, 3, 4> M;
    bool ok = false;
    PlaneMapper(const CameraModel& cam, double zc)
    {
        Eigen::Matrix<double, 3, 4> P;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                P(r, c) = cam.P[r * 4 + c];
        Eigen::Matrix3d H;
        H.col(0) = P.col(0);
        H.col(1) = P.col(1);
        H.col(2) = P.col(2) * (-zc) + P.col(3);
        if (std::abs(H.determinant()) < 1e-12)
            return;
        M  = H.inverse() * P;
        ok = true;
    }
    bool map(double x, double y, double h, double& bx, double& by) const
    {
        const Eigen::Vector3d q = M * Eigen::Vector4d(x, y, h, 1.0);
        if (std::abs(q.z()) < 1e-12)
            return false;
        bx = q.x() / q.z();
        by = q.y() / q.z();
        return true;
    }
};

void rasterize(Grid& g, const PlaneMapper& pm, const Segment& s, double h_offset)
{
    double ax, ay, bx, by;
    if (!pm.map(s.x0, s.y0, s.z + h_offset, ax, ay) || !pm.map(s.x1, s.y1, s.z + h_offset, bx, by))
        return;
    const double len   = std::hypot(bx - ax, by - ay);
    const int    steps = std::max(1, int(std::ceil(len / (0.5 * g.cell))));
    const int    rad   = int(std::floor(s.w / (2.0 * g.cell) + 0.25));
    for (int k = 0; k <= steps; ++k) {
        const double t  = double(k) / steps;
        const int    cx = int(std::floor((ax + (bx - ax) * t - g.x0) / g.cell));
        const int    cy = int(std::floor((ay + (by - ay) * t - g.y0) / g.cell));
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                const int xx = cx + dx, yy = cy + dy;
                if (xx >= 0 && yy >= 0 && xx < g.w && yy < g.h)
                    g.at(xx, yy) = 1;
            }
    }
}

Grid dilate(const Grid& g, int r)
{
    Grid out = g;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            if (g.get(x, y))
                continue;
            bool hit = false;
            for (int dy = -r; dy <= r && !hit; ++dy)
                for (int dx = -r; dx <= r && !hit; ++dx)
                    hit = g.get(x + dx, y + dy) != 0;
            if (hit)
                out.at(x, y) = 1;
        }
    return out;
}

Grid erode4(const Grid& g)
{
    Grid out = g;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (g.get(x, y) && !(g.get(x - 1, y) && g.get(x + 1, y) && g.get(x, y - 1) && g.get(x, y + 1)))
                out.at(x, y) = 0;
    return out;
}

double median(std::vector<float> v)
{
    if (v.empty())
        return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

} // namespace

CheckResult compare(const GCodeModel& model, const Checkpoint& cp, const CameraModel& camera, const Image& baseline, double baseline_z,
                    const Image& current, double current_z)
{
    CheckResult res;
    if (!camera.valid || baseline.empty() || current.empty() || !model.has_bed()) {
        res.reasons.push_back("missing camera calibration, reference photo or bed size");
        return res;
    }
    const double cell = res.cell_mm;
    res.grid_x0 = model.bed_min_x;
    res.grid_y0 = model.bed_min_y;
    res.grid_w  = int(std::ceil((model.bed_max_x - model.bed_min_x) / cell));
    res.grid_h  = int(std::ceil((model.bed_max_y - model.bed_min_y) / cell));
    const int W = res.grid_w, H = res.grid_h;
    res.bed_cells = W * H;
    res.cells.assign(size_t(W) * H, CellInvalid);

    // The parked toolhead is fixed in the frame, so it covers the same pixels in both photos; ignore them,
    // and the bed strip under it where its shadow falls.
    std::vector<std::array<double, 2>> head_pts;
    for (double dx : {-34.0, 34.0})
        for (double dy : {-40.0, 16.0})
            for (double hh : {-2.0, 80.0}) {
                double u, v;
                if (camera.project(camera.park_x + dx, camera.park_y + dy, hh, u, v))
                    head_pts.push_back({u, v});
            }
    const auto head = convex_hull(head_pts);
    // Sample both photos on a bed-plane grid (2 x 2 samples per cell).
    std::vector<float> base(size_t(W) * H * 3), cur(size_t(W) * H * 3);
    std::vector<uint8_t> valid(size_t(W) * H, 0);
    double sum_b = 0, sum_c = 0;
    int    n_valid = 0;
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const double x = res.grid_x0 + (gx + 0.5) * cell, y = res.grid_y0 + (gy + 0.5) * cell;
            if (std::abs(x - camera.park_x) < 45 && y > model.bed_max_y - 40)
                continue;
            // The plate's edge is high-contrast, so a few pixels of calibration error would read as change.
            if (x < model.bed_min_x + 4 || x > model.bed_max_x - 4 || y < model.bed_min_y + 4 || y > model.bed_max_y - 4)
                continue;
            float acc_b[3] = {0, 0, 0}, acc_c[3] = {0, 0, 0};
            bool  ok = true;
            for (double ox : {-0.25, 0.25})
                for (double oy : {-0.25, 0.25}) {
                    double ub, vb, uc, vc;
                    float  sb[3], sc[3];
                    if (!camera.project(x + ox * cell, y + oy * cell, -baseline_z, ub, vb) ||
                        !camera.project(x + ox * cell, y + oy * cell, -current_z, uc, vc) || inside_convex(head, ub, vb) ||
                        inside_convex(head, uc, vc) || !sample(baseline, ub, vb, sb) || !sample(current, uc, vc, sc)) {
                        ok = false;
                        break;
                    }
                    for (int c = 0; c < 3; ++c) {
                        acc_b[c] += sb[c] / 4;
                        acc_c[c] += sc[c] / 4;
                    }
                }
            if (!ok)
                continue;
            const size_t i = size_t(gy) * W + gx;
            valid[i]       = 1;
            for (int c = 0; c < 3; ++c) {
                base[i * 3 + c] = acc_b[c];
                cur[i * 3 + c]  = acc_c[c];
            }
            sum_b += acc_b[0] + acc_b[1] + acc_b[2];
            sum_c += acc_c[0] + acc_c[1] + acc_c[2];
            ++n_valid;
        }
    res.valid_cells = n_valid;
    if (n_valid < res.bed_cells * 0.25) {
        res.reasons.push_back("the camera sees too little of the bed for a reliable check");
        return res;
    }
    const double gain = std::clamp(sum_b / std::max(sum_c, 1.0), 0.7, 1.4); // lighting / exposure drift

    // Per-cell difference, smoothed over the 3 x 3 neighbourhood to suppress the plate's sparkle.
    std::vector<float> diff(size_t(W) * H, 0);
    for (size_t i = 0; i < diff.size(); ++i)
        if (valid[i])
            diff[i] = (std::abs(float(cur[i * 3] * gain) - base[i * 3]) + std::abs(float(cur[i * 3 + 1] * gain) - base[i * 3 + 1]) +
                       std::abs(float(cur[i * 3 + 2] * gain) - base[i * 3 + 2])) / 3.0f;
    std::vector<float> sm(diff.size(), 0), all;
    all.reserve(n_valid);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            if (!valid[i])
                continue;
            float s = 0;
            int   n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = gx + dx, yy = gy + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H || !valid[size_t(yy) * W + xx])
                        continue;
                    s += diff[size_t(yy) * W + xx];
                    ++n;
                }
            sm[i] = s / n;
            all.push_back(sm[i]);
        }
    const double med = median(all);
    std::vector<float> dev;
    dev.reserve(all.size());
    for (float v : all)
        dev.push_back(float(std::abs(v - med)));
    const double mad = median(dev) * 1.4826;
    res.threshold    = std::max(18.0, med + 5.0 * mad);

    // Where the G-code says material is, as the camera sees it now.
    PlaneMapper pm(camera, current_z);
    if (!pm.ok) {
        res.reasons.push_back("camera calibration cannot map this bed height");
        return res;
    }
    Grid allowed;
    allowed.init(W, H);
    allowed.x0 = res.grid_x0;
    allowed.y0 = res.grid_y0;
    allowed.cell = cell;
    Grid footprint = allowed;
    const size_t done = std::min(cp.segments_done, model.segments.size());
    // Older layers are sampled every millimetre of height; the last few layers in full.
    const size_t recent_layer = model.layer_first_segment.empty() ? 0 : std::max<size_t>(cp.layer, 1) - 1;
    double       last_z       = -1;
    for (size_t L = 0; L < model.layer_first_segment.size(); ++L) {
        const size_t a = model.layer_first_segment[L];
        if (a >= done)
            break;
        const size_t b    = std::min(done, L + 1 < model.layer_first_segment.size() ? model.layer_first_segment[L + 1] : model.segments.size());
        const double lz   = model.layer_z[L];
        const bool   full = L + 3 >= cp.layer || L == 0 || lz - last_z >= 1.0;
        if (!full)
            continue;
        last_z = lz;
        for (size_t i = a; i < b; ++i)
            rasterize(allowed, pm, model.segments[i], -current_z);
        if (L == recent_layer || (cp.kind == CheckpointKind::Early && L == 0))
            for (size_t i = a; i < b; ++i)
                rasterize(footprint, pm, model.segments[i], -current_z);
    }
    const Grid allowed_wide = dilate(allowed, 4); // calibration is good to a few millimetres
    const Grid core         = erode4(footprint);

    std::vector<uint8_t> changed(size_t(W) * H, 0);
    for (size_t i = 0; i < changed.size(); ++i)
        changed[i] = valid[i] && sm[i] > res.threshold;
    // Candidate cells: changed where the G-code has nothing. Only connected patches of 8+ cells (32 mm2)
    // count; spaghetti and blobs form patches, while the plate's glitter gives isolated specks.
    std::vector<uint8_t> candidate(size_t(W) * H, 0);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            candidate[i]   = changed[i] && !allowed_wide.get(gx, gy);
        }
    std::vector<uint8_t> unexpected_cell(size_t(W) * H, 0);
    {
        std::vector<uint8_t> seen(size_t(W) * H, 0);
        std::vector<size_t>  stack, comp;
        for (size_t start = 0; start < candidate.size(); ++start) {
            if (!candidate[start] || seen[start])
                continue;
            comp.clear();
            stack.assign(1, start);
            seen[start] = 1;
            while (!stack.empty()) {
                const size_t i = stack.back();
                stack.pop_back();
                comp.push_back(i);
                const int cx = int(i % W), cy = int(i / W);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = cx + dx, yy = cy + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                            continue;
                        const size_t j = size_t(yy) * W + xx;
                        if (candidate[j] && !seen[j]) {
                            seen[j] = 1;
                            stack.push_back(j);
                        }
                    }
            }
            if (comp.size() >= 8)
                for (size_t i : comp)
                    unexpected_cell[i] = 1;
        }
    }
    int unexpected = 0, fp_cells = 0, fp_missing = 0;
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            if (!valid[i])
                continue;
            uint8_t state = changed[i] ? CellExpected : CellUnchanged;
            if (unexpected_cell[i]) {
                state = CellUnexpected;
                ++unexpected;
            }
            if (core.get(gx, gy)) {
                ++fp_cells;
                if (!changed[i]) {
                    ++fp_missing;
                    state = CellMissing;
                }
            }
            res.cells[i] = state;
        }
    res.unexpected_mm2   = unexpected * cell * cell;
    res.footprint_cells  = fp_cells;
    res.missing_fraction = fp_cells > 0 ? double(fp_missing) / fp_cells : 0.0;

    bool fail = false, unsure = false;
    if (res.unexpected_mm2 >= 300.0) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "about %.0f cm2 of material where the G-code has none (spaghetti, a blob, or a part that moved)",
                      res.unexpected_mm2 / 100.0);
        res.reasons.push_back(buf);
        fail = true;
    }
    if (fp_cells >= 30 && res.missing_fraction >= 0.6) {
        char buf[200];
        const bool first_layer = cp.kind == CheckpointKind::Early || cp.kind == CheckpointKind::FirstLayer;
        if (first_layer)
            std::snprintf(buf, sizeof(buf), "%.0f%% of the first layer printed so far shows no new material (not sticking, or not extruding)",
                          res.missing_fraction * 100.0);
        else
            std::snprintf(buf, sizeof(buf), "%.0f%% of the latest layer's area shows no material (the part may have come loose)",
                          res.missing_fraction * 100.0);
        res.reasons.push_back(buf);
        // Unchanged pixels only prove something when the filament should look different from the plate:
        // compare the filament colour from the G-code with the plate where the part should be.
        bool visible = false;
        const std::string colour = model.first_layer_colour();
        if (!colour.empty()) {
            const long rgb = std::strtol(colour.c_str() + 1, nullptr, 16);
            const double fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fb = rgb & 0xFF;
            double pr = 0, pg = 0, pb = 0;
            int    n  = 0;
            for (int gy = 0; gy < H; ++gy)
                for (int gx = 0; gx < W; ++gx) {
                    const size_t i = size_t(gy) * W + gx;
                    if (!valid[i] || !core.get(gx, gy))
                        continue;
                    pr += base[i * 3]; pg += base[i * 3 + 1]; pb += base[i * 3 + 2];
                    ++n;
                }
            if (n > 0)
                visible = std::sqrt((fr - pr / n) * (fr - pr / n) + (fg - pg / n) * (fg - pg / n) + (fb - pb / n) * (fb - pb / n)) >= 90.0;
        }
        if (first_layer && visible) {
            fail = true;
        } else {
            unsure = true;
            if (!visible)
                res.reasons.push_back(colour.empty() ? "the job does not say what colour the filament is, so this may be a filament that looks like the plate"
                                                     : "the filament colour is close to the plate's, so the camera may simply not see it");
        }
    }
    res.verdict = fail ? Verdict::Fail : unsure ? Verdict::Unsure : Verdict::Pass;
    if (res.verdict == Verdict::Pass)
        res.reasons.push_back("the bed matches the G-code");
    return res;
}

Image render_expected_topdown(const GCodeModel& model, size_t segments_done, double mm_per_px)
{
    Image img;
    if (!model.has_bed() || mm_per_px <= 0)
        return img;
    img.w = int(std::ceil((model.bed_max_x - model.bed_min_x) / mm_per_px));
    img.h = int(std::ceil((model.bed_max_y - model.bed_min_y) / mm_per_px));
    img.rgb.assign(size_t(img.w) * img.h * 3, 48);
    const size_t done = std::min(segments_done, model.segments.size());
    size_t recent = 0;
    for (size_t L = 0; L < model.layer_first_segment.size(); ++L)
        if (model.layer_first_segment[L] < done)
            recent = model.layer_first_segment[L];
    auto put = [&](double x, double y, const uint8_t c[3]) {
        const int px = int((x - model.bed_min_x) / mm_per_px);
        const int py = img.h - 1 - int((y - model.bed_min_y) / mm_per_px);
        if (px < 0 || py < 0 || px >= img.w || py >= img.h)
            return;
        std::copy(c, c + 3, &img.rgb[(size_t(py) * img.w + px) * 3]);
    };
    const uint8_t old_c[3] = {150, 150, 150}, new_c[3] = {80, 200, 120};
    for (size_t i = 0; i < done; ++i) {
        const Segment& s     = model.segments[i];
        const double   len   = std::hypot(s.x1 - s.x0, s.y1 - s.y0);
        const int      steps = std::max(1, int(std::ceil(len / (0.5 * mm_per_px))));
        for (int k = 0; k <= steps; ++k) {
            const double t = double(k) / steps;
            put(s.x0 + (s.x1 - s.x0) * t, s.y0 + (s.y1 - s.y0) * t, i >= recent ? new_c : old_c);
        }
    }
    return img;
}

}}} // namespace Slic3r::GUI::FailureCheck
