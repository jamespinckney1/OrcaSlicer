#include "QidiBoxDialog.hpp"

#include "GUI_App.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI.hpp"
#include "format.hpp"
#include "DeviceCore/DevFilaSystem.h"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/colour.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI {

namespace {

wxColour tray_colour(const std::string& raw)
{
    std::string hex = raw;
    if (!hex.empty() && hex[0] != '#')
        hex = "#" + hex;
    if (hex.size() > 7)
        hex = hex.substr(0, 7); // drop alpha
    wxColour c(hex);
    return c.IsOk() ? c : wxColour(200, 200, 200);
}

} // namespace

QidiBoxDialog::QidiBoxDialog(wxWindow* parent, MachineObject* obj)
    : DPIDialog(parent, wxID_ANY, _L("QIDI Box"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_obj(obj)
{
    SetBackgroundColour(*wxWHITE);
    auto* main = new wxBoxSizer(wxVERTICAL);

    auto* hint = new Label(this, _L("Slots as reported by the printer. Load and unload move filament through the extruder; "
                                    "the printer heats the nozzle itself. Eject pushes the spool out of the Box."));
    hint->SetFont(Label::Body_12);
    hint->Wrap(FromDIP(520));
    main->Add(hint, 0, wxALL, FromDIP(12));

    m_rows_panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(560), FromDIP(240)));
    m_rows_panel->SetScrollRate(0, FromDIP(10));
    m_rows_panel->SetBackgroundColour(*wxWHITE);
    m_rows_sizer = new wxBoxSizer(wxVERTICAL);
    m_rows_panel->SetSizer(m_rows_sizer);
    main->Add(m_rows_panel, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_status = new Label(this, "");
    m_status->SetFont(Label::Body_12);
    main->Add(m_status, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto* refresh_btn = new Button(this, _L("Refresh from printer"));
    refresh_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { this->refresh(true); });
    btns->Add(refresh_btn, 0, wxRIGHT, FromDIP(8));
    btns->AddStretchSpacer();
    auto* dlg_btns = new DialogButtons(this, {"OK"});
    dlg_btns->GetOK()->SetLabel(_L("Close"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    btns->Add(dlg_btns, 0);
    main->Add(btns, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizer(main);
    refresh(false);
    Layout();
    Fit();
    SetMinSize(GetSize());
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void QidiBoxDialog::on_dpi_changed(const wxRect&)
{
    Layout();
    Fit();
}

void QidiBoxDialog::refresh(bool fetch)
{
    if (!m_obj) {
        m_status->SetLabel(_L("No printer is connected. Connect the printer on the Device tab first."));
        return;
    }
    if (fetch) {
        auto* agent = wxGetApp().getDeviceManager() ? wxGetApp().getDeviceManager()->get_agent() : nullptr;
        if (agent && agent->get_filament_sync_mode() == FilamentSyncMode::pull) {
            bool fetched = false;
            {
                wxBusyCursor wait;
                fetched = agent->fetch_filament_info(m_obj->get_dev_id());
            }
            if (!fetched)
                m_status->SetLabel(_L("The printer did not report a Box. Check the connection and that the Box is enabled."));
            else
                m_status->SetLabel(_L("Slots refreshed."));
        } else
            m_status->SetLabel(_L("This connection does not fetch the Box on demand; showing the slots as last reported."));
    }
    rebuild_rows();
}

void QidiBoxDialog::rebuild_rows()
{
    m_rows_sizer->Clear(true);
    auto fila = m_obj ? m_obj->GetFilaSystem() : nullptr;
    int shown = 0;
    if (fila) {
        for (const auto& [ams_id, ams] : fila->GetAmsList()) {
            if (!ams)
                continue;
            int ams_index = 0;
            try { ams_index = std::stoi(ams_id); } catch (...) {}
            for (const auto& [tray_id, tray] : ams->GetTrays()) {
                if (!tray)
                    continue;
                int slot_in_ams = 0;
                try { slot_in_ams = std::stoi(tray_id); } catch (...) {}
                const int         slot  = ams_index * 4 + slot_in_ams;
                const std::string sname = "slot" + std::to_string(slot);

                auto* row = new wxBoxSizer(wxHORIZONTAL);
                auto* swatch = new wxPanel(m_rows_panel, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(18, 18)));
                swatch->SetBackgroundColour(tray->is_exists ? tray_colour(tray->color) : wxColour(235, 235, 235));
                row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

                wxString text = wxString::Format(_L("Slot %d"), slot + 1) + ": ";
                text += tray->is_exists ? from_u8(tray->m_fila_type.empty() ? std::string("filament") : tray->m_fila_type) : _L("empty");
                auto* label = new Label(m_rows_panel, text);
                label->SetFont(Label::Body_13);
                label->SetMinSize(FromDIP(wxSize(170, -1)));
                row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

                auto add_btn = [&](const wxString& caption, const std::string& gcode, bool confirm, bool enabled) {
                    auto* b = new Button(m_rows_panel, caption);
                    b->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
                    b->Enable(enabled);
                    b->Bind(wxEVT_BUTTON, [this, caption, gcode, confirm](wxCommandEvent&) { send(caption, gcode, confirm); });
                    row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                };
                add_btn(_L("Load"),      "EXTRUDER_LOAD SLOT=" + sname,          true,  tray->is_exists);
                add_btn(_L("Unload"),    "EXTRUDER_UNLOAD SLOT=" + sname,        true,  tray->is_exists);
                add_btn(_L("Eject"),     "MULTI_COLOR_BOX_UNLOAD SLOT=" + sname, true,  tray->is_exists);
                add_btn(_L("Read RFID"), "MULTI_COLOR_READ_RFID SLOT=" + sname,  false, true);

                m_rows_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
                ++shown;
            }
        }
    }
    if (shown == 0) {
        auto* none = new Label(m_rows_panel, _L("No Box slots known yet. Click \"Refresh from printer\"."));
        none->SetFont(Label::Body_13);
        m_rows_sizer->Add(none, 0, wxALL, FromDIP(4));
    }
    m_rows_panel->Layout();
    m_rows_panel->FitInside();
    Layout();
}

void QidiBoxDialog::send(const wxString& what, const std::string& gcode, bool confirm)
{
    if (!m_obj)
        return;
    if (confirm) {
        MessageDialog dlg(this, format_wxstr(_L("%1% now?\n\nThe printer will run:\n%2%"), what, gcode),
                          _L("QIDI Box"), wxICON_QUESTION | wxYES_NO);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }
    const int rc = m_obj->publish_gcode(gcode);
    if (rc == 0)
        m_status->SetLabel(format_wxstr(_L("Sent: %1%"), gcode));
    else
        m_status->SetLabel(format_wxstr(_L("Could not send \"%1%\" (error %2%)."), gcode, rc));
    Layout();
}

}} // namespace Slic3r::GUI
