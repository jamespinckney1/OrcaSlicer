#include "FailureCheckDialogs.hpp"

#include "FailureCheckMonitor.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "ShopNotify.hpp"
#include "format.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/geometry.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/weakref.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <exception>
#include <functional>
#include <thread>

namespace Slic3r { namespace GUI {

namespace {

wxImage to_wx(const FailureCheck::Image& img)
{
    if (img.empty())
        return wxImage();
    wxImage out(img.w, img.h, false);
    std::copy(img.rgb.begin(), img.rgb.begin() + size_t(img.w) * img.h * 3, out.GetData());
    return out;
}

void blend(wxImage& img, int x, int y, unsigned char r, unsigned char g, unsigned char b, double a)
{
    if (x < 0 || y < 0 || x >= img.GetWidth() || y >= img.GetHeight())
        return;
    unsigned char* p = img.GetData() + (size_t(y) * img.GetWidth() + x) * 3;
    p[0] = (unsigned char) (p[0] * (1 - a) + r * a);
    p[1] = (unsigned char) (p[1] * (1 - a) + g * a);
    p[2] = (unsigned char) (p[2] * (1 - a) + b * a);
}

// Marks the comparison on the photo: red = material the G-code does not explain, orange = expected
// material not seen, green = what the G-code printed most recently.
void draw_overlay(wxImage& img, const FailureCheckEvent& ev)
{
    const auto& r   = ev.result;
    const auto& cam = ev.camera;
    for (int gy = 0; gy < r.grid_h; ++gy)
        for (int gx = 0; gx < r.grid_w; ++gx) {
            const uint8_t s = r.cells[size_t(gy) * r.grid_w + gx];
            if (s != FailureCheck::CellUnexpected && s != FailureCheck::CellMissing)
                continue;
            const double x = r.grid_x0 + (gx + 0.5) * r.cell_mm, y = r.grid_y0 + (gy + 0.5) * r.cell_mm;
            double u, v, u2, v2;
            if (!cam.project(x, y, -ev.z, u, v) || !cam.project(x + r.cell_mm, y, -ev.z, u2, v2))
                continue;
            const int half = std::max(1, int(std::ceil(std::hypot(u2 - u, v2 - v) / 2)));
            for (int dy = -half; dy <= half; ++dy)
                for (int dx = -half; dx <= half; ++dx)
                    if (s == FailureCheck::CellUnexpected)
                        blend(img, int(u) + dx, int(v) + dy, 240, 40, 40, 0.55);
                    else
                        blend(img, int(u) + dx, int(v) + dy, 250, 160, 20, 0.45);
        }
    // Outline of where the (aligned) G-code put material.
    const int W = r.grid_w, H = r.grid_h;
    if (r.expected.size() != size_t(W) * H)
        return;
    for (int gy = 1; gy + 1 < H; ++gy)
        for (int gx = 1; gx + 1 < W; ++gx) {
            const size_t i = size_t(gy) * W + gx;
            if (!r.expected[i] || (r.expected[i - 1] && r.expected[i + 1] && r.expected[i - W] && r.expected[i + W]))
                continue;
            double u, v;
            if (cam.project(r.grid_x0 + (gx + 0.5) * r.cell_mm, r.grid_y0 + (gy + 0.5) * r.cell_mm, -ev.z, u, v))
                for (int dy = 0; dy <= 1; ++dy)
                    for (int dx = 0; dx <= 1; ++dx)
                        blend(img, int(u) + dx, int(v) + dy, 60, 230, 90, 0.9);
        }
}

wxImage scaled_to(const wxImage& img, int max_w, int max_h)
{
    if (!img.IsOk())
        return img;
    const double s = std::min(double(max_w) / img.GetWidth(), double(max_h) / img.GetHeight());
    return img.Scale(std::max(1, int(img.GetWidth() * s)), std::max(1, int(img.GetHeight() * s)), wxIMAGE_QUALITY_HIGH);
}

// ---- checkpoint window --------------------------------------------------------------------------

class FailureCheckWindow : public wxDialog
{
public:
    explicit FailureCheckWindow(const FailureCheckEvent& ev)
        : wxDialog(wxGetApp().mainframe, wxID_ANY, _L("Print failure check"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP | wxRESIZE_BORDER)
        , m_ev(ev)
    {
        auto* top = new wxBoxSizer(wxVERTICAL);
        m_title   = new wxStaticText(this, wxID_ANY, "");
        m_title->SetFont(m_title->GetFont().Bold().Larger());
        top->Add(m_title, 0, wxALL, FromDIP(10));

        auto*   pics = new wxBoxSizer(wxHORIZONTAL);
        wxImage photo;
        if (photo.LoadFile(from_u8(ev.photo_path), wxBITMAP_TYPE_ANY) && photo.IsOk()) {
            if (ev.has_result)
                draw_overlay(photo, ev);
            auto* col = new wxBoxSizer(wxVERTICAL);
            col->Add(new wxStaticText(this, wxID_ANY, _L("Camera")), 0, wxBOTTOM, FromDIP(4));
            col->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(scaled_to(photo, FromDIP(560), FromDIP(420)))));
            pics->Add(col, 0, wxRIGHT, FromDIP(10));
        }
        if (ev.model) {
            const wxImage expected = to_wx(FailureCheck::render_expected_topdown(*ev.model, ev.segments_done, 1.0));
            if (expected.IsOk()) {
                auto* col = new wxBoxSizer(wxVERTICAL);
                col->Add(new wxStaticText(this, wxID_ANY, _L("What the G-code has printed (top view, front at the bottom)")), 0, wxBOTTOM,
                         FromDIP(4));
                col->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(scaled_to(expected, FromDIP(320), FromDIP(320)))));
                pics->Add(col, 0);
            }
        }
        top->Add(pics, 0, wxLEFT | wxRIGHT, FromDIP(10));

        wxString text;
        if (ev.has_result) {
            for (const std::string& r : ev.result.reasons)
                text += wxString::FromUTF8("• ") + from_u8(r) + "\n";
            text += _L("Red: material the G-code does not explain. Orange: expected material the camera does not see. "
                       "Green: outline of the printed material, lined up with the photo.");
        }
        m_text = new wxStaticText(this, wxID_ANY, text);
        m_text->Wrap(FromDIP(880));
        top->Add(m_text, 0, wxALL, FromDIP(10));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_countdown_label = new wxStaticText(this, wxID_ANY, "");
        buttons->Add(m_countdown_label, 1, wxALIGN_CENTER_VERTICAL);
        auto add = [&](const wxString& label, std::function<void()> fn) {
            auto* b = new wxButton(this, wxID_ANY, label);
            b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
            buttons->Add(b, 0, wxLEFT, FromDIP(8));
            return b;
        };
        m_resume_now = add(_L("Resume now"), [this]() { send_and_close("SHOP_CHECK_OK"); });
        m_keep       = add(_L("Keep paused"), [this]() {
            m_timer.Stop();
            shop_send_script(m_ev.api, "SHOP_CHECK_HOLD");
            set_mode(FailureCheckEvent::Mode::Hold, true);
        });
        m_resume     = add(_L("Resume print"), [this]() {
            notify_outcome(_u8L("Print resumed: %1%"), _u8L("Resumed from the slicer after checkpoint \"%1%\"."), "arrow_forward");
            send_and_close("SHOP_CHECK_OK");
        });
        m_cancel     = add(_L("Cancel print..."), [this]() {
            MessageDialog dlg(this, _L("Cancel the print on the printer?"), _L("Print failure check"), wxICON_WARNING | wxYES_NO);
            if (dlg.ShowModal() != wxID_YES)
                return;
            notify_outcome(_u8L("Print cancelled: %1%"), _u8L("Cancelled from the slicer after checkpoint \"%1%\"."), "x");
            send_and_close("CANCEL_PRINT");
        });
        m_pause      = add(_L("Pause the print"), [this]() { send_and_close("PAUSE"); });
        m_close      = add(_L("Close"), [this]() { Destroy(); });
        top->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));
        SetSizer(top);

        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (--m_seconds <= 0) {
                send_and_close("SHOP_CHECK_OK");
                return;
            }
            update_countdown();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            // Closing a review lets the print go on; closing a hold leaves it paused.
            if (m_mode == FailureCheckEvent::Mode::Review)
                shop_send_script(m_ev.api, "SHOP_CHECK_OK");
            Destroy();
        });
        set_mode(ev.mode, false);
        top->SetSizeHints(this);
        Fit();
        CentreOnParent();
    }

private:
    void set_mode(FailureCheckEvent::Mode mode, bool held_by_user)
    {
        m_mode = mode;
        const wxString where = format_wxstr(_L("%1%: checkpoint \"%2%\""), from_u8(m_ev.job_name), from_u8(m_ev.label));
        switch (mode) {
        case FailureCheckEvent::Mode::Hold:
            m_title->SetLabel((held_by_user ? _L("Paused at your request") : _L("Possible print failure: the printer is paused")) + "\n" + where);
            m_title->SetForegroundColour(wxColour(200, 40, 40));
            m_countdown_label->SetLabel(_L("The hotend turns off after 15 minutes paused; Resume reheats it."));
            break;
        case FailureCheckEvent::Mode::Notice:
            m_title->SetLabel(_L("Please check this print (it is still running)") + "\n" + where);
            m_title->SetForegroundColour(wxColour(200, 120, 0));
            m_countdown_label->SetLabel("");
            break;
        case FailureCheckEvent::Mode::Review:
            m_title->SetLabel(_L("Checkpoint photo for review (the camera is not calibrated yet)") + "\n" + where);
            m_seconds = 25;
            update_countdown();
            m_timer.Start(1000);
            break;
        }
        m_resume_now->Show(mode == FailureCheckEvent::Mode::Review);
        m_keep->Show(mode == FailureCheckEvent::Mode::Review);
        m_resume->Show(mode == FailureCheckEvent::Mode::Hold);
        m_cancel->Show(mode == FailureCheckEvent::Mode::Hold);
        m_pause->Show(mode == FailureCheckEvent::Mode::Notice);
        m_close->Show(mode != FailureCheckEvent::Mode::Review);
        Layout();
    }

    void update_countdown() { m_countdown_label->SetLabel(format_wxstr(_L("The print resumes in %1% s."), m_seconds)); }

    // Follows up the phone notification sent when the check held the print (not a user's Keep paused).
    void notify_outcome(const std::string& title, const std::string& body, const char* tags)
    {
        if (m_ev.mode != FailureCheckEvent::Mode::Hold)
            return;
        ShopNotify::Message msg;
        msg.title = (boost::format(title) % m_ev.job_name).str();
        msg.body  = (boost::format(body) % m_ev.label).str();
        msg.tags  = tags;
        ShopNotify::send(msg);
    }

    void send_and_close(const std::string& script)
    {
        m_timer.Stop();
        shop_send_script(m_ev.api, script);
        m_mode = FailureCheckEvent::Mode::Hold; // nothing more to send on close
        Destroy();
    }

    FailureCheckEvent       m_ev;
    FailureCheckEvent::Mode m_mode{FailureCheckEvent::Mode::Review};
    wxStaticText*           m_title{nullptr};
    wxStaticText*           m_text{nullptr};
    wxStaticText*           m_countdown_label{nullptr};
    wxButton *m_resume_now{nullptr}, *m_keep{nullptr}, *m_resume{nullptr}, *m_cancel{nullptr}, *m_pause{nullptr}, *m_close{nullptr};
    wxTimer                 m_timer;
    int                     m_seconds{25};
};

wxWeakRef<FailureCheckWindow> s_window;

// ---- calibration --------------------------------------------------------------------------------

// Shows a photo scaled to fit, reports clicks in image pixels and shows a magnifier under the mouse.
class ClickImagePanel : public wxPanel
{
public:
    struct Mark
    {
        double   u, v;
        wxColour colour;
    };
    std::function<void(double, double)> on_click;
    std::vector<Mark>                   marks;
    std::vector<std::vector<wxPoint2DDouble>> polylines; // image pixels

    explicit ClickImagePanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(640, 480)));
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) { paint(); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { m_scaled = wxBitmap(); Refresh(); e.Skip(); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& e) { m_mouse = e.GetPosition(); m_mouse_in = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_mouse_in = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
            double u, v;
            if (on_click && to_image(e.GetPosition(), u, v))
                on_click(u, v);
        });
    }
    void set_image(const wxImage& img)
    {
        m_image  = img;
        m_scaled = wxBitmap();
        marks.clear();
        polylines.clear();
        Refresh();
    }
    const wxImage& image() const { return m_image; }

private:
    void layout_rect()
    {
        const wxSize sz = GetClientSize();
        if (!m_image.IsOk() || sz.x <= 0 || sz.y <= 0)
            return;
        m_scale = std::min(double(sz.x) / m_image.GetWidth(), double(sz.y) / m_image.GetHeight());
        m_off   = wxPoint(int((sz.x - m_image.GetWidth() * m_scale) / 2), int((sz.y - m_image.GetHeight() * m_scale) / 2));
    }
    bool to_image(const wxPoint& p, double& u, double& v)
    {
        layout_rect();
        if (!m_image.IsOk() || m_scale <= 0)
            return false;
        u = (p.x - m_off.x) / m_scale;
        v = (p.y - m_off.y) / m_scale;
        return u >= 0 && v >= 0 && u < m_image.GetWidth() && v < m_image.GetHeight();
    }
    wxPoint to_screen(double u, double v) const { return wxPoint(int(m_off.x + u * m_scale), int(m_off.y + v * m_scale)); }
    void    paint()
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(*wxBLACK_BRUSH);
        dc.Clear();
        if (!m_image.IsOk())
            return;
        layout_rect();
        if (!m_scaled.IsOk())
            m_scaled = wxBitmap(m_image.Scale(std::max(1, int(m_image.GetWidth() * m_scale)), std::max(1, int(m_image.GetHeight() * m_scale)),
                                              wxIMAGE_QUALITY_HIGH));
        dc.DrawBitmap(m_scaled, m_off);
        for (const auto& line : polylines) {
            dc.SetPen(wxPen(wxColour(60, 230, 90), 2));
            for (size_t i = 1; i < line.size(); ++i)
                dc.DrawLine(to_screen(line[i - 1].m_x, line[i - 1].m_y), to_screen(line[i].m_x, line[i].m_y));
        }
        for (const Mark& m : marks) {
            const wxPoint p = to_screen(m.u, m.v);
            dc.SetPen(wxPen(m.colour, 2));
            dc.DrawLine(p.x - 8, p.y, p.x + 8, p.y);
            dc.DrawLine(p.x, p.y - 8, p.x, p.y + 8);
        }
        double u, v;
        if (m_mouse_in && to_image(m_mouse, u, v)) {
            // Magnifier: 4x around the mouse, top-right corner.
            const int r = 24, zoom = 4;
            const int x0 = std::clamp(int(u) - r, 0, std::max(0, m_image.GetWidth() - 2 * r));
            const int y0 = std::clamp(int(v) - r, 0, std::max(0, m_image.GetHeight() - 2 * r));
            const wxImage sub = m_image.GetSubImage(wxRect(x0, y0, std::min(2 * r, m_image.GetWidth()), std::min(2 * r, m_image.GetHeight())))
                                    .Scale(2 * r * zoom, 2 * r * zoom, wxIMAGE_QUALITY_NORMAL);
            const wxPoint at(GetClientSize().x - 2 * r * zoom - 8, 8);
            dc.DrawBitmap(wxBitmap(sub), at);
            dc.SetPen(wxPen(*wxWHITE, 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(at, wxSize(2 * r * zoom, 2 * r * zoom));
            const wxPoint c(at.x + int((u - x0) * zoom), at.y + int((v - y0) * zoom));
            dc.SetPen(wxPen(wxColour(255, 60, 60), 1));
            dc.DrawLine(c.x - 12, c.y, c.x + 12, c.y);
            dc.DrawLine(c.x, c.y - 12, c.x, c.y + 12);
        }
    }

    wxImage  m_image;
    wxBitmap m_scaled;
    double   m_scale{1};
    wxPoint  m_off;
    wxPoint  m_mouse;
    bool     m_mouse_in{false};
};

class CameraCalibrationDialog : public wxDialog
{
public:
    CameraCalibrationDialog(wxWindow* parent, const std::string& print_host, double bx0, double by0, double bx1, double by1)
        : wxDialog(parent, wxID_ANY, _L("Camera calibration"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_print_host(print_host)
        , m_host(shop_printer_host(print_host))
    {
        // Plate corners (first guess: the printable area; refined by the calibration), photographed at two
        // bed heights with the toolhead parked; then the nozzle tip at a 3 x 3 grid.
        const wxString corner_names[4] = {_L("front-left plate corner"), _L("front-right plate corner"), _L("back-right plate corner"),
                                          _L("back-left plate corner")};
        const double   cx[4] = {bx0, bx1, bx1, bx0}, cy[4] = {by0, by0, by1, by1};
        for (double z : {10.0, 170.0}) {
            Pose p{95, 324, z, {}};
            for (int c = 0; c < 4; ++c)
                p.targets.push_back({corner_names[c], c, cx[c], cy[c], false});
            m_poses.push_back(p);
        }
        for (double fy : {0.0, 0.5, 1.0})
            for (double fx : {0.0, 0.5, 1.0}) {
                const double x = bx0 + 20 + (bx1 - bx0 - 40) * fx, y = by0 + 20 + (by1 - by0 - 40) * fy;
                m_poses.push_back({x, y, 40, {{_L("nozzle tip"), -1, x, y, true}}});
            }

        auto* top = new wxBoxSizer(wxVERTICAL);
        m_step    = new wxStaticText(this, wxID_ANY, _L("Teach the slicer how the camera sees the bed"));
        m_step->SetFont(m_step->GetFont().Bold().Larger());
        top->Add(m_step, 0, wxALL, FromDIP(10));
        m_help = new wxStaticText(this, wxID_ANY,
                                  _L("The printer must be idle with an empty, clean plate. It will home, then move the bed and the "
                                     "toolhead to 11 positions. In each photo, click the point named above the photo (the "
                                     "magnifier in the corner helps); use Skip when it is out of view. This takes about five minutes."));
        m_help->Wrap(FromDIP(640));
        top->Add(m_help, 0, wxLEFT | wxRIGHT, FromDIP(10));
        m_panel = new ClickImagePanel(this);
        m_panel->on_click = [this](double u, double v) { on_click(u, v); };
        top->Add(m_panel, 1, wxEXPAND | wxALL, FromDIP(10));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_status      = new wxStaticText(this, wxID_ANY, "");
        buttons->Add(m_status, 1, wxALIGN_CENTER_VERTICAL);
        m_start = new wxButton(this, wxID_ANY, _L("Start"));
        m_skip  = new wxButton(this, wxID_ANY, _L("Skip (not visible)"));
        m_undo  = new wxButton(this, wxID_ANY, _L("Undo"));
        m_save  = new wxButton(this, wxID_ANY, _L("Save"));
        auto* close = new wxButton(this, wxID_CANCEL, _L("Close"));
        for (wxButton* b : {m_start, m_skip, m_undo, m_save, close})
            buttons->Add(b, 0, wxLEFT, FromDIP(8));
        top->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));
        SetSizer(top);
        m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start(); });
        m_skip->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            ++m_target;
            prompt();
        });
        m_undo->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { undo(); });
        m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save(); });
        update_buttons();
        top->SetSizeHints(this);
        Fit();
        CentreOnParent();
    }

private:
    struct Target
    {
        wxString label;
        int      group; // plate corner index, -1 for the nozzle
        double   x, y;
        bool     nozzle;
    };
    struct Pose
    {
        double              x, y, z;
        std::vector<Target> targets;
    };
    struct Clicked
    {
        size_t                       pose, target;
        FailureCheck::Correspondence pt;
    };

    void update_buttons()
    {
        const bool clicking = m_running && !m_busy && m_pose < m_poses.size();
        m_start->Enable(!m_running && !m_busy);
        m_start->SetLabel(m_solved ? _L("Start over") : _L("Start"));
        m_skip->Enable(clicking);
        m_undo->Enable(clicking && !m_clicked.empty() && m_clicked.back().pose == m_pose);
        m_save->Enable(m_solved);
        Layout();
    }

    void start()
    {
        wxBusyCursor wait;
        m_api = shop_resolve_api(m_print_host);
        if (m_api.empty()) {
            MessageDialog(this, _L("Could not reach the printer's Moonraker API."), _L("Camera calibration"), wxICON_WARNING | wxOK).ShowModal();
            return;
        }
        std::string body;
        bool        ok = false;
        Http::get(m_api + "/printer/objects/query?print_stats=state&" + Http::url_encode("gcode_macro SHOP_CAMERA_POSE"))
            .timeout_connect(4)
            .timeout_max(8)
            .on_complete([&](std::string b, unsigned status) { if (status == 200) { body = std::move(b); ok = true; } })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
        const auto j = nlohmann::json::parse(body, nullptr, false);
        std::string state;
        bool        has_macro = false;
        if (ok && !j.is_discarded()) {
            try {
                state     = j["result"]["status"]["print_stats"]["state"].get<std::string>();
                has_macro = j["result"]["status"].contains("gcode_macro SHOP_CAMERA_POSE");
            } catch (...) {}
        }
        if (!has_macro) {
            MessageDialog(this, _L("The printer does not have the SHOP_CAMERA_POSE macro (docs/shop/plus4/failure_check.cfg)."),
                          _L("Camera calibration"), wxICON_WARNING | wxOK).ShowModal();
            return;
        }
        if (state == "printing" || state == "paused") {
            MessageDialog(this, _L("The printer is busy. Calibrate the camera when it is idle."), _L("Camera calibration"),
                          wxICON_INFORMATION | wxOK).ShowModal();
            return;
        }
        m_clicked.clear();
        m_photos.assign(m_poses.size(), wxImage());
        m_actual.assign(m_poses.size(), {0, 0, 0});
        m_solved  = false;
        m_running = true;
        go_to_pose(0);
    }

    void go_to_pose(size_t i)
    {
        m_pose   = i;
        m_target = 0;
        if (i >= m_poses.size()) {
            finish();
            return;
        }
        m_busy = true;
        m_step->SetLabel(format_wxstr(_L("Position %1% of %2%: moving the printer..."), i + 1, m_poses.size()));
        m_status->SetLabel("");
        update_buttons();
        const Pose        p      = m_poses[i];
        const std::string api    = m_api;
        const std::string snap   = shop_snapshot_url(m_print_host);
        const std::string script = (boost::format("SHOP_CAMERA_POSE X=%.1f Y=%.1f Z=%.1f") % p.x % p.y % p.z).str();
        wxWeakRef<CameraCalibrationDialog> self(this);
        std::thread([self, api, snap, script, i]() {
            bool        moved = false;
            std::string error, jpeg, pos_body;
            Http::post(api + "/printer/gcode/script")
        .header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"script", script}}.dump())
                .timeout_connect(5)
                .timeout_max(300)
                .on_complete([&](std::string, unsigned status) { moved = status == 200; })
                .on_error([&](std::string body, std::string err, unsigned) { error = err + " " + body; })
                .perform_sync();
            if (moved) {
                Http::get(api + "/printer/objects/query?toolhead=position").timeout_connect(4).timeout_max(8)
                    .on_complete([&](std::string b, unsigned) { pos_body = std::move(b); }).on_error([](std::string, std::string, unsigned) {}).perform_sync();
                Http::get(snap).timeout_connect(4).timeout_max(10)
                    .on_complete([&](std::string b, unsigned status) { if (status == 200) jpeg = std::move(b); })
                    .on_error([](std::string, std::string, unsigned) {}).perform_sync();
            }
            wxGetApp().CallAfter([self, i, moved, error, jpeg, pos_body]() {
                if (self)
                    self->on_pose_ready(i, moved, error, jpeg, pos_body);
            });
        }).detach();
    }

    void on_pose_ready(size_t i, bool moved, const std::string& error, const std::string& jpeg, const std::string& pos_body)
    {
        m_busy = false;
        FailureCheck::Image img;
        if (!moved || !shop_decode_image(jpeg, img)) {
            m_running = false;
            m_step->SetLabel(_L("The printer did not move or the camera did not answer."));
            m_status->SetLabel(from_u8(error));
            update_buttons();
            return;
        }
        std::array<double, 3> pos{m_poses[i].x, m_poses[i].y, m_poses[i].z};
        const auto j = nlohmann::json::parse(pos_body, nullptr, false);
        if (!j.is_discarded())
            try {
                const auto& p = j["result"]["status"]["toolhead"]["position"];
                pos = {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
            } catch (...) {}
        m_actual[i] = pos;
        m_photos[i] = to_wx(img);
        m_panel->set_image(m_photos[i]);
        prompt();
    }

    void prompt()
    {
        if (m_pose >= m_poses.size())
            return;
        const Pose& p = m_poses[m_pose];
        if (m_target >= p.targets.size()) {
            go_to_pose(m_pose + 1);
            return;
        }
        m_step->SetLabel(format_wxstr(_L("Position %1% of %2%: click the %3%"), m_pose + 1, m_poses.size(), p.targets[m_target].label));
        m_panel->marks.clear();
        for (const Clicked& c : m_clicked)
            if (c.pose == m_pose)
                m_panel->marks.push_back({c.pt.u, c.pt.v, wxColour(255, 60, 60)});
        m_panel->Refresh();
        update_buttons();
    }

    void on_click(double u, double v)
    {
        if (!m_running || m_busy || m_pose >= m_poses.size())
            return;
        const Pose&   p = m_poses[m_pose];
        const Target& t = p.targets[m_target];
        const auto&   a = m_actual[m_pose];
        FailureCheck::Correspondence c{t.nozzle ? a[0] : t.x, t.nozzle ? a[1] : t.y, t.nozzle ? 0.0 : -a[2], u, v};
        c.group = t.group;
        m_clicked.push_back({m_pose, m_target, c});
        ++m_target;
        prompt();
    }

    void undo()
    {
        if (m_clicked.empty() || m_clicked.back().pose != m_pose)
            return;
        m_target = m_clicked.back().target;
        m_clicked.pop_back();
        prompt();
    }

    void finish()
    {
        m_running = false;
        std::vector<FailureCheck::Correspondence> pts;
        for (const Clicked& c : m_clicked)
            pts.push_back(c.pt);
        const wxImage& any = m_photos.empty() ? wxImage() : m_photos.front();
        std::string    err;
        if (!any.IsOk() || !FailureCheck::calibrate_camera(pts, any.GetWidth(), any.GetHeight(), m_result, err)) {
            m_step->SetLabel(_L("Calibration failed"));
            m_status->SetLabel(from_u8(err) + " - " + _L("click more points and try again."));
            update_buttons();
            return;
        }
        m_solved = true;
        // Check: the printable-area outline at the first bed height, and the nozzle positions.
        m_panel->set_image(m_photos.front());
        const auto& b = m_poses.front().targets;
        std::vector<wxPoint2DDouble> outline;
        for (int k = 0; k <= 40; ++k) {
            const int    side = k / 10;
            const double t    = (k % 10) / 10.0;
            const auto&  p0   = b[side % 4];
            const auto&  p1   = b[(side + 1) % 4];
            double       u, v;
            if (m_result.project(p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t, -m_actual.front()[2], u, v))
                outline.push_back({u, v});
        }
        m_panel->polylines.push_back(outline);
        for (size_t i = 2; i < m_poses.size(); ++i) {
            double u, v;
            if (m_result.project(m_actual[i][0], m_actual[i][1], 0, u, v))
                m_panel->marks.push_back({u, v, wxColour(60, 230, 90)});
        }
        m_panel->Refresh();
        const wxString quality = m_result.rms_px < 3 ? _L("good") : m_result.rms_px < 6 ? _L("usable") : _L("poor - please start over");
        m_step->SetLabel(format_wxstr(_L("Calibrated from %1% points: average error %2% px (%3%)"), m_result.points,
                                      wxString::Format("%.1f", m_result.rms_px), quality));
        m_status->SetLabel(_L("Green: the printable area and the nozzle positions as the calibration predicts them."));
        update_buttons();
    }

    void save()
    {
        m_result.park_x = 95;
        m_result.park_y = 324;
        const std::time_t t = std::time(nullptr);
        char              buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
        m_result.when = buf;
        if (save_shop_camera(m_host, m_result)) {
            MessageDialog(this, _L("Camera calibration saved. Failure checks on this printer now compare photos automatically."),
                          _L("Camera calibration"), wxICON_INFORMATION | wxOK).ShowModal();
            EndModal(wxID_OK);
        } else {
            MessageDialog(this, _L("Could not save the calibration."), _L("Camera calibration"), wxICON_WARNING | wxOK).ShowModal();
        }
    }

    std::string                         m_print_host, m_host, m_api;
    std::vector<Pose>                   m_poses;
    std::vector<wxImage>                m_photos;
    std::vector<std::array<double, 3>>  m_actual;
    std::vector<Clicked>                m_clicked;
    size_t                              m_pose{0}, m_target{0};
    bool                                m_running{false}, m_busy{false}, m_solved{false};
    FailureCheck::CameraModel           m_result;
    ClickImagePanel*                    m_panel{nullptr};
    wxStaticText *                      m_step{nullptr}, *m_help{nullptr}, *m_status{nullptr};
    wxButton *                          m_start{nullptr}, *m_skip{nullptr}, *m_undo{nullptr}, *m_save{nullptr};
};

} // namespace

void show_failure_check_event(const FailureCheckEvent& ev)
{
    try {
        if (s_window)
            s_window->Destroy();
        auto* w  = new FailureCheckWindow(ev);
        s_window = w;
        w->Show();
        w->Raise();
        w->RequestUserAttention();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Shop failure check: cannot show the checkpoint window: " << e.what();
    }
}

void show_camera_calibration(wxWindow* parent, const std::string& print_host)
{
    double x0 = 0, y0 = 0, x1 = 305, y1 = 305;
    if (auto* bundle = wxGetApp().preset_bundle) {
        if (auto* area = bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("printable_area");
            area && area->values.size() >= 3) {
            x0 = y0 = 1e9;
            x1 = y1 = -1e9;
            for (const auto& p : area->values) {
                x0 = std::min(x0, p.x()); x1 = std::max(x1, p.x());
                y0 = std::min(y0, p.y()); y1 = std::max(y1, p.y());
            }
        }
    }
    CameraCalibrationDialog dlg(parent, print_host, x0, y0, x1, y1);
    dlg.ShowModal();
}

}} // namespace Slic3r::GUI
