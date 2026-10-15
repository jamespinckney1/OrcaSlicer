#include "BedMeshView.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "slic3r/Utils/Http.hpp"

#include <algorithm>
#include <exception>
#include <boost/log/trivial.hpp>
#include <cmath>
#include <regex>
#include <thread>
#include <utility>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

namespace Slic3r { namespace GUI {

// ---- canvas ---------------------------------------------------------------------------------

BedMeshCanvas::BedMeshCanvas(wxWindow* parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(FromDIP(wxSize(320, 220)));
    Bind(wxEVT_PAINT, &BedMeshCanvas::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &BedMeshCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &BedMeshCanvas::on_mouse, this);
    Bind(wxEVT_MOTION, &BedMeshCanvas::on_mouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
    SetToolTip(_L("Drag to rotate the view."));
}

void BedMeshCanvas::set_mesh(const BedMesh& mesh, const BedMeshStats& stats)
{
    m_mesh = mesh;
    m_stats = stats;
    m_has_mesh = mesh.valid();
    Refresh();
}

void BedMeshCanvas::clear()
{
    m_has_mesh = false;
    Refresh();
}

void BedMeshCanvas::on_mouse(wxMouseEvent& e)
{
    if (e.LeftDown()) {
        m_dragging = true;
        m_drag_start = e.GetPosition();
        m_drag_az = m_azimuth;
        m_drag_el = m_elevation;
        CaptureMouse();
    } else if (e.LeftUp() && m_dragging) {
        m_dragging = false;
        if (HasCapture()) ReleaseMouse();
    } else if (e.Dragging() && m_dragging) {
        const wxPoint d = e.GetPosition() - m_drag_start;
        m_azimuth   = m_drag_az + d.x * 0.5;
        m_elevation = std::clamp(m_drag_el + d.y * 0.3, 10.0, 80.0);
        Refresh();
    }
}

// Diverging blue (low) - near white (centre) - red (high).
static wxColour diverging(double t)
{
    t = std::clamp(t, -1.0, 1.0);
    const double r0 = 0.95, g0 = 0.95, b0 = 0.95;
    double r, g, b;
    if (t < 0) { const double k = -t; r = r0 + (0.13 - r0) * k; g = g0 + (0.40 - g0) * k; b = b0 + (0.85 - b0) * k; }
    else       { const double k = t;  r = r0 + (0.84 - r0) * k; g = g0 + (0.19 - g0) * k; b = b0 + (0.15 - b0) * k; }
    return wxColour((unsigned char) (r * 255), (unsigned char) (g * 255), (unsigned char) (b * 255));
}

void BedMeshCanvas::on_paint(wxPaintEvent&)
{
    // An exception escaping a paint handler aborts the whole app, so contain it here.
    try {
        paint();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Bed mesh view: drawing failed: " << e.what();
    }
}

void BedMeshCanvas::paint()
{
    wxAutoBufferedPaintDC dc(this);
    const wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    const wxColour fg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    dc.SetBackground(wxBrush(bg));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return;
    const wxSize sz = GetClientSize();
    gc->SetFont(GetFont(), fg);
    if (!m_has_mesh) {
        const wxString msg = _L("No mesh loaded.");
        double w, h;
        gc->GetTextExtent(msg, &w, &h);
        gc->DrawText(msg, (sz.x - w) / 2, (sz.y - h) / 2);
        return;
    }

    // Upsample the grid bilinearly (4 sub-cells per cell) for a smoother surface.
    const size_t R = m_mesh.rows(), C = m_mesh.cols(), sub = 4;
    const size_t NR = (R - 1) * sub + 1, NC = (C - 1) * sub + 1;
    // analyze_bed_mesh leaves warp empty when the plane fit fails; show raw heights then.
    const bool  use_warp = m_warp_mode && m_stats.warp.size() == R && m_stats.warp.front().size() == C;
    const auto& src = use_warp ? m_stats.warp : m_mesh.z;
    auto sample = [&](double fr, double fc) {
        const size_t r0 = std::min(size_t(fr), R - 2), c0 = std::min(size_t(fc), C - 2);
        const double tr = fr - r0, tc = fc - c0;
        return (1 - tr) * ((1 - tc) * src[r0][c0] + tc * src[r0][c0 + 1]) + tr * ((1 - tc) * src[r0 + 1][c0] + tc * src[r0 + 1][c0 + 1]);
    };
    std::vector<std::vector<double>> zz(NR, std::vector<double>(NC));
    double vmin = 1e9, vmax = -1e9;
    for (size_t r = 0; r < NR; ++r)
        for (size_t c = 0; c < NC; ++c) {
            zz[r][c] = sample(double(r) / sub, double(c) / sub);
            vmin = std::min(vmin, zz[r][c]);
            vmax = std::max(vmax, zz[r][c]);
        }
    const double centre = use_warp ? 0.0 : 0.5 * (vmin + vmax);
    const double half   = std::max(0.05, std::max(vmax - centre, centre - vmin));

    // Projection: bed centred at the origin, rotate about Z, tilt by elevation, exaggerate height.
    const double bw = m_mesh.max_x - m_mesh.min_x, bh = m_mesh.max_y - m_mesh.min_y;
    const double span = std::max(bw, bh);
    const double exag = 0.22 * span / std::max(vmax - vmin, 0.05);
    const double az = m_azimuth * M_PI / 180.0, el = m_elevation * M_PI / 180.0;
    auto project = [&](double x, double y, double z, double& sx, double& sy, double& depth) {
        const double px = x - (m_mesh.min_x + bw / 2), py = y - (m_mesh.min_y + bh / 2);
        const double rx = px * std::cos(az) - py * std::sin(az);
        const double ry = px * std::sin(az) + py * std::cos(az);
        sx    = rx;
        sy    = -ry * std::sin(el) - (z - centre) * exag * std::cos(el);
        depth = ry;
    };
    struct Quad { double depth; wxPoint2DDouble p[4]; double v; };
    std::vector<Quad> quads;
    quads.reserve((NR - 1) * (NC - 1));
    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    std::vector<std::vector<wxPoint2DDouble>> P(NR, std::vector<wxPoint2DDouble>(NC));
    std::vector<std::vector<double>> D(NR, std::vector<double>(NC));
    for (size_t r = 0; r < NR; ++r)
        for (size_t c = 0; c < NC; ++c) {
            const double x = m_mesh.min_x + bw * double(c) / double(NC - 1), y = m_mesh.min_y + bh * double(r) / double(NR - 1);
            double sx, sy, d;
            project(x, y, zz[r][c], sx, sy, d);
            P[r][c] = {sx, sy};
            D[r][c] = d;
            minx = std::min(minx, sx); maxx = std::max(maxx, sx); miny = std::min(miny, sy); maxy = std::max(maxy, sy);
        }
    // Keep the side labels inside the canvas.
    for (const auto& [x, y] : {std::pair{m_mesh.min_x - span * 0.12, m_mesh.min_y - span * 0.12},
                               std::pair{m_mesh.max_x + span * 0.12, m_mesh.min_y - span * 0.12},
                               std::pair{m_mesh.min_x - span * 0.12, m_mesh.max_y + span * 0.12},
                               std::pair{m_mesh.max_x + span * 0.12, m_mesh.max_y + span * 0.12}}) {
        double sx, sy, d;
        project(x, y, centre, sx, sy, d);
        minx = std::min(minx, sx); maxx = std::max(maxx, sx); miny = std::min(miny, sy); maxy = std::max(maxy, sy);
    }
    for (size_t r = 0; r + 1 < NR; ++r)
        for (size_t c = 0; c + 1 < NC; ++c)
            quads.push_back({(D[r][c] + D[r + 1][c + 1]) / 2, {P[r][c], P[r][c + 1], P[r + 1][c + 1], P[r + 1][c]},
                             (zz[r][c] + zz[r][c + 1] + zz[r + 1][c] + zz[r + 1][c + 1]) / 4});
    std::sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.depth > b.depth; }); // far first

    const double margin = FromDIP(18), legend_w = FromDIP(54);
    const double avail_w = sz.x - 2 * margin - legend_w, avail_h = sz.y - 2 * margin;
    const double scale = std::min(avail_w / std::max(maxx - minx, 1.0), avail_h / std::max(maxy - miny, 1.0));
    const double ox = margin + (avail_w - (maxx - minx) * scale) / 2 - minx * scale;
    const double oy = margin + (avail_h - (maxy - miny) * scale) / 2 - miny * scale;
    auto to_screen = [&](const wxPoint2DDouble& p) { return wxPoint2DDouble(ox + p.m_x * scale, oy + p.m_y * scale); };

    const wxColour edge(0, 0, 0, 28);
    for (const auto& q : quads) {
        wxGraphicsPath path = gc->CreatePath();
        path.MoveToPoint(to_screen(q.p[0]));
        for (int k = 1; k < 4; ++k) path.AddLineToPoint(to_screen(q.p[k]));
        path.CloseSubpath();
        gc->SetBrush(wxBrush(diverging((q.v - centre) / half)));
        gc->SetPen(wxPen(edge, 1));
        gc->DrawPath(path);
    }

    // High / low markers (raw heights) and a "Front" label.
    auto marker = [&](const BedMeshPoint& p, const wxString& label) {
        double sx, sy, d;
        const size_t r = size_t(std::lround((p.y - m_mesh.min_y) / std::max(bh, 1e-6) * (NR - 1)));
        const size_t c = size_t(std::lround((p.x - m_mesh.min_x) / std::max(bw, 1e-6) * (NC - 1)));
        project(p.x, p.y, zz[std::min(r, NR - 1)][std::min(c, NC - 1)], sx, sy, d);
        const wxPoint2DDouble s = to_screen({sx, sy});
        gc->SetPen(wxPen(fg, 1));
        gc->SetBrush(wxBrush(bg));
        gc->DrawEllipse(s.m_x - 4, s.m_y - 4, 8, 8);
        gc->DrawText(label, s.m_x + 6, s.m_y - 8);
    };
    marker(m_stats.high, _L("high") + wxString::Format(" %+.2f", m_stats.high.z));
    marker(m_stats.low, _L("low") + wxString::Format(" %+.2f", m_stats.low.z));
    // Side labels, as seen standing at the printer's door: front is the door side, left is the
    // viewer's left (X min), back is the far wall (Y max).
    auto side_label = [&](double x, double y, const wxString& text) {
        double sx, sy, d;
        project(x, y, centre, sx, sy, d);
        const wxPoint2DDouble s = to_screen({sx, sy});
        double w, h;
        gc->GetTextExtent(text, &w, &h);
        gc->DrawText(text, s.m_x - w / 2, s.m_y - h / 2);
    };
    const double pad = span * 0.08;
    side_label(m_mesh.min_x + bw / 2, m_mesh.min_y - pad, _L("Front"));
    side_label(m_mesh.min_x + bw / 2, m_mesh.max_y + pad, _L("Back"));
    side_label(m_mesh.min_x - pad, m_mesh.min_y + bh / 2, _L("Left"));
    side_label(m_mesh.max_x + pad, m_mesh.min_y + bh / 2, _L("Right"));

    // Legend
    const double lx = sz.x - margin - legend_w + FromDIP(8), ly = margin, lh = sz.y - 2 * margin, lw = FromDIP(10);
    for (int i = 0; i < 64; ++i) {
        const double t = 1.0 - 2.0 * i / 63.0;
        gc->SetBrush(wxBrush(diverging(t)));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRectangle(lx, ly + lh * i / 64.0, lw, lh / 64.0 + 1);
    }
    gc->DrawText(wxString::Format("%+.2f", centre + half), lx + lw + 4, ly - 2);
    gc->DrawText(wxString::Format("%+.2f", centre - half), lx + lw + 4, ly + lh - 12);
    gc->DrawText(use_warp ? _L("warp") : _L("mm"), lx, ly + lh / 2 - 6);
}

// ---- panel ----------------------------------------------------------------------------------

BedMeshPanel::BedMeshPanel(wxWindow* parent, std::function<std::string()> base_url_fn)
    : wxPanel(parent, wxID_ANY), m_base_url_fn(std::move(base_url_fn))
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    m_canvas  = new BedMeshCanvas(this);
    row->Add(m_canvas, 3, wxEXPAND | wxALL, FromDIP(6));

    auto* side = new wxBoxSizer(wxVERTICAL);
    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    m_mode = new wxChoice(this, wxID_ANY);
    m_mode->Append(_L("Heights"));
    m_mode->Append(_L("Warp (tilt removed)"));
    m_mode->SetSelection(0);
    m_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { m_canvas->set_warp_mode(m_mode->GetSelection() == 1); });
    m_refresh = new wxButton(this, wxID_ANY, _L("Refresh"));
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_from_printer(); });
    controls->Add(m_mode, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    controls->Add(m_refresh, 0, wxALIGN_CENTER_VERTICAL);
    side->Add(controls, 0, wxBOTTOM, FromDIP(6));
    m_stats = new wxStaticText(this, wxID_ANY, "");
    side->Add(m_stats, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    m_notes = new wxStaticText(this, wxID_ANY, "");
    side->Add(m_notes, 1, wxEXPAND);
    row->Add(side, 2, wxEXPAND | wxALL, FromDIP(6));
    SetSizer(row);
    SetMinSize(FromDIP(wxSize(-1, 260)));
}

void BedMeshPanel::show_error(const wxString& message)
{
    m_canvas->clear();
    m_stats->SetLabel("");
    m_notes->SetLabel(message);
    m_notes->Wrap(FromDIP(300));
    Layout();
}

void BedMeshPanel::show_mesh(const BedMesh& mesh)
{
    const BedMeshStats s = analyze_bed_mesh(mesh);
    m_canvas->set_mesh(mesh, s);
    m_stats->SetLabel(format_wxstr(_L("Profile \"%1%\", %2% x %3% points%4%\n"
                                      "Range %5% mm (low %6%, high %7%)\n"
                                      "Tilt left to right %8% mm, front to back %9% mm\n"
                                      "Warp after removing tilt %10% mm"),
                                   from_u8(mesh.profile), mesh.cols(), mesh.rows(),
                                   mesh.trusted ? _L(", built by Rebuild full-bed mesh") : wxString(""),
                                   wxString::Format("%.2f", s.range), wxString::Format("%+.2f", s.low.z), wxString::Format("%+.2f", s.high.z),
                                   wxString::Format("%+.2f", s.tilt_x), wxString::Format("%+.2f", s.tilt_y),
                                   wxString::Format("%.2f", s.warp_range)));
    wxString notes;
    for (const std::string& n : bed_mesh_recommendations(mesh, s))
        notes += wxString::FromUTF8("• ") + from_u8(n) + "\n";
    m_notes->SetLabel(notes);
    m_notes->Wrap(std::max(FromDIP(220), m_notes->GetSize().x > 0 ? m_notes->GetSize().x : FromDIP(300)));
    Layout();
}

void BedMeshPanel::refresh_from_printer()
{
    if (m_fetching)
        return;
    const std::string root = m_base_url_fn ? m_base_url_fn() : std::string();
    if (root.empty()) {
        show_error(_L("No printer page is loaded, so the mesh cannot be fetched."));
        return;
    }
    m_fetching = true;
    m_refresh->Disable();
    m_notes->SetLabel(_L("Fetching the bed mesh from the printer..."));
    Layout();
    wxWeakRef<BedMeshPanel> self(this);
    std::thread([self, root]() {
        // Nothing may escape a detached thread (std::terminate), so failures become an error message.
        BedMesh     mesh;
        std::string error;
        bool        parsed = false;
        try {
            auto fetch = [](const std::string& base, std::string& body) {
                bool ok = false;
                Http::get(base + "/printer/objects/query?bed_mesh&save_variables")
                    .timeout_connect(4)
                    .timeout_max(20)
                    .on_complete([&](std::string b, unsigned status) { if (status == 200) { body = std::move(b); ok = true; } })
                    .on_error([](std::string, std::string, unsigned) {})
                    .perform_sync();
                return ok;
            };
            std::string body;
            bool ok = fetch(root, body);
            if (!ok) {
                static const std::regex host_re(R"(^(https?://[^/:]+))", std::regex::icase);
                std::smatch m;
                if (std::regex_search(root, m, host_re))
                    ok = fetch(m[1].str() + ":7125", body);
            }
            parsed = ok && parse_moonraker_bed_mesh(body, "default", mesh, error);
            if (!ok)
                error = "could not reach the printer";
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Bed mesh view: fetching the mesh failed: " << e.what();
            parsed = false;
            error  = "fetching the mesh failed";
        } catch (...) {
            parsed = false;
            error  = "fetching the mesh failed";
        }
        wxGetApp().CallAfter([self, parsed, mesh, error]() {
            if (!self)
                return;
            self->m_fetching = false;
            self->m_refresh->Enable();
            if (parsed)
                self->show_mesh(mesh);
            else
                self->show_error(_L("No full-bed mesh to show") + ": " + from_u8(error) + ".");
        });
    }).detach();
}

}} // namespace Slic3r::GUI
