#include "CalibrationLogDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "MainFrame.hpp"
#include "Tab.hpp"
#include "ShopPrinters.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

namespace {
wxString fmt_val(double v, int decimals)
{
    if (v < 0)
        return "";
    return wxString::FromDouble(v, decimals);
}
double parse_val(const wxString& s)
{
    double v;
    wxString t = s; // FromDouble above formats in the C locale
    t.Trim(true).Trim(false);
    if (t.empty() || !t.ToCDouble(&v))
        return -1;
    return v;
}
wxString write_failed_msg() { return _L("Could not write the calibration log."); }
} // namespace

CalibrationLogDialog::CalibrationLogDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Shop calibration log"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    auto* main = new wxBoxSizer(wxVERTICAL);

    auto* hint = new Label(this, _L("Record what each calibration test found for a printer and filament, then apply it to the "
                                    "filament preset in one click. Values left blank are not applied."));
    hint->SetFont(Label::Body_12);
    hint->Wrap(FromDIP(720));
    main->Add(hint, 0, wxALL, FromDIP(12));

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(760, 220)), wxLC_REPORT | wxLC_SINGLE_SEL);
    const wxString cols[] = {_L("Date"), _L("Printer"), _L("Filament"), _L("Machine"), _L("PA"), _L("Flow"), _L("Max vol."), _L("Temp")};
    const int widths[]    = {90, 190, 200, 100, 60, 60, 70, 55};
    for (int i = 0; i < 8; ++i)
        m_list->InsertColumn(i, cols[i], wxLIST_FORMAT_LEFT, FromDIP(widths[i]));
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { load_fields(int(e.GetIndex())); });
    main->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    auto* grid = new wxFlexGridSizer(2, 8, FromDIP(6), FromDIP(8));
    auto add_field = [&](const wxString& caption, wxTextCtrl*& ctrl, int width) {
        auto* l = new Label(this, caption + ":");
        l->SetFont(Label::Body_12);
        ctrl = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(width, -1)));
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
    };
    add_field(_L("Printer preset"), m_printer, 230);
    add_field(_L("Filament preset"), m_filament, 230);
    add_field(_L("Machine"), m_machine, 150);
    add_field(_L("Pressure advance"), m_pa, 80);
    add_field(_L("Flow ratio"), m_flow, 80);
    add_field(_L("Max volumetric speed"), m_mvs, 80);
    add_field(_L("Temperature"), m_temp, 80);
    add_field(_L("Notes"), m_notes, 230);
    main->Add(grid, 0, wxALL, FromDIP(12));

    m_status = new Label(this, "");
    m_status->SetFont(Label::Body_12);
    main->Add(m_status, 0, wxLEFT | wxRIGHT, FromDIP(12));

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto mk = [&](const wxString& caption, std::function<void()> fn) {
        auto* b = new Button(this, caption);
        b->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        btns->Add(b, 0, wxRIGHT, FromDIP(8));
    };
    mk(_L("New from current presets"), [this]() { add_from_current(); });
    mk(_L("Save entry"), [this]() { save_fields(); });
    mk(_L("Apply to filament preset"), [this]() { apply_to_filament(); });
    mk(_L("Delete"), [this]() { remove_selected(); });
    btns->AddStretchSpacer();
    auto* dlg_btns = new DialogButtons(this, {"OK"});
    dlg_btns->GetOK()->SetLabel(_L("Close"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    btns->Add(dlg_btns, 0);
    main->Add(btns, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizer(main);
    fill_list();
    Layout();
    Fit();
    SetMinSize(GetSize());
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void CalibrationLogDialog::on_dpi_changed(const wxRect&) { Layout(); }

int CalibrationLogDialog::selected() const
{
    return int(m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED));
}

void CalibrationLogDialog::fill_list(int select)
{
    m_list->DeleteAllItems();
    long row = 0;
    for (const CalibResult& r : ShopCalibration::get().results()) {
        long idx = m_list->InsertItem(row++, from_u8(r.date));
        m_list->SetItem(idx, 1, from_u8(r.printer_preset));
        m_list->SetItem(idx, 2, from_u8(r.filament_preset));
        m_list->SetItem(idx, 3, from_u8(r.machine));
        m_list->SetItem(idx, 4, fmt_val(r.pressure_advance, 3));
        m_list->SetItem(idx, 5, fmt_val(r.flow_ratio, 3));
        m_list->SetItem(idx, 6, fmt_val(r.max_volumetric, 1));
        m_list->SetItem(idx, 7, r.temperature < 0 ? wxString("") : wxString::Format("%d", r.temperature));
    }
    if (select >= 0 && select < m_list->GetItemCount()) {
        m_list->SetItemState(select, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        load_fields(select);
    }
}

void CalibrationLogDialog::load_fields(int idx)
{
    const auto& results = ShopCalibration::get().results();
    if (idx < 0 || size_t(idx) >= results.size())
        return;
    const CalibResult& r = results[idx];
    m_printer->ChangeValue(from_u8(r.printer_preset));
    m_filament->ChangeValue(from_u8(r.filament_preset));
    m_machine->ChangeValue(from_u8(r.machine));
    m_pa->ChangeValue(fmt_val(r.pressure_advance, 3));
    m_flow->ChangeValue(fmt_val(r.flow_ratio, 3));
    m_mvs->ChangeValue(fmt_val(r.max_volumetric, 1));
    m_temp->ChangeValue(r.temperature < 0 ? wxString("") : wxString::Format("%d", r.temperature));
    m_notes->ChangeValue(from_u8(r.notes));
}

void CalibrationLogDialog::add_from_current()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    CalibResult r;
    r.printer_preset  = bundle.printers.get_selected_preset_name();
    r.filament_preset = bundle.filament_presets.empty() ? bundle.filaments.get_selected_preset_name() : bundle.filament_presets.front();
    for (const ShopPrinter& m : ShopPrinters::get().list())
        if (m.printer_preset == r.printer_preset) { r.machine = m.name; break; }
    // pre-fill with what the filament preset holds today, so the log shows the starting point
    if (const Preset* fil = bundle.filaments.find_preset(r.filament_preset, false)) {
        if (auto* pa = fil->config.option<ConfigOptionFloats>("pressure_advance"); pa && !pa->values.empty()) r.pressure_advance = pa->values.front();
        if (auto* fr = fil->config.option<ConfigOptionFloats>("filament_flow_ratio"); fr && !fr->values.empty()) r.flow_ratio = fr->values.front();
        if (auto* mv = fil->config.option<ConfigOptionFloats>("filament_max_volumetric_speed"); mv && !mv->values.empty()) r.max_volumetric = mv->values.front();
        if (auto* t = fil->config.option<ConfigOptionInts>("nozzle_temperature"); t && !t->values.empty()) r.temperature = t->values.front();
    }
    r.date = ShopCalibration::today();
    ShopCalibration::get().results().push_back(r);
    const bool saved = ShopCalibration::get().save();
    fill_list(int(ShopCalibration::get().results().size()) - 1);
    m_status->SetLabel(saved ? _L("New entry created from the current presets. Edit the values and click \"Save entry\".") :
                               write_failed_msg());
}

void CalibrationLogDialog::save_fields()
{
    const int idx = selected();
    auto& results = ShopCalibration::get().results();
    if (idx < 0 || size_t(idx) >= results.size()) {
        m_status->SetLabel(_L("Select an entry first, or create one with \"New from current presets\"."));
        return;
    }
    CalibResult& r = results[idx];
    r.printer_preset   = into_u8(m_printer->GetValue());
    r.filament_preset  = into_u8(m_filament->GetValue());
    r.machine          = into_u8(m_machine->GetValue());
    r.pressure_advance = parse_val(m_pa->GetValue());
    r.flow_ratio       = parse_val(m_flow->GetValue());
    r.max_volumetric   = parse_val(m_mvs->GetValue());
    const double t     = parse_val(m_temp->GetValue());
    r.temperature      = t < 0 ? -1 : int(t);
    r.notes            = into_u8(m_notes->GetValue());
    if (r.date.empty())
        r.date = ShopCalibration::today();
    const bool saved = ShopCalibration::get().save();
    fill_list(idx);
    m_status->SetLabel(saved ? _L("Entry saved.") : write_failed_msg());
}

void CalibrationLogDialog::apply_to_filament()
{
    const int idx = selected();
    const auto& results = ShopCalibration::get().results();
    if (idx < 0 || size_t(idx) >= results.size()) {
        m_status->SetLabel(_L("Select an entry to apply."));
        return;
    }
    const CalibResult r = results[idx]; // copy: selecting a preset below runs arbitrary GUI code
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
    if (!tab)
        return;
    const std::string current = bundle.filaments.get_selected_preset_name();
    if (!r.filament_preset.empty() && current != r.filament_preset) {
        if (!bundle.filaments.find_preset(r.filament_preset, false)) {
            m_status->SetLabel(format_wxstr(_L("Filament preset \"%1%\" no longer exists."), r.filament_preset));
            return;
        }
        if (!tab->select_preset(r.filament_preset) || bundle.filaments.get_selected_preset_name() != r.filament_preset) {
            m_status->SetLabel(format_wxstr(_L("Could not switch to filament preset \"%1%\"; nothing was applied."), r.filament_preset));
            return;
        }
    }
    DynamicPrintConfig new_conf = *tab->get_config();
    auto set_float = [&](const char* key, double v) {
        if (v < 0) return;
        auto* opt = new_conf.option<ConfigOptionFloats>(key, true);
        if (opt->values.empty()) opt->values.push_back(v); else opt->values[0] = v;
    };
    set_float("pressure_advance", r.pressure_advance);
    if (r.pressure_advance >= 0) {
        auto* en = new_conf.option<ConfigOptionBools>("enable_pressure_advance", true);
        if (en->values.empty()) en->values.push_back(true); else en->values[0] = true;
    }
    set_float("filament_flow_ratio", r.flow_ratio);
    set_float("filament_max_volumetric_speed", r.max_volumetric);
    if (r.temperature >= 0) {
        auto* t = new_conf.option<ConfigOptionInts>("nozzle_temperature", true);
        if (t->values.empty()) t->values.push_back(r.temperature); else t->values[0] = r.temperature;
    }
    tab->load_config(new_conf);
    tab->update_dirty();
    tab->update();
    m_status->SetLabel(_L("Applied to the filament preset (unsaved). Open the Filament settings tab and save the preset to keep it."));
}

void CalibrationLogDialog::remove_selected()
{
    const int idx = selected();
    auto& results = ShopCalibration::get().results();
    if (idx < 0 || size_t(idx) >= results.size())
        return;
    MessageDialog confirm(this, _L("Delete this calibration entry?"), _L("Shop calibration log"), wxICON_WARNING | wxYES_NO);
    if (confirm.ShowModal() != wxID_YES)
        return;
    results.erase(results.begin() + idx);
    const bool saved = ShopCalibration::get().save();
    fill_list();
    m_status->SetLabel(saved ? _L("Entry deleted.") : write_failed_msg());
}

}} // namespace Slic3r::GUI
