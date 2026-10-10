#include "GCodeEditDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <sstream>
#include <wx/sizer.h>
#include <wx/font.h>

namespace Slic3r { namespace GUI {

GCodeEditDialog::GCodeEditDialog(wxWindow* parent, const std::string& gcode_path)
    : DPIDialog(parent, wxID_ANY, _L("Edit sliced G-code"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
    , m_path(gcode_path)
{
    SetBackgroundColour(*wxWHITE);
    auto* main = new wxBoxSizer(wxVERTICAL);

    // toolbar: find + go to layer
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto* find_label = new Label(this, _L("Find") + ":");
    find_label->SetFont(Label::Body_12);
    m_find = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(220, -1)), wxTE_PROCESS_ENTER);
    m_find->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { find_next(); });
    auto* find_btn = new Button(this, _L("Next"));
    find_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    find_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { find_next(); });
    auto* layer_label = new Label(this, _L("Layer") + ":");
    layer_label->SetFont(Label::Body_12);
    m_layer = new wxSpinCtrl(this, wxID_ANY, "1", wxDefaultPosition, FromDIP(wxSize(80, -1)), wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 1, 100000, 1);
    m_layer->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { go_to_layer(); });
    auto* layer_btn = new Button(this, _L("Go"));
    layer_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    layer_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { go_to_layer(); });
    const int gap = FromDIP(6);
    bar->Add(find_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    bar->Add(m_find, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    bar->Add(find_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(18));
    bar->Add(layer_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    bar->Add(m_layer, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    bar->Add(layer_btn, 0, wxALIGN_CENTER_VERTICAL);
    main->Add(bar, 0, wxEXPAND | wxALL, FromDIP(10));

    // editor
    m_editor = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(900, 560)),
                              wxTE_MULTILINE | wxTE_RICH2 | wxTE_DONTWRAP | wxHSCROLL);
    wxFont mono(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE));
    m_editor->SetFont(mono);
    main->Add(m_editor, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    // status + buttons
    m_status = new Label(this, "");
    m_status->SetFont(Label::Body_12);
    main->Add(m_status, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto* dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Save and reload preview"));
    dlg_btns->GetCANCEL()->SetLabel(_L("Cancel"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (save())
            EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_editor->IsModified()) {
            MessageDialog confirm(this, _L("Discard the changes to the G-code?"), _L("Edit sliced G-code"), wxICON_QUESTION | wxYES_NO);
            if (confirm.ShowModal() != wxID_YES)
                return;
        }
        EndModal(wxID_CANCEL);
    });
    main->Add(dlg_btns, 0, wxEXPAND | wxALL, FromDIP(10));

    // load file
    {
        wxBusyCursor wait;
        boost::nowide::ifstream in(m_path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        m_editor->ChangeValue(from_u8(text));
        m_editor->DiscardEdits();
        size_t lines = 0;
        for (char c : text)
            if (c == '\n') ++lines;
        m_status->SetLabel(format_wxstr(_L("%1%  (%2% lines, %3% KB). Export G-code and Send to printer use this file."),
                                        from_u8(boost::filesystem::path(m_path).filename().string()), lines, text.size() / 1024));
    }

    SetSizer(main);
    Layout();
    Fit();
    SetMinSize(FromDIP(wxSize(640, 400)));
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void GCodeEditDialog::on_dpi_changed(const wxRect&)
{
    Layout();
}

void GCodeEditDialog::find_next()
{
    const wxString needle = m_find->GetValue();
    if (needle.empty())
        return;
    const wxString hay = m_editor->GetValue();
    long from = 0, to = 0;
    m_editor->GetSelection(&from, &to);
    long start = std::max<long>(to, m_last_find_pos);
    if (start >= (long) hay.length())
        start = 0;
    const wxString hay_l = hay.Lower(), needle_l = needle.Lower();
    size_t pos = hay_l.find(needle_l, size_t(start));
    if (pos == wxString::npos && start > 0)
        pos = hay_l.find(needle_l, 0); // wrap around
    if (pos == wxString::npos) {
        m_status->SetLabel(format_wxstr(_L("\"%1%\" not found."), needle));
        return;
    }
    m_editor->SetFocus();
    m_editor->ShowPosition(long(pos));
    m_editor->SetSelection(long(pos), long(pos + needle.length()));
    m_last_find_pos = long(pos + needle.length());
    m_status->SetLabel(format_wxstr(_L("Found \"%1%\" at character %2%."), needle, pos));
}

void GCodeEditDialog::go_to_layer()
{
    const int layer = m_layer->GetValue();
    const wxString hay = m_editor->GetValue();
    static const wxString marker = ";LAYER_CHANGE";
    size_t pos = 0;
    int count = 0;
    while (true) {
        pos = hay.find(marker, pos);
        if (pos == wxString::npos)
            break;
        ++count;
        if (count == layer)
            break;
        pos += marker.length();
    }
    if (pos == wxString::npos) {
        m_status->SetLabel(format_wxstr(_L("Layer %1% not found (%2% layer markers in file)."), layer, count));
        return;
    }
    m_editor->SetFocus();
    m_editor->ShowPosition(long(pos));
    m_editor->SetSelection(long(pos), long(pos + marker.length()));
    m_last_find_pos = long(pos + marker.length());
    m_status->SetLabel(format_wxstr(_L("Layer %1%."), layer));
}

bool GCodeEditDialog::save()
{
    try {
        const boost::filesystem::path p(m_path);
        const boost::filesystem::path backup = p.string() + ".orig";
        if (!boost::filesystem::exists(backup))
            boost::filesystem::copy_file(p, backup);
        boost::nowide::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        const std::string text = into_u8(m_editor->GetValue());
        out.write(text.data(), std::streamsize(text.size()));
        if (!out) {
            MessageDialog err(this, _L("Could not write the G-code file."), _L("Edit sliced G-code"), wxICON_ERROR | wxOK);
            err.ShowModal();
            return false;
        }
    } catch (const std::exception& e) {
        MessageDialog err(this, from_u8(e.what()), _L("Edit sliced G-code"), wxICON_ERROR | wxOK);
        err.ShowModal();
        return false;
    }
    m_saved = true;
    m_editor->DiscardEdits();
    return true;
}

}} // namespace Slic3r::GUI
