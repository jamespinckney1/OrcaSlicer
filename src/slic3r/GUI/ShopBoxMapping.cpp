#include "ShopBoxMapping.hpp"

#include "FailureCheckMonitor.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/bitmap.h>
#include <wx/bmpcbox.h>
#include <wx/button.h>
#include <wx/dc.h>
#include <wx/dcmemory.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cstdlib>

namespace Slic3r { namespace GUI { namespace ShopBox {

namespace {

constexpr const char* kSyncKey = "shop_box_sync"; // app_config: {"<filament index>": <slot>, ...}

struct Slot
{
    int         index = 0; // 0-based, as the printer names it (slot<index>)
    int         state = 0; // 0 empty, 1 ready, 2 loaded in the toolhead
    std::string colour;    // "#RRGGBB"
    std::string name;      // e.g. "PETG-Tough"
    std::string type;      // e.g. "PETG"
};

std::map<int, int> load_remembered()
{
    std::map<int, int> out;
    const auto j = nlohmann::json::parse(wxGetApp().app_config->get(kSyncKey), nullptr, false);
    if (j.is_object())
        for (auto it = j.begin(); it != j.end(); ++it)
            if (it.value().is_number_integer())
                out[std::atoi(it.key().c_str())] = it.value().get<int>();
    return out;
}

void save_remembered(const std::map<int, int>& m)
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [f, s] : m)
        j[std::to_string(f)] = s;
    wxGetApp().app_config->set(kSyncKey, j.dump());
}

int to_int(const nlohmann::json& v, int fallback = 0)
{
    if (v.is_number())
        return v.get<int>();
    if (v.is_string())
        try { return std::stoi(v.get<std::string>()); } catch (...) {}
    return fallback;
}

// The Box's slots and the printer state, or false when the printer has no Box (or does not answer).
bool fetch_box(const std::string& api, std::vector<Slot>& slots, std::string& state)
{
    std::string body;
    Http::get(api + "/printer/objects/query?multi_color_controller&print_stats=state")
        .timeout_connect(4)
        .timeout_max(8)
        .on_complete([&](std::string b, unsigned status) { if (status == 200) body = std::move(b); })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    try {
        const auto& st = j.at("result").at("status");
        state          = st.at("print_stats").value("state", std::string());
        if (!st.contains("multi_color_controller") || !st["multi_color_controller"].is_object())
            return false;
        const auto& mcc = st["multi_color_controller"];
        int boxes = 0;
        if (mcc.contains("hardware") && mcc["hardware"].contains("box_count"))
            boxes = to_int(mcc["hardware"]["box_count"]);
        if (boxes <= 0 || !mcc.contains("slots"))
            return false;
        const auto& sl     = mcc["slots"];
        const auto  states = sl.value("states", nlohmann::json::object());
        const auto  mats   = sl.value("materials", nlohmann::json::object());
        for (int i = 0; i < std::min(boxes * 4, 16); ++i) {
            const std::string key = "slot" + std::to_string(i);
            Slot s;
            s.index = i;
            s.state = states.contains(key) ? to_int(states[key]) : 0;
            if (mats.contains(key) && mats[key].is_object()) {
                const auto& m = mats[key];
                s.colour      = m.value("color", std::string());
                if (m.contains("filament") && m["filament"].is_object()) {
                    s.name = m["filament"].value("filament", std::string());
                    s.type = m["filament"].value("type", std::string());
                }
            }
            slots.push_back(s);
        }
    } catch (...) {
        return false;
    }
    return !slots.empty();
}

wxColour parse_colour(const std::string& c)
{
    wxColour w;
    if (c.size() >= 7 && c[0] == '#')
        w.Set(wxString::FromUTF8(c.substr(0, 7)));
    return w.IsOk() ? w : wxColour(128, 128, 128);
}

// "#RRGGBB00" is how the slicer marks a transparent (clear) filament.
bool is_clear(const std::string& c) { return c.size() == 9 && boost::algorithm::to_lower_copy(c.substr(7)) == "00"; }

wxBitmap swatch(wxWindow* w, const std::string& colour, bool clear)
{
    const int s = w->FromDIP(16);
    wxBitmap  bmp(s, s);
    wxMemoryDC dc(bmp);
    if (clear) { // checkerboard, as the sidebar draws a transparent filament
        const int q = s / 4;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                dc.SetBrush(wxBrush((x + y) % 2 ? wxColour(200, 200, 200) : wxColour(255, 255, 255)));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(x * q, y * q, q, q);
            }
    } else {
        dc.SetBrush(wxBrush(parse_colour(colour)));
        dc.SetPen(wxPen(wxColour(90, 90, 90)));
        dc.DrawRectangle(0, 0, s, s);
    }
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxString slot_label(const Slot& s)
{
    wxString text = format_wxstr(_L("Slot %1%"), s.index + 1); // the touchscreen counts from 1
    if (!s.name.empty())
        text += " - " + from_u8(s.name);
    if (s.state == 2)
        text += " " + _L("(in the toolhead)");
    return text;
}

} // namespace

void remember_sync(const std::map<int, AMSMapInfo>& maps, bool overwrite)
{
    std::map<int, int> m;
    if (!overwrite) {
        for (const auto& [f, info] : maps) {
            try {
                m[f] = std::stoi(info.ams_id) * 4 + std::stoi(info.slot_id);
            } catch (...) {}
        }
    } else if (auto* bundle = wxGetApp().preset_bundle) {
        // Overwriting replaces the filament list with the loaded trays, in tray order.
        int f = 0;
        for (const auto& [key, cfg] : bundle->filament_ams_list) {
            const auto* exist = cfg.option<ConfigOptionBools>("filament_exist");
            if (exist && !exist->values.empty() && !exist->values.front())
                continue;
            try {
                m[f++] = std::stoi(cfg.opt_string("ams_id", 0u)) * 4 + std::stoi(cfg.opt_string("slot_id", 0u));
            } catch (...) {}
        }
    }
    if (!m.empty()) {
        save_remembered(m);
        BOOST_LOG_TRIVIAL(info) << "Shop Box: remembered the sync mapping for " << m.size() << " filament(s)";
    }
}

std::string tool_map_gcode(int tool, int slot)
{
    return "SAVE_VARIABLE VARIABLE=value_t" + std::to_string(tool) + " VALUE=\"'slot" + std::to_string(slot) + "'\"";
}

bool confirm_slots(wxWindow* parent, const std::string& print_host, const std::vector<UsedFilament>& used)
{
    if (used.empty())
        return true;
    std::string       api, state;
    std::vector<Slot> slots;
    {
        wxBusyCursor wait;
        api = shop_resolve_api(print_host);
        if (api.empty() || !fetch_box(api, slots, state))
            return true; // not a Box printer, or not reachable: upload as before
    }
    std::vector<Slot> loaded;
    for (const Slot& s : slots)
        if (s.state != 0)
            loaded.push_back(s);
    const wxString title = _L("QIDI Box slots");
    if (loaded.empty()) {
        MessageDialog dlg(parent, _L("The Box reports no loaded slots. Upload anyway? The printer will use its current tool map."), title,
                          wxICON_WARNING | wxYES_NO);
        return dlg.ShowModal() == wxID_YES;
    }

    // Defaults: the slot this filament was synced from, else a slot of exactly its colour, else the n-th
    // loaded slot (the order Box sync fills the filament list in).
    const std::map<int, int> remembered = load_remembered();
    auto default_slot = [&](const UsedFilament& f) -> int {
        auto it = remembered.find(f.index);
        if (it != remembered.end())
            for (size_t k = 0; k < loaded.size(); ++k)
                if (loaded[k].index == it->second)
                    return int(k);
        if (!is_clear(f.colour))
            for (size_t k = 0; k < loaded.size(); ++k)
                if (boost::algorithm::to_lower_copy(loaded[k].colour) == boost::algorithm::to_lower_copy(f.colour.substr(0, 7)))
                    return int(k);
        return std::min(f.index, int(loaded.size()) - 1);
    };

    wxDialog dlg(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    auto*    top  = new wxBoxSizer(wxVERTICAL);
    auto*    help = new wxStaticText(&dlg, wxID_ANY,
                                     _L("Which Box slot should each filament load from? Box sync copies colours and types only, "
                                        "so the printer is told here."));
    help->Wrap(dlg.FromDIP(460));
    top->Add(help, 0, wxALL, dlg.FromDIP(12));
    const bool busy = state == "printing" || state == "paused";
    if (busy) {
        auto* note = new wxStaticText(&dlg, wxID_ANY, _L("The printer is busy: its tool map is left alone until this print can start."));
        note->SetForegroundColour(wxColour(200, 120, 0));
        top->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(12));
    }
    auto* grid = new wxFlexGridSizer(3, dlg.FromDIP(6), dlg.FromDIP(10));
    std::vector<wxBitmapComboBox*> combos;
    for (const UsedFilament& f : used) {
        grid->Add(new wxStaticBitmap(&dlg, wxID_ANY, swatch(&dlg, f.colour, is_clear(f.colour))), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(new wxStaticText(&dlg, wxID_ANY, format_wxstr(_L("Filament %1% (%2%)"), f.index + 1, from_u8(f.type))), 0,
                  wxALIGN_CENTER_VERTICAL);
        auto* combo = new wxBitmapComboBox(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(dlg.FromDIP(260), -1), 0, nullptr,
                                           wxCB_READONLY);
        for (const Slot& s : loaded)
            combo->Append(slot_label(s), swatch(&dlg, s.colour, false));
        combo->SetSelection(default_slot(f));
        grid->Add(combo, 0, wxALIGN_CENTER_VERTICAL);
        combos.push_back(combo);
    }
    top->Add(grid, 0, wxLEFT | wxRIGHT, dlg.FromDIP(12));
    auto* buttons = dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    top->Add(buttons, 0, wxEXPAND | wxALL, dlg.FromDIP(12));
    dlg.SetSizerAndFit(top);
    dlg.CentreOnParent();
    if (dlg.ShowModal() != wxID_OK)
        return false;

    std::map<int, int> chosen = remembered;
    for (size_t i = 0; i < used.size(); ++i)
        chosen[used[i].index] = loaded[size_t(std::max(0, combos[i]->GetSelection()))].index;
    save_remembered(chosen);
    if (busy)
        return true;

    // Write the tool map before the upload starts the print.
    std::string failed;
    for (const UsedFilament& f : used) {
        const std::string script = tool_map_gcode(f.index, chosen[f.index]);
        bool              ok     = false;
        Http::post(api + "/printer/gcode/script")
            .header("Content-Type", "application/json")
            .set_post_body(nlohmann::json{{"script", script}}.dump())
            .timeout_connect(5)
            .timeout_max(15)
            .on_complete([&](std::string, unsigned status) { ok = status == 200; })
            .on_error([&](std::string body, std::string err, unsigned) { failed += script + ": " + err + " " + body + "\n"; })
            .perform_sync();
        BOOST_LOG_TRIVIAL(info) << "Shop Box: " << script << (ok ? " ok" : " FAILED");
    }
    if (!failed.empty()) {
        MessageDialog err(parent, _L("Could not set the printer's tool map:") + "\n" + from_u8(failed) + "\n" + _L("Upload anyway?"), title,
                          wxICON_WARNING | wxYES_NO);
        return err.ShowModal() == wxID_YES;
    }
    return true;
}

}}} // namespace Slic3r::GUI::ShopBox
