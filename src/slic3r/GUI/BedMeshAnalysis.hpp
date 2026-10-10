#pragma once

// Print-shop addition: analysis of a printer's full-bed mesh for the Device tab's mesh health view.
// No GUI dependencies, so it can be tested on its own.

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct BedMesh
{
    std::string profile;          // e.g. "default"
    double      min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    std::vector<std::vector<double>> z; // z[row][col]; row = Y index (front first), col = X index
    bool        trusted = false;  // built by SHOP_FULL_MESH (after Z_TILT_ADJUST)

    size_t cols() const { return z.empty() ? 0 : z.front().size(); }
    size_t rows() const { return z.size(); }
    double x_at(size_t col) const { return cols() > 1 ? min_x + (max_x - min_x) * double(col) / double(cols() - 1) : min_x; }
    double y_at(size_t row) const { return rows() > 1 ? min_y + (max_y - min_y) * double(row) / double(rows() - 1) : min_y; }
    bool   valid() const { return rows() >= 3 && cols() >= 3; }
};

struct BedMeshPoint
{
    double x = 0, y = 0, z = 0;
};

struct BedMeshStats
{
    double zmin = 0, zmax = 0, range = 0, mean = 0;
    BedMeshPoint high, low;
    // Best-fit plane z = a + b*x + c*y
    double plane_a = 0, plane_b = 0, plane_c = 0;
    double tilt_x = 0;     // plane height change across the mesh, left -> right (mm)
    double tilt_y = 0;     // plane height change across the mesh, front -> back (mm)
    double warp_range = 0; // range of the residual after removing the plane (mm)
    std::vector<std::vector<double>> warp; // residual, same layout as z
    std::vector<BedMeshPoint> spots;       // interior bumps/dips vs their 8 neighbours (residual), merged
    struct Corner { const char* name; BedMeshPoint p; double warp; };
    Corner worst_corner{"", {}, 0.0};      // corner whose residual is furthest from the plane
};

// Parses a Moonraker /printer/objects/query?bed_mesh&save_variables response.
bool parse_moonraker_bed_mesh(const std::string& json, const std::string& profile, BedMesh& out, std::string& error);

BedMeshStats analyze_bed_mesh(const BedMesh& mesh);

// Plain-language maintenance notes, most important first.
std::vector<std::string> bed_mesh_recommendations(const BedMesh& mesh, const BedMeshStats& stats);

}} // namespace Slic3r::GUI
