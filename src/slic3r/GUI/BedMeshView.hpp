#pragma once

// Print-shop addition: bed mesh health view for the Device tab. Fetches the printer's full-bed mesh
// from Moonraker, draws it as an angled 3D surface (drag to rotate) and lists tilt, warp and
// maintenance recommendations from BedMeshAnalysis.

#include "BedMeshAnalysis.hpp"

#include <functional>
#include <string>
#include <wx/panel.h>

class wxStaticText;
class wxButton;
class wxChoice;

namespace Slic3r { namespace GUI {

class BedMeshCanvas : public wxPanel
{
public:
    explicit BedMeshCanvas(wxWindow* parent);
    void set_mesh(const BedMesh& mesh, const BedMeshStats& stats);
    void set_warp_mode(bool warp) { m_warp_mode = warp; Refresh(); }
    void clear();

private:
    void on_paint(wxPaintEvent&);
    void paint();
    void on_mouse(wxMouseEvent&);

    BedMesh      m_mesh;
    BedMeshStats m_stats;
    bool         m_has_mesh{false};
    bool         m_warp_mode{false};
    double       m_azimuth{-35.0};   // degrees
    double       m_elevation{32.0};  // degrees
    wxPoint      m_drag_start;
    double       m_drag_az{0}, m_drag_el{0};
    bool         m_dragging{false};
};

class BedMeshPanel : public wxPanel
{
public:
    // base_url_fn returns the printer page root (scheme://host[:port]); port 7125 is tried as fallback.
    BedMeshPanel(wxWindow* parent, std::function<std::string()> base_url_fn);
    void refresh_from_printer();

private:
    void show_mesh(const BedMesh& mesh);
    void show_error(const wxString& message);

    std::function<std::string()> m_base_url_fn;
    BedMeshCanvas* m_canvas{nullptr};
    wxStaticText*  m_stats{nullptr};
    wxStaticText*  m_notes{nullptr};
    wxChoice*      m_mode{nullptr};
    wxButton*      m_refresh{nullptr};
    bool           m_fetching{false};
};

}} // namespace Slic3r::GUI
