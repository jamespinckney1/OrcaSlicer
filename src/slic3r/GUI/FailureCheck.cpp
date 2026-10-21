#include "FailureCheck.hpp"

#include <Eigen/Dense>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

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
    // About 1 mm up: catches parts that start above the first layer (a body floating over the bed).
    if (layers >= 3) {
        for (size_t idx = 2; idx < layers; ++idx)
            if (m.layer_z[idx] >= m.layer_z[0] + 1.0) {
                if (idx < size_t(std::lround(0.2 * double(layers)))) {
                    add(CheckpointKind::EarlyLayers, 0, m.layer_line[idx], m.layer_first_segment[idx], idx);
                    last_layer = idx;
                }
                break;
            }
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
    // Orca's default first filament colour: most likely never set, so it says nothing about the spool. A clear
    // filament ("#RRGGBB00", fully transparent) does not look like its RGB either.
    if (c.size() < 7 || c[0] != '#' || c.compare(0, 7, "#26A69A") == 0 || (c.size() >= 9 && c.compare(7, 2, "00") == 0))
        return {};
    return c.substr(0, 7);
}

std::string checkpoint_label(const Checkpoint& cp)
{
    switch (cp.kind) {
    case CheckpointKind::Baseline: return "reference photo";
    case CheckpointKind::Early: return "early first layer";
    case CheckpointKind::FirstLayer: return "first layer done";
    case CheckpointKind::EarlyLayers: return "early layers";
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
    std::vector<Checkpoint> written;

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
        if (is_checkpoint_line(line)) {
            // An uploaded job: remember the checkpoint as written (not counted, so line numbers match a
            // stripped copy).
            Checkpoint cp;
            const std::string t   = ltrim(line);
            const size_t      npos = t.find(" N=");
            cp.index         = npos == std::string::npos ? -1 : std::atoi(t.c_str() + npos + 3);
            cp.line          = line_no;
            cp.segments_done = out.segments.size();
            cp.layer         = out.layer_first_segment.size();
            const size_t lab = t.find("shop failure check: ");
            const std::string label = lab == std::string::npos ? std::string() : t.substr(lab + 20);
            if (label == "reference photo") cp.kind = CheckpointKind::Baseline;
            else if (label == "early first layer") { cp.kind = CheckpointKind::Early; cp.layer = 0; }
            else if (label == "first layer done") cp.kind = CheckpointKind::FirstLayer;
            else if (label == "early layers") cp.kind = CheckpointKind::EarlyLayers;
            else { cp.kind = CheckpointKind::Progress; cp.percent = std::atoi(label.c_str()); }
            if (cp.index >= 0)
                written.push_back(cp);
            continue;
        }
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
            } else if (starts_with(c, " filament used [mm] = ")) {
                out.filament_used_mm.clear();
                std::istringstream in(c.substr(22));
                std::string        item;
                while (std::getline(in, item, ','))
                    out.filament_used_mm.push_back(std::atof(item.c_str()));
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
    if (written.empty())
        plan_checkpoints(out);
    else
        out.checkpoints = written;
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
            std::remove(tmp.c_str());
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
            out.close();
            std::remove(tmp.c_str());
            return false;
        }
    }
    in.close();
    // Replace in one step: removing out_path first would lose the G-code when in_path == out_path and the rename fails.
    boost::system::error_code ec;
    boost::filesystem::rename(tmp, out_path, ec);
    if (ec) {
        error = "cannot replace " + out_path;
        std::remove(tmp.c_str());
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
    if (!frame_lines.empty()) {
        nlohmann::json fl = nlohmann::json::array();
        for (const auto& l : frame_lines)
            fl.push_back({l[0], l[1], l[2], l[3]});
        j["frame_lines"] = fl;
    }
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
        c.park_x = j.value("park_x", kShopParkX);
        c.park_y = j.value("park_y", kShopParkY);
        c.when   = j.value("when", std::string());
        if (j.contains("frame_lines") && j["frame_lines"].is_array())
            for (const auto& l : j["frame_lines"])
                if (l.is_array() && l.size() == 4)
                    c.frame_lines.push_back({l[0].get<double>(), l[1].get<double>(), l[2].get<double>(), l[3].get<double>()});
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

// Adds a segment's extruded area (mm2) to the cells it crosses, so cov / cell^2 is how much of each cell
// the G-code covers.
void accumulate_coverage(std::vector<float>& cov, const Grid& g, const PlaneMapper& pm, const Segment& s, double h_offset)
{
    double ax, ay, bx, by;
    if (!pm.map(s.x0, s.y0, s.z + h_offset, ax, ay) || !pm.map(s.x1, s.y1, s.z + h_offset, bx, by))
        return;
    const double len   = std::hypot(bx - ax, by - ay);
    const int    steps = std::max(1, int(std::ceil(len / (0.25 * g.cell))));
    const float  add   = float(len / steps * s.w);
    for (int k = 0; k < steps; ++k) {
        const double t  = (k + 0.5) / steps;
        const int    cx = int(std::floor((ax + (bx - ax) * t - g.x0) / g.cell));
        const int    cy = int(std::floor((ay + (by - ay) * t - g.y0) / g.cell));
        if (cx >= 0 && cy >= 0 && cx < g.w && cy < g.h)
            cov[size_t(cy) * g.w + cx] += add;
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

// First-layer checks fail when a part's solid first layer is dark (bare or shadowed plate) in this share of
// its cells, or in one patch of this many mm2 well inside the part (see compare()). On the shop's saved jobs a
// good first layer reached 10% and 8 mm2; a peeled and folded one 392 mm2.
constexpr double kFirstLayerHoles    = 0.25;
constexpr double kFirstLayerPatchMm2 = 60.0;

double median(std::vector<float> v)
{
    if (v.empty())
        return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// The reference no longer shows the scene when the frame around the plate changed in one solid patch of at
// least this share of it (a hand or an object in view; the light it blocked or bounced changes the plate too).
// On the shop's saved jobs a hand in the reference gave 6%; good jobs stayed at 0.5%, and 2.2% when white parts
// made the camera re-expose a lot.
constexpr double kSceneChange = 0.04;

// Share of the frame around the plate (walls, gantry, frame: nothing printed can reach it) that changed between
// the photos, as the largest solid patch of 8 x 8 pixel blocks. The plate with a margin and the parked toolhead
// are left out; the exposure is matched with a tone curve, as on the plate.
double scene_change(const GCodeModel& model, const CameraModel& camera, const Image& a, double za, const Image& b, double zb)
{
    const int W = a.w, H = a.h;
    std::vector<uint8_t> keep(size_t(W) * H, 1);
    {
        // The plate's outline 20 mm out, traced along each edge (the lens bends straight edges), at both bed heights.
        std::vector<std::array<double, 2>> pts;
        const double x0 = model.bed_min_x - 20, y0 = model.bed_min_y - 20, x1 = model.bed_max_x + 20, y1 = model.bed_max_y + 20;
        for (double z : {za, zb})
            for (int k = 0; k <= 20; ++k) {
                const double t = k / 20.0;
                for (const auto& p : {std::array<double, 2>{x0 + (x1 - x0) * t, y0}, std::array<double, 2>{x1, y0 + (y1 - y0) * t},
                                      std::array<double, 2>{x0 + (x1 - x0) * t, y1}, std::array<double, 2>{x0, y0 + (y1 - y0) * t}}) {
                    double u, v;
                    if (camera.project(p[0], p[1], -z, u, v))
                        pts.push_back({u, v});
                }
            }
        const auto plate = convex_hull(pts);
        double     hu0 = 1e9, hu1 = -1e9, hv0 = 1e9, hv1 = -1e9;
        for (double dx : {-40.0, 40.0})
            for (double dy : {-46.0, 22.0})
                for (double hh : {-2.0, 90.0}) {
                    double u, v;
                    if (camera.project(camera.park_x + dx, camera.park_y + dy, hh, u, v)) {
                        hu0 = std::min(hu0, u - 10); hu1 = std::max(hu1, u + 10);
                        hv0 = std::min(hv0, v - 10); hv1 = std::max(hv1, v + 10);
                    }
                }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if ((x >= hu0 && x <= hu1 && y >= hv0 && y <= hv1) || inside_convex(plate, x, y))
                    keep[size_t(y) * W + x] = 0;
    }
    constexpr int B = 8;
    const int     BW = W / B, BH = H / B;
    std::vector<float> la(size_t(BW) * BH, -1), lb(size_t(BW) * BH, -1);
    for (int by = 0; by < BH; ++by)
        for (int bx = 0; bx < BW; ++bx) {
            double sa = 0, sb = 0;
            bool   full = true;
            for (int y = by * B; y < by * B + B && full; ++y)
                for (int x = bx * B; x < bx * B + B; ++x) {
                    const size_t i = size_t(y) * W + x;
                    if (!keep[i]) {
                        full = false;
                        break;
                    }
                    sa += (a.rgb[i * 3] + a.rgb[i * 3 + 1] + a.rgb[i * 3 + 2]) / 3.0;
                    sb += (b.rgb[i * 3] + b.rgb[i * 3 + 1] + b.rgb[i * 3 + 2]) / 3.0;
                }
            if (full) {
                la[size_t(by) * BW + bx] = float(sa / (B * B));
                lb[size_t(by) * BW + bx] = float(sb / (B * B));
            }
        }
    // Tone curve: for each brightness band of the second photo, the median brightness the same blocks had in the first.
    std::array<std::vector<float>, 16> bands;
    int                                n = 0;
    for (size_t i = 0; i < la.size(); ++i)
        if (la[i] >= 0) {
            bands[std::min(15, int(lb[i] / 16))].push_back(la[i]);
            ++n;
        }
    if (n < 200)
        return 0.0;
    std::array<double, 16> curve{};
    std::array<bool, 16>   have{};
    for (int k = 0; k < 16; ++k)
        if (bands[k].size() >= 10) {
            curve[k] = median(bands[k]);
            have[k]  = true;
        }
    for (int k = 0; k < 16; ++k)
        if (!have[k]) {
            int lo = k - 1, hi = k + 1;
            while (lo >= 0 && !have[lo]) --lo;
            while (hi < 16 && !have[hi]) ++hi;
            curve[k] = lo >= 0 && hi < 16 ? curve[lo] + (curve[hi] - curve[lo]) * (k - lo) / double(hi - lo)
                     : lo >= 0            ? curve[lo]
                     : hi < 16            ? curve[hi]
                                          : k * 16.0 + 8.0;
        }
    for (int k = 1; k < 16; ++k)
        curve[k] = std::max(curve[k], curve[k - 1]);
    auto matched = [&](double l) {
        const double f = std::clamp(l / 16.0 - 0.5, 0.0, 15.0);
        const int    k = int(f);
        return k < 15 ? curve[k] * (1 - (f - k)) + curve[k + 1] * (f - k) : curve[15];
    };
    std::vector<uint8_t> changed(la.size(), 0);
    for (size_t i = 0; i < la.size(); ++i)
        if (la[i] >= 0) {
            const double m = matched(lb[i]), d = std::abs(m - la[i]);
            changed[i]     = d > 25 && d > 0.25 * std::max<double>(la[i], m);
        }
    // Solid change only: a block whose eight neighbours changed too. Reflections that move when the exposure
    // changes give scattered blocks; a hand gives a solid patch.
    std::vector<uint8_t> solid(changed.size(), 0);
    for (int y = 1; y + 1 < BH; ++y)
        for (int x = 1; x + 1 < BW; ++x) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (!changed[size_t(y + dy) * BW + x + dx]) {
                        all = false;
                        break;
                    }
            solid[size_t(y) * BW + x] = all;
        }
    std::vector<uint8_t> seen(solid.size(), 0);
    int                  best = 0;
    for (size_t s = 0; s < solid.size(); ++s) {
        if (!solid[s] || seen[s])
            continue;
        std::vector<size_t> stack{s};
        seen[s] = 1;
        int k   = 0;
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            ++k;
            const int x = int(i % BW), y = int(i / BW);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= BW || yy >= BH)
                        continue;
                    const size_t j = size_t(yy) * BW + xx;
                    if (solid[j] && !seen[j]) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
        }
        best = std::max(best, k);
    }
    return double(best) / n;
}

} // namespace

CheckResult compare(const GCodeModel& model, const Checkpoint& cp, const CameraModel& camera, const Image& baseline, double baseline_z,
                    const Image& current, double current_z, const Alignment& prior, size_t baseline_segments)
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
    // The plate is glossy enough to mirror the lit toolhead: its reflection lies where the head's mirror image
    // below the plate is seen, which moves forward as the bed drops (a 70 x 40 mm smear in front of the park
    // position at 27 mm). Each photo has its own, at its own bed height.
    auto head_reflection = [&](double bed_z) {
        std::vector<std::array<double, 2>> pts;
        for (double dx : {-40.0, 40.0})
            for (double dy : {-46.0, 22.0})
                for (double hh : {-2.0, 20.0}) { // the lit shroud around the nozzle; the housing above barely shows
                    double u, v;
                    if (camera.project(camera.park_x + dx, camera.park_y + dy, -2.0 * bed_z - hh, u, v))
                        pts.push_back({u, v});
                }
        return convex_hull(pts);
    };
    const auto reflection_base = head_reflection(baseline_z), reflection_cur = head_reflection(current_z);
    // Rod reflections: near a rod's image line, below its top, and within reach of the plate edge (the
    // reflection starts at the edge and reaches further in as the bed drops).
    auto on_rod_line = [&](double u, double v) {
        for (const auto& l : camera.frame_lines) {
            if (v < l[1])
                continue;
            const double du = l[2] - l[0], dv = l[3] - l[1], len = std::hypot(du, dv);
            if (len > 0 && std::abs(du * (l[1] - v) - (l[0] - u) * dv) / len < 8.0)
                return true;
        }
        return false;
    };
    const double rod_reach = 10.0 + 1.2 * std::max(baseline_z, current_z);
    // Sample both photos on a bed-plane grid (2 x 2 samples per cell).
    std::vector<float> base(size_t(W) * H * 3), cur(size_t(W) * H * 3);
    std::vector<uint8_t> valid(size_t(W) * H, 0);
    std::vector<double>  tex_b1(size_t(W) * H, 0), tex_b2(size_t(W) * H, 0), tex_c1(size_t(W) * H, 0), tex_c2(size_t(W) * H, 0);
    int    n_valid = 0;
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const double x = res.grid_x0 + (gx + 0.5) * cell, y = res.grid_y0 + (gy + 0.5) * cell;
            if (std::abs(x - camera.park_x) < 45 && y > model.bed_max_y - 40)
                continue;
            // The plate's edge is high-contrast, and calibration error grows as the bed drops, so stay 10 mm clear.
            if (x < model.bed_min_x + 10 || x > model.bed_max_x - 10 || y < model.bed_min_y + 10 || y > model.bed_max_y - 10)
                continue;
            float acc_b[3] = {0, 0, 0}, acc_c[3] = {0, 0, 0};
            bool  ok = true;
            const double edge_dist = std::min({x - model.bed_min_x, model.bed_max_x - x, y - model.bed_min_y, model.bed_max_y - y});
            const bool   rod_zone  = !camera.frame_lines.empty() && edge_dist < rod_reach;
            for (double ox : {-0.25, 0.25})
                for (double oy : {-0.25, 0.25}) {
                    double ub, vb, uc, vc;
                    float  sb[3], sc[3];
                    if (!camera.project(x + ox * cell, y + oy * cell, -baseline_z, ub, vb) ||
                        !camera.project(x + ox * cell, y + oy * cell, -current_z, uc, vc) || inside_convex(head, ub, vb) ||
                        inside_convex(head, uc, vc) || inside_convex(reflection_base, ub, vb) || inside_convex(reflection_cur, uc, vc) ||
                        (rod_zone && (on_rod_line(ub, vb) || on_rod_line(uc, vc))) || !sample(baseline, ub, vb, sb) ||
                        !sample(current, uc, vc, sc)) {
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
            // Texture: brightness samples on a 4 x 4 grid inside the cell, for both photos. The plate's
            // glitter varies a lot from pixel to pixel; printed plastic is smooth even when it is a
            // similar colour to the plate.
            double tb1 = 0, tb2 = 0, tc1 = 0, tc2 = 0;
            for (double ox : {-0.375, -0.125, 0.125, 0.375})
                for (double oy : {-0.375, -0.125, 0.125, 0.375}) {
                    double ub, vb, uc, vc;
                    float  sb[3], sc[3];
                    if (!camera.project(x + ox * cell, y + oy * cell, -baseline_z, ub, vb) ||
                        !camera.project(x + ox * cell, y + oy * cell, -current_z, uc, vc) || !sample(baseline, ub, vb, sb) ||
                        !sample(current, uc, vc, sc)) {
                        ok = false;
                        break;
                    }
                    const double lb = (sb[0] + sb[1] + sb[2]) / 3, lc = (sc[0] + sc[1] + sc[2]) / 3;
                    tb1 += lb; tb2 += lb * lb; tc1 += lc; tc2 += lc * lc;
                }
            if (!ok)
                continue;
            const size_t i = size_t(gy) * W + gx;
            valid[i]       = 1;
            tex_b1[i] = tb1; tex_b2[i] = tb2; tex_c1[i] = tc1; tex_c2[i] = tc2;
            for (int c = 0; c < 3; ++c) {
                base[i * 3 + c] = acc_b[c];
                cur[i * 3 + c]  = acc_c[c];
            }
            ++n_valid;
        }
    res.valid_cells = n_valid;
    if (n_valid < res.bed_cells * 0.25) {
        res.reasons.push_back("the camera sees too little of the bed for a reliable check");
        return res;
    }
    res.scene_change       = scene_change(model, camera, baseline, baseline_z, current, current_z);
    res.reference_mismatch = res.scene_change >= kSceneChange;
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
    std::vector<float> coverage(size_t(W) * H, 0.f);
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
            for (size_t i = a; i < b; ++i) {
                rasterize(footprint, pm, model.segments[i], -current_z);
                accumulate_coverage(coverage, footprint, pm, model.segments[i], -current_z);
            }
    }
    // Material the reference already shows (a reference taken at a later checkpoint): it cannot change any more.
    Grid in_base = allowed;
    std::fill(in_base.v.begin(), in_base.v.end(), 0);
    for (size_t i = 0; i < std::min(baseline_segments, done); ++i)
        rasterize(in_base, pm, model.segments[i], -current_z);
    // Cells the latest layer fills (not sparse support base or an outline only): where a first-layer hole means
    // something.
    Grid solid = footprint;
    for (size_t i = 0; i < solid.v.size(); ++i)
        solid.v[i] = solid.v[i] && coverage[i] >= 0.6f * float(cell * cell) && !in_base.v[i];
    // Exposure drift: the camera re-exposes when bright parts fill the view, which shifts the whole
    // plate. Measure it only on plate well away (16 mm) from anything the G-code prints, as the median
    // brightness ratio, so neither the parts nor a failure can skew it.
    const Grid background_excl = dilate(allowed, 8);
    std::vector<uint8_t> background(size_t(W) * H, 0);
    std::vector<float> ratios;
    std::array<std::vector<float>, 16> curve_samples;
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            if (!valid[i] || background_excl.get(gx, gy))
                continue;
            background[i]   = 1;
            const double lb = (base[i * 3] + base[i * 3 + 1] + base[i * 3 + 2]) / 3.0;
            const double lc = (cur[i * 3] + cur[i * 3 + 1] + cur[i * 3 + 2]) / 3.0;
            if (lc > 8.0)
                ratios.push_back(float(std::clamp(lb / lc, 0.4, 2.5)));
            curve_samples[std::min(15, int(lc / 16.0))].push_back(float(lb));
        }
    const double gain = ratios.size() >= 50 ? std::clamp(median(ratios), 0.5, 2.0) : 1.0;
    // The camera's re-exposure is not a plain scale (dark plate and bright glitter respond differently),
    // so fit a tone curve: for each brightness band of the current photo, the median brightness the same
    // plate had in the reference. It is learnt from the empty plate as a whole and applied by brightness,
    // never by position, so a failure cannot teach it to hide itself.
    std::array<double, 16> curve{};
    std::array<bool, 16>   have{};
    for (int b = 0; b < 16; ++b)
        if (curve_samples[b].size() >= 20) {
            curve[b] = median(curve_samples[b]);
            have[b]  = true;
        }
    for (int b = 0; b < 16; ++b)
        if (!have[b]) {
            int lo = b - 1, hi = b + 1;
            while (lo >= 0 && !have[lo]) --lo;
            while (hi < 16 && !have[hi]) ++hi;
            const double centre = b * 16.0 + 8.0;
            if (lo >= 0 && hi < 16)
                curve[b] = curve[lo] + (curve[hi] - curve[lo]) * double(b - lo) / double(hi - lo);
            else if (lo >= 0)
                curve[b] = curve[lo] * centre / (lo * 16.0 + 8.0);
            else if (hi < 16)
                curve[b] = curve[hi] * centre / (hi * 16.0 + 8.0);
            else
                curve[b] = centre * gain;
        }
    for (int b = 1; b < 16; ++b) // keep it monotonic
        curve[b] = std::max(curve[b], curve[b - 1]);
    auto tone_gain = [&](double lc) {
        const double f = std::clamp(lc / 16.0 - 0.5, 0.0, 15.0);
        const int    b = int(f);
        const double t = f - b;
        const double v = b < 15 ? curve[b] * (1 - t) + curve[b + 1] * t : curve[15];
        return std::clamp(v / std::max(lc, 4.0), 0.4, 2.5);
    };
    std::vector<float> cell_gain(size_t(W) * H, float(gain));
    for (size_t i = 0; i < cell_gain.size(); ++i)
        if (valid[i])
            cell_gain[i] = float(tone_gain((cur[i * 3] + cur[i * 3 + 1] + cur[i * 3 + 2]) / 3.0));

    // Per-cell difference, smoothed over the 3 x 3 neighbourhood to suppress the plate's sparkle.
    std::vector<float> diff(size_t(W) * H, 0), ref_texture(size_t(W) * H, 0);
    for (size_t i = 0; i < diff.size(); ++i)
        if (valid[i])
            diff[i] = (std::abs(cur[i * 3] * cell_gain[i] - base[i * 3]) + std::abs(cur[i * 3 + 1] * cell_gain[i] - base[i * 3 + 1]) +
                       std::abs(cur[i * 3 + 2] * cell_gain[i] - base[i * 3 + 2])) / 3.0f;
    // Texture change: brightness spread over the 3 x 3 cell neighbourhood (6 mm), reference vs now.
    // The camera also changes contrast when it re-exposes, which changes how the glitter looks, so
    // texture only counts while the exposure barely moved (dark filament, which does not make the camera
    // re-expose and barely changes colour on the dark plate). Bright filament stands out by colour.
    const double exposure_shift = std::abs(std::log(gain));
    const double texture_weight = 1.5 * std::clamp(1.0 - (exposure_shift - 0.10) / 0.15, 0.0, 1.0);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            if (!valid[i])
                continue;
            double b1 = 0, b2 = 0, c1 = 0, c2 = 0, n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = gx + dx, yy = gy + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H || !valid[size_t(yy) * W + xx])
                        continue;
                    const size_t j = size_t(yy) * W + xx;
                    b1 += tex_b1[j]; b2 += tex_b2[j]; c1 += tex_c1[j]; c2 += tex_c2[j];
                    n += 16;
                }
            const double sb = std::sqrt(std::max(0.0, b2 / n - (b1 / n) * (b1 / n)));
            const double sc = std::sqrt(std::max(0.0, c2 / n - (c1 / n) * (c1 / n))) * cell_gain[i];
            diff[i]         = std::max(diff[i], float(texture_weight * std::abs(sc - sb)));
            ref_texture[i]  = float(sb);
        }
    // Which way the brightness went (after the tone curve), smoothed like the difference below.
    std::vector<float> dlum(diff.size(), 0);
    for (size_t i = 0; i < dlum.size(); ++i)
        if (valid[i])
            dlum[i] = ((cur[i * 3] + cur[i * 3 + 1] + cur[i * 3 + 2]) * cell_gain[i] - (base[i * 3] + base[i * 3 + 1] + base[i * 3 + 2])) / 3.0f;
    std::vector<float> sm(diff.size(), 0), sm_dlum(diff.size(), 0), all;
    all.reserve(n_valid);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            if (!valid[i])
                continue;
            float s = 0, sl = 0;
            int   n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = gx + dx, yy = gy + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H || !valid[size_t(yy) * W + xx])
                        continue;
                    s += diff[size_t(yy) * W + xx];
                    sl += dlum[size_t(yy) * W + xx];
                    ++n;
                }
            sm[i]      = s / n;
            sm_dlum[i] = sl / n;
            if (background[i])
                all.push_back(sm[i]); // noise level measured on the empty plate only
        }
    if (all.size() < 50)
        for (size_t i = 0; i < sm.size(); ++i)
            if (valid[i])
                all.push_back(sm[i]);
    // Noise differs across the plate: where the glitter catches the light it sparkles differently in
    // every photo. The reference photo (which cannot contain a failure) shows where: the threshold rises
    // with the reference's local texture there, never below the whole plate's noise level.
    auto robust_level = [](std::vector<float> v) {
        const double m = median(v);
        for (float& x : v)
            x = float(std::abs(x - m));
        return m + 5.0 * 1.4826 * median(v);
    };
    res.threshold = std::max(18.0, robust_level(all));
    constexpr double texture_k = 1.5;
    std::vector<uint8_t> changed(size_t(W) * H, 0);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i  = size_t(gy) * W + gx;
            changed[i]      = valid[i] && sm[i] > std::max(res.threshold, texture_k * ref_texture[i]);
        }

    // Line each part up with what changed. Parts are the connected groups of G-code material (a part and
    // its supports and brim count as one); each gets the whole-cell offset with the most overlap,
    // preferring small moves, within 8 cells (16 mm), or 3 cells of the nearest first-layer offset,
    // and on the first layer possibly a scale (below).
    {
        const Grid groups_mask = dilate(allowed, 2);
        std::vector<int> label(size_t(W) * H, -1);
        std::vector<std::vector<size_t>> groups;
        for (size_t start = 0; start < label.size(); ++start) {
            if (!groups_mask.v[start] || label[start] >= 0)
                continue;
            const int id = int(groups.size());
            groups.emplace_back();
            std::vector<size_t> stack{start};
            label[start] = id;
            while (!stack.empty()) {
                const size_t i = stack.back();
                stack.pop_back();
                if (allowed.v[i])
                    groups[id].push_back(i);
                const int cx = int(i % W), cy = int(i / W);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = cx + dx, yy = cy + dy;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                            continue;
                        const size_t j = size_t(yy) * W + xx;
                        if (groups_mask.v[j] && label[j] < 0) {
                            label[j] = id;
                            stack.push_back(j);
                        }
                    }
            }
        }
        Grid new_allowed = allowed, new_footprint = footprint, new_solid = solid;
        std::fill(new_solid.v.begin(), new_solid.v.end(), 0);
        std::fill(new_allowed.v.begin(), new_allowed.v.end(), 0);
        std::fill(new_footprint.v.begin(), new_footprint.v.end(), 0);
        for (size_t g = 0; g < groups.size(); ++g) {
            const auto& cells_g = groups[g];
            if (cells_g.empty())
                continue;
            double mx = 0, my = 0;
            for (size_t i : cells_g) {
                mx += res.grid_x0 + (double(i % W) + 0.5) * cell;
                my += res.grid_y0 + (double(i / W) + 0.5) * cell;
            }
            mx /= double(cells_g.size());
            my /= double(cells_g.size());
            int    cx0 = 0, cy0 = 0, win = 8;
            double scale0 = 1.0;
            if (prior.valid()) {
                const Alignment::Anchor* nearest = nullptr;
                double                   best_d  = 1e18;
                for (const auto& an : prior.anchors) {
                    const double d = std::hypot(an.x - mx, an.y - my);
                    if (d < best_d) {
                        best_d  = d;
                        nearest = &an;
                    }
                }
                cx0    = int(std::lround(nearest->dx / cell));
                cy0    = int(std::lround(nearest->dy / cell));
                scale0 = nearest->s;
                win    = 3;
            }
            // The calibration's error also varies across a part, so a big part can look up to ~10% larger or
            // smaller than projected, which no shift absorbs. On the first layer each part of 40 mm or more
            // may also be scaled about its bounding-box centre (the walls come first, so that is the part's
            // centre); later checkpoints keep the first layer's scale.
            int gx0 = W, gy0 = H, gx1 = -1, gy1 = -1;
            for (size_t i : cells_g) {
                gx0 = std::min(gx0, int(i % W)); gx1 = std::max(gx1, int(i % W));
                gy0 = std::min(gy0, int(i / W)); gy1 = std::max(gy1, int(i / W));
            }
            const double pcx = 0.5 * (gx0 + gx1), pcy = 0.5 * (gy0 + gy1);
            auto scaled = [&](double sc, const std::vector<uint8_t>& src) {
                std::vector<size_t> out;
                const int x0 = std::max(0, int(std::floor(pcx + (gx0 - pcx) * sc)) - 1), x1 = std::min(W - 1, int(std::ceil(pcx + (gx1 - pcx) * sc)) + 1);
                const int y0 = std::max(0, int(std::floor(pcy + (gy0 - pcy) * sc)) - 1), y1 = std::min(H - 1, int(std::ceil(pcy + (gy1 - pcy) * sc)) + 1);
                for (int ty = y0; ty <= y1; ++ty)
                    for (int tx = x0; tx <= x1; ++tx) {
                        const int sx = int(std::lround(pcx + (tx - pcx) / sc)), sy = int(std::lround(pcy + (ty - pcy) / sc));
                        if (sx >= 0 && sy >= 0 && sx < W && sy < H && label[size_t(sy) * W + sx] == int(g) && src[size_t(sy) * W + sx])
                            out.push_back(size_t(ty) * W + tx);
                    }
                return out;
            };
            std::vector<double> scales{scale0};
            if (!prior.valid() && std::max(gx1 - gx0, gy1 - gy0) * cell >= 40.0)
                scales = {1.0, 0.96, 1.04, 0.92, 1.08, 0.88, 1.12};
            int    bx = cx0, by = cy0;
            double bs = scales.front(), best_fit = -1e18, fit_unscaled = -1e18;
            std::vector<size_t> best_cells;
            for (const double sc : scales) {
                const std::vector<size_t> cells_s = sc == 1.0 ? cells_g : scaled(sc, allowed.v);
                auto overlap = [&](int sx, int sy) {
                    int n = 0;
                    for (size_t i : cells_s) {
                        const int xx = int(i % W) + sx, yy = int(i / W) + sy;
                        if (xx >= 0 && yy >= 0 && xx < W && yy < H && changed[size_t(yy) * W + xx])
                            ++n;
                    }
                    return n;
                };
                int          sbx = cx0, sby = cy0;
                const double base_score = overlap(cx0, cy0);
                double       best       = base_score;
                for (int sy = cy0 - win; sy <= cy0 + win; ++sy)
                    for (int sx = cx0 - win; sx <= cx0 + win; ++sx) {
                        const double score = overlap(sx, sy) - 0.5 * (std::abs(sx - cx0) + std::abs(sy - cy0));
                        if (score > best) {
                            best = score;
                            sbx  = sx;
                            sby  = sy;
                        }
                    }
                if (best < base_score * 1.05 + 4) { // keep the starting offset unless moving clearly helps
                    sbx = cx0;
                    sby = cy0;
                }
                // Across scales the sizes differ: score matched cells against expected cells left unmatched.
                const int    matched = overlap(sbx, sby);
                const double fit     = 1.5 * matched - 0.5 * double(cells_s.size());
                if (sc == scales.front())
                    fit_unscaled = fit;
                // Another scale only when the part is clearly visible at it (a missing part matches nothing at
                // any scale, and the smallest would win just by being small) and it fits clearly better.
                if (sc == scales.front() || (matched >= 0.6 * double(cells_s.size()) &&
                                             fit > std::max(best_fit, fit_unscaled + std::max(6.0, 0.03 * cells_g.size())))) {
                    best_fit   = fit;
                    bx         = sbx;
                    by         = sby;
                    bs         = sc;
                    best_cells = cells_s;
                }
            }
            res.alignment.anchors.push_back({mx, my, bx * cell, by * cell, bs});
            for (size_t i : best_cells) {
                const int xx = int(i % W) + bx, yy = int(i / W) + by;
                if (xx >= 0 && yy >= 0 && xx < W && yy < H)
                    new_allowed.at(xx, yy) = 1;
            }
            std::vector<size_t> fp_cells_g;
            if (bs == 1.0) {
                for (size_t i = 0; i < label.size(); ++i)
                    if (label[i] == int(g) && footprint.v[i])
                        fp_cells_g.push_back(i);
            } else
                fp_cells_g = scaled(bs, footprint.v);
            for (size_t i : fp_cells_g) {
                const int xx = int(i % W) + bx, yy = int(i / W) + by;
                if (xx >= 0 && yy >= 0 && xx < W && yy < H)
                    new_footprint.at(xx, yy) = 1;
            }
            std::vector<size_t> solid_g;
            if (bs == 1.0) {
                for (size_t i = 0; i < label.size(); ++i)
                    if (label[i] == int(g) && solid.v[i])
                        solid_g.push_back(i);
            } else
                solid_g = scaled(bs, solid.v);
            for (size_t i : solid_g) {
                const int xx = int(i % W) + bx, yy = int(i / W) + by;
                if (xx >= 0 && yy >= 0 && xx < W && yy < H)
                    new_solid.at(xx, yy) = 1;
            }
        }
        allowed   = new_allowed;
        footprint = new_footprint;
        solid     = new_solid;
        res.expected = allowed.v;
    }
    // Margin for what the offset does not absorb: 8 mm, plus the printed height, since a tall part
    // throws a shadow and reflects the chamber light onto the plate around it.
    const double printed_height = cp.layer > 0 && cp.layer <= model.layer_z.size() ? model.layer_z[cp.layer - 1] : 0.0;
    const int    margin_cells   = 4 + int(std::ceil(printed_height / cell));
    const Grid allowed_wide = dilate(allowed, margin_cells);
    const Grid allowed_far  = dilate(allowed_wide, 4); // a further 16 mm
    Grid core = erode4(footprint);
    for (size_t i = 0; i < core.v.size(); ++i)
        core.v[i] = core.v[i] && !in_base.v[i];
    // Shadows: as the bed drops, the gantry and the parts throw shadows that move across the plate, and a
    // shadow also hides the plate's glitter, which reads like dark matte filament. When every filament of
    // the job is much lighter than the plate, stray material can only make the plate brighter, so a patch
    // that got darker is a shadow, not material. Dark or unknown filament keeps counting either way. Only
    // the filaments the job uses count: a project often lists every loaded Box spool.
    bool light_filament = !model.first_layer_colour().empty();
    if (light_filament) {
        std::vector<float> plate;
        for (size_t i = 0; i < background.size(); ++i)
            if (background[i])
                plate.push_back(float((base[i * 3] + base[i * 3 + 1] + base[i * 3 + 2]) / 3.0));
        const double plate_lum = plate.empty() ? 128.0 : median(plate);
        for (size_t f = 0; f < model.filament_colours.size(); ++f) {
            const std::string& c = model.filament_colours[f];
            if (!model.filament_used(f))
                continue;
            if (c.size() < 7 || c[0] != '#') {
                light_filament = false;
                break;
            }
            const long rgb = std::strtol(c.c_str() + 1, nullptr, 16);
            if (0.299 * ((rgb >> 16) & 0xFF) + 0.587 * ((rgb >> 8) & 0xFF) + 0.114 * (rgb & 0xFF) < plate_lum + 60.0) {
                light_filament = false;
                break;
            }
        }
    }
    res.light_filament = light_filament;
    // With light filament, stray material brightens the plate about as much as the job's own parts do. A
    // smooth, glossy plate also brightens a little where it picks up light bounced off the white parts, or
    // where the exposure correction is slightly off, and its low noise lets that pass the threshold. So a
    // brightening only counts once it reaches a quarter of what the printed parts show over the plate (the
    // sheen stays under a tenth; a white blob, even in a shadow, is well above).
    double min_brightening = 0.0;
    if (light_filament) {
        std::vector<float> parts;
        for (int gy = 0; gy < H; ++gy)
            for (int gx = 0; gx < W; ++gx) {
                const size_t i = size_t(gy) * W + gx;
                if (valid[i] && core.get(gx, gy))
                    parts.push_back(sm_dlum[i]);
            }
        if (parts.size() >= 20)
            min_brightening = 0.25 * median(parts);
    }
    // Candidate cells: changed where the G-code has nothing. Only connected patches of 8+ cells (32 mm2)
    // count; spaghetti and blobs form patches, while the plate's glitter gives isolated specks.
    std::vector<uint8_t> candidate(size_t(W) * H, 0);
    for (int gy = 0; gy < H; ++gy)
        for (int gx = 0; gx < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            candidate[i]   = changed[i] && !allowed_wide.get(gx, gy) && (!light_filament || sm_dlum[i] > min_brightening);
        }
    // Keep only solid change: a cell needs at least 5 changed cells in its 3 x 3 neighbourhood. Glitter
    // that sparkles differently gives loose specks; spaghetti, blobs and parts give solid patches.
    {
        std::vector<uint8_t> solid(candidate.size(), 0);
        for (int gy = 0; gy < H; ++gy)
            for (int gx = 0; gx < W; ++gx) {
                const size_t i = size_t(gy) * W + gx;
                if (!candidate[i])
                    continue;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = gx + dx, yy = gy + dy;
                        if (xx >= 0 && yy >= 0 && xx < W && yy < H && changed[size_t(yy) * W + xx])
                            ++n;
                    }
                solid[i] = n >= 5;
            }
        candidate.swap(solid);
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
            // A band hugging a part (leftover calibration error, the part's shadow) is not a failure,
            // however long; spaghetti, blobs and parts that moved put a real patch of material well
            // away (16 mm) from where the G-code printed.
            size_t far_cells = 0;
            for (size_t i : comp)
                if (!allowed_far.get(int(i % W), int(i / W)))
                    ++far_cells;
            if (comp.size() >= 8 && far_cells >= 8)
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
                bool missing = !changed[i];
                // As for first-layer holes: on the edge, change one cell away is leftover calibration error.
                if (missing && !(core.get(gx - 1, gy) && core.get(gx + 1, gy) && core.get(gx, gy - 1) && core.get(gx, gy + 1)))
                    for (int dy = -1; dy <= 1 && missing; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = gx + dx, yy = gy + dy;
                            if (xx >= 0 && yy >= 0 && xx < W && yy < H && footprint.get(xx, yy) && changed[size_t(yy) * W + xx]) {
                                missing = false;
                                break;
                            }
                        }
                if (missing) {
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
    // Unchanged pixels only prove something when the filament should look different from the plate:
    // compare the filament colour from the G-code with the plate where the part should be.
    const std::string colour  = model.first_layer_colour();
    bool              visible = false;
    if (!colour.empty()) {
        const long   rgb = std::strtol(colour.c_str() + 1, nullptr, 16);
        const double fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fb = rgb & 0xFF;
        double       pr = 0, pg = 0, pb = 0;
        int          n  = 0;
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
    const bool first_layer = cp.kind == CheckpointKind::Early || cp.kind == CheckpointKind::FirstLayer;
    // First layer, part by part and cell by cell: a torn, lifted or dragged first layer leaves holes of a few
    // millimetres that the 6 mm smoothing above fills in, and a healthy part next to it dilutes the plate-wide
    // fraction. With a filament the camera can see, each part's own first layer must show material almost
    // everywhere.
    if (first_layer && visible) {
        // The first layer is printed in one filament: is it much lighter than the plate?
        bool first_light = false;
        {
            const long   rgb = std::strtol(colour.c_str() + 1, nullptr, 16);
            const double fl  = 0.299 * ((rgb >> 16) & 0xFF) + 0.587 * ((rgb >> 8) & 0xFF) + 0.114 * (rgb & 0xFF);
            std::vector<float> plate;
            for (size_t i = 0; i < background.size(); ++i)
                if (background[i])
                    plate.push_back(float((base[i * 3] + base[i * 3 + 1] + base[i * 3 + 2]) / 3.0));
            first_light = fl >= (plate.empty() ? 128.0 : median(plate)) + 60.0;
        }
        std::vector<uint8_t> seen(size_t(W) * H, 0), hole_cell(size_t(W) * H, 0);
        for (size_t start = 0; start < seen.size(); ++start) {
            if (seen[start] || !core.v[start] || !valid[start])
                continue;
            std::vector<size_t> stack{start}, part;
            seen[start] = 1;
            while (!stack.empty()) {
                const size_t i = stack.back();
                stack.pop_back();
                part.push_back(i);
                const int cx = int(i % W), cy = int(i / W);
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : nb) {
                    const int xx = cx + d[0], yy = cy + d[1];
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                        continue;
                    const size_t j = size_t(yy) * W + xx;
                    if (!seen[j] && core.v[j] && valid[j]) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
            // Light filament: a hole is a cell clearly darker than the part itself now (bare plate, or plate in
            // the shadow of a lifted flap). Comparing with the part, not with the reference photo, keeps a spot of
            // plate that happened to glint in the reference from reading as "did not brighten".
            auto lum_now = [&](size_t i) { return (cur[i * 3] + cur[i * 3 + 1] + cur[i * 3 + 2]) * cell_gain[i] / 3.0f; };
            float part_lum = 0;
            if (first_light) {
                std::vector<float> l;
                l.reserve(part.size());
                for (size_t i : part)
                    if (solid.v[i])
                        l.push_back(lum_now(i));
                if (l.empty())
                    continue;
                part_lum = float(median(l));
            }
            int n = 0;
            for (size_t i : part)
                n += solid.v[i] ? 1 : 0;
            auto bare = [&](size_t i) {
                return first_light ? lum_now(i) < 0.5f * part_lum : diff[i] <= std::max(res.threshold, texture_k * ref_texture[i]);
            };
            int holes = 0;
            for (size_t i : part) {
                if (!solid.v[i])
                    continue; // sparse support base or outline: not a solid first layer
                // On the part's edge, material seen one cell (2 mm) away is leftover calibration error rather than a
                // hole; a thin part (a ring a few millimetres wide) is all edge.
                const int cx = int(i % W), cy = int(i / W);
                bool      hole = bare(i);
                if (hole && !(core.get(cx - 1, cy) && core.get(cx + 1, cy) && core.get(cx, cy - 1) && core.get(cx, cy + 1)))
                    for (int dy = -1; dy <= 1 && hole; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = cx + dx, yy = cy + dy;
                            if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                                continue;
                            const size_t j = size_t(yy) * W + xx;
                            if (j != i && footprint.v[j] && valid[j] && !bare(j)) {
                                hole = false;
                                break;
                            }
                        }
                if (hole) {
                    ++holes;
                    hole_cell[i] = 1;
                    res.cells[i] = CellMissing; // drawn orange
                }
            }
            if (n >= 40)
                res.first_layer_holes = std::max(res.first_layer_holes, double(holes) / n);
        }
        // A peeled or torn patch is small in area (a lifted flap still covers most of it) but solid and well inside
        // the part; holes along a part's edge are leftover calibration error. Largest cluster of hole cells at
        // least two cells (4 mm) inside the part.
        {
            const Grid inner = erode4(erode4(core));
            std::vector<uint8_t> done(size_t(W) * H, 0);
            for (size_t start = 0; start < done.size(); ++start) {
                if (done[start] || !hole_cell[start] || !inner.v[start])
                    continue;
                std::vector<size_t> stack{start};
                done[start] = 1;
                int n = 0;
                while (!stack.empty()) {
                    const size_t i = stack.back();
                    stack.pop_back();
                    ++n;
                    const int cx = int(i % W), cy = int(i / W);
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = cx + dx, yy = cy + dy;
                            if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                                continue;
                            const size_t j = size_t(yy) * W + xx;
                            if (!done[j] && hole_cell[j] && inner.v[j]) {
                                done[j] = 1;
                                stack.push_back(j);
                            }
                        }
                }
                res.first_layer_patch_mm2 = std::max(res.first_layer_patch_mm2, n * cell * cell);
            }
        }
        if (res.first_layer_holes >= kFirstLayerHoles) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%.0f%% of a part's first layer shows holes or no material (lifting, tearing or not sticking)",
                          res.first_layer_holes * 100.0);
            res.reasons.push_back(buf);
            fail = true;
        } else if (res.first_layer_patch_mm2 >= kFirstLayerPatchMm2) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "a dark patch of about %.0f mm2 inside a part's first layer (torn, lifted or folded over)",
                          res.first_layer_patch_mm2);
            res.reasons.push_back(buf);
            fail = true;
        }
    }
    if (fp_cells >= 30 && res.missing_fraction >= 0.6) {
        char buf[200];
        if (first_layer)
            std::snprintf(buf, sizeof(buf), "%.0f%% of the first layer printed so far shows no new material (not sticking, or not extruding)",
                          res.missing_fraction * 100.0);
        else
            std::snprintf(buf, sizeof(buf), "%.0f%% of the latest layer is not where the G-code put it (the part may have come loose)",
                          res.missing_fraction * 100.0);
        res.reasons.push_back(buf);
        if (visible) {
            fail = true;
        } else {
            unsure = true;
            res.reasons.push_back(colour.empty() ? "the job does not say what colour the filament is, so this may be a filament that looks like the plate"
                                                 : "the filament colour is close to the plate's, so the camera may simply not see it");
        }
    }
    if (res.reference_mismatch) {
        char buf[240];
        std::snprintf(buf, sizeof(buf),
                      "the reference photo no longer matches the printer (%.0f%% of the view around the bed changed: a hand or "
                      "something else was in view, or the light changed), so this checkpoint could not be judged",
                      res.scene_change * 100.0);
        res.reasons.insert(res.reasons.begin(), buf);
        fail   = false;
        unsure = true;
    }
    res.verdict = fail ? Verdict::Fail : unsure ? Verdict::Unsure : Verdict::Pass;
    if (res.verdict == Verdict::Pass)
        res.reasons.push_back("the bed matches the G-code");
    return res;
}

std::vector<PrintedDot> find_printed_dots(const GCodeModel& model)
{
    std::vector<PrintedDot> dots;
    if (model.layer_first_segment.empty() || !model.has_bed())
        return dots;
    // Rasterise the first layer on a 0.5 mm grid, then take its islands.
    constexpr double res = 0.5;
    const int        W = int(std::ceil((model.bed_max_x - model.bed_min_x) / res)) + 1, H = int(std::ceil((model.bed_max_y - model.bed_min_y) / res)) + 1;
    std::vector<uint8_t> grid(size_t(W) * H, 0);
    const size_t s0 = model.layer_first_segment[0];
    const size_t s1 = model.layer_first_segment.size() > 1 ? model.layer_first_segment[1] : model.segments.size();
    for (size_t k = s0; k < s1; ++k) {
        const Segment& sg  = model.segments[k];
        const double   len = std::hypot(sg.x1 - sg.x0, sg.y1 - sg.y0);
        const int      n   = std::max(1, int(len / (res * 0.5)));
        const int      rr  = std::max(1, int(std::ceil(sg.w * 0.5 / res))); // full width, so neighbouring lines join
        for (int t = 0; t <= n; ++t) {
            const double x = sg.x0 + (sg.x1 - sg.x0) * t / n, y = sg.y0 + (sg.y1 - sg.y0) * t / n;
            const int    cx = int((x - model.bed_min_x) / res), cy = int((y - model.bed_min_y) / res);
            for (int dy = -rr; dy <= rr; ++dy)
                for (int dx = -rr; dx <= rr; ++dx)
                    if (cx + dx >= 0 && cy + dy >= 0 && cx + dx < W && cy + dy < H)
                        grid[size_t(cy + dy) * W + cx + dx] = 1;
        }
    }
    std::vector<uint8_t> seen(grid.size(), 0);
    for (size_t start = 0; start < grid.size(); ++start) {
        if (!grid[start] || seen[start])
            continue;
        std::vector<size_t> stack{start};
        seen[start]  = 1;
        double sx = 0, sy = 0;
        size_t n  = 0;
        int    x0 = W, x1 = -1, y0 = H, y1 = -1;
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            const int x = int(i % W), y = int(i / W);
            sx += x; sy += y; ++n;
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                        continue;
                    const size_t j = size_t(yy) * W + xx;
                    if (grid[j] && !seen[j]) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
        }
        const double w = (x1 - x0 + 1) * res, h = (y1 - y0 + 1) * res, area = n * res * res;
        const double d = std::max(w, h);
        // A dot: 8-20 mm, about as wide as long, and mostly filled (a disc fills pi/4 of its box).
        if (d < 8 || d > 20 || std::min(w, h) < 0.8 * d || area < 0.6 * d * d)
            continue;
        PrintedDot dot;
        dot.x    = model.bed_min_x + (sx / n + 0.5) * res;
        dot.y    = model.bed_min_y + (sy / n + 0.5) * res;
        dot.size = d;
        for (const Segment& sg : model.segments) // top: the highest extrusion over the dot
            if (std::hypot(0.5 * (sg.x0 + sg.x1) - dot.x, 0.5 * (sg.y0 + sg.y1) - dot.y) < 0.5 * d)
                dot.top = std::max(dot.top, double(sg.z));
        dots.push_back(dot);
    }
    return dots;
}

bool locate_dot(const Image& before, const Image& after, double u0, double v0, double radius_px, double expected_area_px, double& u,
                double& v)
{
    if (before.empty() || after.empty() || before.w != after.w || before.h != after.h)
        return false;
    const int x0 = std::max(0, int(u0 - radius_px)), x1 = std::min(after.w - 1, int(u0 + radius_px));
    const int y0 = std::max(0, int(v0 - radius_px)), y1 = std::min(after.h - 1, int(v0 + radius_px));
    if (x1 <= x0 || y1 <= y0)
        return false;
    const int          W = x1 - x0 + 1, H = y1 - y0 + 1;
    std::vector<float> d(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t(y0 + y) * after.w + (x0 + x)) * 3;
            d[size_t(y) * W + x] = (std::abs(int(after.rgb[i]) - before.rgb[i]) + std::abs(int(after.rgb[i + 1]) - before.rgb[i + 1]) +
                                    std::abs(int(after.rgb[i + 2]) - before.rgb[i + 2])) / 3.0f;
        }
    // Threshold well above the window's own noise (the plate's glitter sparkles differently every photo).
    std::vector<float> tmp = d;
    const double       med = median(tmp);
    for (float& x : tmp)
        x = float(std::abs(x - med));
    const double thr = std::max(25.0, med + 6.0 * 1.4826 * median(tmp));
    // Where a dot is big in the photo, keep only pixels whose whole 3 x 3 neighbourhood changed: a skirt or
    // purge line next to it is a pixel or two wide and would otherwise join it and pull its centre. (A far dot
    // is only a few pixels tall and would vanish.)
    const bool erode = expected_area_px >= 80;
    if (erode) {
    std::vector<float> solid(d.size(), 0.f);
    for (int y = 1; y + 1 < H; ++y)
        for (int x = 1; x + 1 < W; ++x) {
            bool all = true;
            for (int dy = -1; dy <= 1 && all; ++dy)
                for (int dx = -1; dx <= 1 && all; ++dx)
                    all = d[size_t(y + dy) * W + x + dx] > thr;
            if (all)
                solid[size_t(y) * W + x] = d[size_t(y) * W + x];
        }
    d.swap(solid);
    }
    // Eroding shrinks a dot by about its perimeter.
    const double area_lo = (erode ? 0.15 : 0.33) * expected_area_px, area_hi = 3.0 * expected_area_px;
    std::vector<uint8_t> seen(d.size(), 0);
    double best_score = -1;
    for (size_t start = 0; start < d.size(); ++start) {
        if (d[start] <= thr || seen[start])
            continue;
        std::vector<size_t> stack{start};
        seen[start] = 1;
        double wsum = 0, su = 0, sv = 0;
        size_t n    = 0;
        while (!stack.empty()) {
            const size_t i = stack.back();
            stack.pop_back();
            const int x = int(i % W), y = int(i / W);
            const double wgt = d[i] - thr;
            wsum += wgt; su += wgt * x; sv += wgt * y; ++n;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H)
                        continue;
                    const size_t j = size_t(yy) * W + xx;
                    if (d[j] > thr && !seen[j]) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
        }
        if (n < std::max(4.0, area_lo) || n > area_hi || wsum <= 0)
            continue;
        const double cu = x0 + su / wsum + 0.5, cv = y0 + sv / wsum + 0.5;
        const double dist = std::hypot(cu - u0, cv - v0);
        if (dist > radius_px)
            continue;
        // Prefer big patches near the prediction: a dot over a few sparkling specks.
        const double score = double(n) / (1.0 + dist / 10.0);
        if (score > best_score) {
            best_score = score;
            u          = cu;
            v          = cv;
        }
    }
    return best_score > 0;
}

bool dot_search(const CameraModel& cam, const std::vector<PrintedDot>& dots, size_t i, double h, double max_radius_px, double& u0,
                double& v0, double& radius_px, double& area_px)
{
    const PrintedDot& d = dots[i];
    if (!cam.project(d.x, d.y, h, u0, v0) || u0 < 0 || v0 < 0 || u0 >= cam.width || v0 >= cam.height)
        return false;
    // Area of the projected disc (shoelace over 24 rim points).
    double area = 0, pu = 0, pv = 0;
    for (int k = 0; k <= 24; ++k) {
        const double a = 2 * M_PI * k / 24;
        double       ru, rv;
        if (!cam.project(d.x + 0.5 * d.size * std::cos(a), d.y + 0.5 * d.size * std::sin(a), h, ru, rv))
            return false;
        if (k > 0)
            area += pu * rv - ru * pv;
        pu = ru;
        pv = rv;
    }
    area_px   = 0.5 * std::abs(area);
    radius_px = max_radius_px;
    for (size_t j = 0; j < dots.size(); ++j) {
        double uj, vj;
        if (j != i && cam.project(dots[j].x, dots[j].y, h, uj, vj))
            radius_px = std::min(radius_px, 0.5 * std::hypot(uj - u0, vj - v0));
    }
    return true;
}

double reprojection_rms(const CameraModel& cam, const std::vector<Correspondence>& pts)
{
    double sum = 0;
    int    n   = 0;
    for (const auto& p : pts) {
        double u, v;
        if (!cam.project(p.x, p.y, p.h, u, v))
            continue;
        sum += (u - p.u) * (u - p.u) + (v - p.v) * (v - p.v);
        ++n;
    }
    return n > 0 ? std::sqrt(sum / n) : -1.0;
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
