#pragma once

// Print-shop addition: G-code-aware print failure check, core logic (no GUI dependencies, so it can
// be tested on its own).
//
// The slicer inserts `SHOP_CHECKPOINT N=<n>` lines into an uploaded job: one before the first layer
// (the reference photo of the bed), one a few moves into the first layer, one at the end of the first
// layer and one every 10% after that. At each checkpoint the printer parks the toolhead and waits; the
// slicer photographs the bed and compares the photo with what the G-code says has been printed so far,
// so supports, brims and purge lines count as expected material instead of looking like spaghetti.
//
// Coordinates: bed X/Y as in the G-code. Heights are "frame heights", relative to the nozzle-tip plane:
// a point at height z on the part is at h = z - toolhead_z, because the bed moves in Z and the camera
// is fixed to the frame (Qidi Plus 4 and other CoreXY printers with a Z bed).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace FailureCheck {

// Where SHOP_CHECKPOINT parks the toolhead (bed X/Y, mm).
constexpr double kShopParkX = 95.0, kShopParkY = 324.0;

struct Segment
{
    float x0, y0, x1, y1;
    float z; // top of the extrusion
    float w; // extrusion width
};

enum class CheckpointKind { Baseline, Early, FirstLayer, EarlyLayers, Progress };

struct Checkpoint
{
    int            index         = 0;
    CheckpointKind kind          = CheckpointKind::Baseline;
    int            percent       = 0; // Progress only
    size_t         line          = 0; // inserted before this line (0-based, not counting SHOP_CHECKPOINT lines)
    size_t         segments_done = 0; // extrusions printed before the checkpoint
    size_t         layer         = 0; // index of the layer in progress (Baseline/Early) or the next layer
};

struct GCodeModel
{
    std::vector<Segment>    segments;            // print order
    std::vector<size_t>     segment_line;        // source line of each segment
    std::vector<size_t>     layer_first_segment; // per layer
    std::vector<size_t>     layer_line;          // line of each layer change marker
    std::vector<double>     layer_z;
    double                  bed_min_x = 0, bed_min_y = 0, bed_max_x = 0, bed_max_y = 0; // printable_area
    std::vector<std::string> filament_colours; // "#RRGGBB" per extruder, from the config block
    std::vector<double>      filament_used_mm; // per extruder, from the "filament used [mm]" summary
    int                      first_tool = 0;   // tool printing the first layer

    // Whether the job prints with this filament (every filament counts when the summary is missing).
    bool filament_used(size_t i) const { return filament_used_mm.size() != filament_colours.size() || filament_used_mm[i] > 0; }
    std::vector<Checkpoint> checkpoints;

    // Colour of the filament on the first layer, unless it is unset or Orca's default (then empty).
    std::string             first_layer_colour() const;

    bool              has_bed() const { return bed_max_x > bed_min_x && bed_max_y > bed_min_y; }
    const Checkpoint* checkpoint(int index) const;
};

// Parses extrusions and layer changes. A file that already has SHOP_CHECKPOINT lines (an uploaded job)
// keeps exactly those checkpoints; otherwise they are planned. Line numbers never count SHOP_CHECKPOINT lines.
bool parse_gcode(const std::string& path, GCodeModel& out, std::string& error);
// Copies in_path to out_path (which may be the same file) with the model's checkpoints inserted and any
// older SHOP_CHECKPOINT lines removed. An empty checkpoint list just strips them.
bool write_with_checkpoints(const std::string& in_path, const std::string& out_path, const std::vector<Checkpoint>& checkpoints,
                            int timeout_s, std::string& error);
// English label for logs and the G-code comment, e.g. "first layer done" or "40%".
std::string checkpoint_label(const Checkpoint& cp);

struct Image
{
    int                  w = 0, h = 0;
    std::vector<uint8_t> rgb; // w * h * 3
    bool                 empty() const { return w <= 0 || h <= 0 || rgb.size() < size_t(w) * size_t(h) * 3; }
};

// Pinhole camera with one radial distortion term, mapping frame coordinates to image pixels.
struct CameraModel
{
    bool        valid  = false;
    int         width  = 640;
    int         height = 480;
    double      P[12]  = {};  // 3x4 row-major: (x, y, h, 1) -> undistorted pixels
    double      k1     = 0;   // radial distortion about the image centre, radius normalised by width / 2
    double      rms_px = 0;   // reprojection error of the calibration
    int         points = 0;   // number of calibration points
    double      park_x = kShopParkX; // toolhead position while parked at a checkpoint
    double      park_y = kShopParkY;
    std::string when;
    // Image segments (u1, v1, u2, v2), top first, of vertical rods fixed in the frame beside the plate. The plate
    // mirrors them, and a vertical rod's reflection continues its own image line, so plate samples near the
    // line below its top, within reach of the plate's edge, are ignored.
    std::vector<std::array<double, 4>> frame_lines;

    // To observed (distorted) pixels; false when the point is behind the camera.
    bool        project(double x, double y, double h, double& u, double& v) const;
    std::string to_json() const;
    static bool from_json(const std::string& text, CameraModel& out);
};

struct Correspondence
{
    double x, y, h; // frame coordinates
    double u, v;    // clicked pixel
    int    group = -1; // >= 0: x/y are only a first guess, shared by every point of the group (one plate corner
                       // seen at several bed heights) and refined during calibration
};

// Direct linear transform with a search over the distortion term. Needs at least 6 points that are not
// all in one plane, e.g. the nozzle tip at a grid of positions (h = 0) plus plate corners at two bed
// heights. Grouped points get their x/y re-estimated, so the plate's exact size need not be known.
bool calibrate_camera(std::vector<Correspondence> pts, int width, int height, CameraModel& out, std::string& error);

// ---- calibration from a test print ----
// A test print of small dots spread over the bed; the G-code says exactly where each one is, so photos of
// the plate before and after it at several bed heights give exact correspondences, with no clicking.

struct PrintedDot
{
    double x = 0, y = 0; // centre (mm), by area
    double top  = 0;     // height of the dot's top surface (mm)
    double size = 0;     // larger side of its bounding box (mm)
};
// First-layer islands of the G-code that look like dots (round, 8-20 mm across): purge lines, skirts and
// anything else are left out.
std::vector<PrintedDot> find_printed_dots(const GCodeModel& model);

// Centre of the material that appeared between two photos near (u0, v0): the changed patch nearest to it
// within radius_px whose area is within a factor of 3 of expected_area_px, weighted by how much each pixel
// changed. False when nothing dot-like changed there.
bool locate_dot(const Image& before, const Image& after, double u0, double v0, double radius_px, double expected_area_px, double& u,
                double& v);
// Expected image area (px) of a printed dot, and the window for locate_dot: half the distance to the nearest
// other dot's predicted position, at most max_radius_px. False when the dot does not project into the image.
bool dot_search(const CameraModel& cam, const std::vector<PrintedDot>& dots, size_t i, double h, double max_radius_px, double& u0,
                double& v0, double& radius_px, double& area_px);

// Reprojection error (px, RMS) of a camera on correspondences, or -1 when none projects.
double reprojection_rms(const CameraModel& cam, const std::vector<Correspondence>& pts);

enum class Verdict { Pass, Fail, Unsure };

enum CellState : uint8_t {
    CellInvalid = 0,    // outside the picture, hidden by the toolhead, or off the bed
    CellUnchanged,      // looks like the reference photo
    CellExpected,       // changed, and the G-code put material here
    CellUnexpected,     // changed where the G-code has no material
    CellMissing,        // the G-code put material here but nothing changed
};

// Calibration is good to roughly a centimetre, and its error varies across the bed, so each comparison
// first finds, for every separate part on the bed, the bed X/Y offset that best lines that part up with
// what changed. On the first layer (where a layer shift is impossible) it searches up to 16 mm; later
// checkpoints pass the first layer's offsets as `prior`, and each part may only move a few millimetres
// from the offset of the nearest first-layer part, so a real layer shift is not explained away.
struct Alignment
{
    struct Anchor
    {
        double x, y;    // part centre (mm)
        double dx, dy;  // offset (mm)
        double s = 1.0; // scale about the part's bounding-box centre
    };
    std::vector<Anchor> anchors;
    bool valid() const { return !anchors.empty(); }
};

struct CheckResult
{
    Verdict                  verdict          = Verdict::Unsure;
    double                   unexpected_mm2   = 0;
    double                   missing_fraction = 0;
    int                      footprint_cells  = 0;
    int                      valid_cells      = 0;
    int                      bed_cells        = 0;
    double                   threshold        = 0;
    bool                     light_filament   = false; // darkened patches were taken as shadows
    double                   first_layer_holes = 0;    // worst part's share of first-layer cells with no material
    double                   first_layer_patch_mm2 = 0; // largest patch of them well inside a part (mm2)
    // Largest solid patch of the frame around the plate that differs between the photos (share of it). From
    // kSceneChange (4%) on the reference no longer shows the scene: a hand or an object was in view in one of
    // them, and the light it blocked or bounced changes the plate as well. The verdict is then unsure, and
    // during the first layers the caller takes this photo as the job's new reference.
    double                   scene_change       = 0;
    bool                     reference_mismatch = false;
    std::vector<std::string> reasons; // English, one per finding
    // Per-part offsets that line the G-code up with the photo (absorbing calibration error).
    Alignment                alignment;

    // Bed-plane grid used for the comparison, for overlays (row 0 = front).
    double               cell_mm = 2.0;
    double               grid_x0 = 0, grid_y0 = 0;
    int                  grid_w = 0, grid_h = 0;
    std::vector<uint8_t> cells;    // CellState
    std::vector<uint8_t> expected; // 1 where the aligned G-code put material (for the overlay)
};

// Compares the photo at a checkpoint with the reference photo taken before the first layer.
// baseline_z / current_z are the toolhead Z positions when each photo was taken. A reference taken at a later
// checkpoint (after the first one turned out not to match the scene) already shows the material of its first
// `baseline_segments` extrusions; those cells cannot change any more and do not count as missing.
CheckResult compare(const GCodeModel& model, const Checkpoint& cp, const CameraModel& camera, const Image& baseline, double baseline_z,
                    const Image& current, double current_z, const Alignment& prior = Alignment(), size_t baseline_segments = 0);

// Top-down picture of what the G-code has printed by `segments_done` (1 pixel per `mm_per_px`, back at
// the top); the most recent layer is highlighted. Used when the camera is not calibrated.
Image render_expected_topdown(const GCodeModel& model, size_t segments_done, double mm_per_px = 1.0);

}}} // namespace Slic3r::GUI::FailureCheck
