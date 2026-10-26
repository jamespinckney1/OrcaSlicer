#include "ShopQuickRestartDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "ShopQuickRestart.hpp"
#include "format.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <thread>

namespace Slic3r { namespace GUI {

namespace {

// Moonraker's error text for a failed request ({"error": {"message": ...}}), else the transport error.
std::string error_text(const std::string& body, const std::string& error)
{
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") && j["error"]["message"].is_string())
        return j["error"]["message"].get<std::string>();
    return error;
}

// Runs a script and waits for it (Moonraker answers when it has finished). False with `error` set on failure.
bool run_script(const std::string& api, const std::string& script, long max_s, std::string& error)
{
    bool ok = false;
    BOOST_LOG_TRIVIAL(info) << "Shop quick restart: sending " << script << " to " << api;
    Http::post(api + "/printer/gcode/script")
        .header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"script", script}}.dump())
        .timeout_connect(5)
        .timeout_max(max_s)
        .on_complete([&](std::string, unsigned status) { ok = status == 200; })
        .on_error([&](std::string body, std::string err, unsigned) { error = error_text(body, err); })
        .perform_sync();
    return ok;
}

bool query_state(const std::string& api, QuickRestart::PrinterState& st, std::string& filename)
{
    const std::string url = api + "/printer/objects/query?print_stats=state,filename,info&pause_resume=is_paused&virtual_sdcard=file_position,file_size&"
                                  "toolhead=homed_axes&" +
                            Http::url_encode("gcode_macro SHOP_RESTART_POINT") + "=pos,size,ok";
    bool ok = false;
    Http::get(url)
        .timeout_connect(3)
        .timeout_max(8)
        .on_complete([&](std::string body, unsigned status) {
            if (status != 200)
                return;
            const auto j = nlohmann::json::parse(body, nullptr, false);
            try {
                const auto& s    = j.at("result").at("status");
                const auto& ps   = s.at("print_stats");
                st.state         = ps.value("state", std::string());
                filename         = ps.value("filename", std::string());
                if (ps.contains("info") && ps["info"].is_object() && ps["info"].contains("current_layer") && ps["info"]["current_layer"].is_number())
                    st.current_layer = ps["info"]["current_layer"].get<int>();
                st.paused        = s.at("pause_resume").value("is_paused", false);
                st.file_position = s.at("virtual_sdcard").value("file_position", 0LL);
                st.file_size     = s.at("virtual_sdcard").value("file_size", 0LL);
                st.homed_axes    = s.at("toolhead").value("homed_axes", std::string());
                if (s.contains("gcode_macro SHOP_RESTART_POINT")) {
                    const auto& rp = s["gcode_macro SHOP_RESTART_POINT"];
                    st.has_macros  = true;
                    st.pos         = rp.value("pos", -1LL);
                    st.size        = rp.value("size", -1LL);
                    st.ok          = rp.value("ok", 0);
                }
                ok = true;
            } catch (...) {}
        })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    return ok;
}

wxString blocker_text(QuickRestart::Blocker b)
{
    switch (b) {
    case QuickRestart::Blocker::NoMacros:
        return _L("The printer does not have the quick restart macros installed (docs/shop/plus4/quick_restart.cfg).");
    case QuickRestart::Blocker::NotPaused: return _L("The print is not paused. Pause it first.");
    case QuickRestart::Blocker::NotMarked:
        return _L("This print was not prepared for a quick restart: upload it from this slicer with the quick restart macros installed.");
    case QuickRestart::Blocker::BeforeLayer1: return _L("The print has not reached layer 1 yet; just resume it.");
    case QuickRestart::Blocker::PastLimit:
        return format_wxstr(_L("A quick restart is only possible during the first %1% layers and before the first filament change."),
                            QuickRestart::kMaxLayers);
    case QuickRestart::Blocker::NotHomed: return _L("The printer is no longer homed. Cancel the print and start it again.");
    case QuickRestart::Blocker::None: break;
    }
    return {};
}

// The banner shown while the printer waits for the plate to be cleared.
class QuickRestartBanner : public wxDialog
{
public:
    QuickRestartBanner(wxWindow* parent, const std::string& api, const std::string& job_name)
        : wxDialog(parent, wxID_ANY, _L("Quick restart"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxSTAY_ON_TOP)
        , m_api(api)
    {
        auto* top   = new wxBoxSizer(wxVERTICAL);
        auto* title = new wxStaticText(this, wxID_ANY, _L("Clear the plate without removing it"));
        title->SetFont(title->GetFont().Bold().Scaled(1.6f));
        title->SetForegroundColour(wxColour(220, 110, 0));
        top->Add(title, 0, wxALL, FromDIP(14));
        auto* text = new wxStaticText(
            this, wxID_ANY,
            (job_name.empty() ? wxString() : from_u8(job_name) + "\n\n") +
                _L("The printer keeps the hotend and bed warm and has lowered the bed. Scrape off the failed print and the purge line "
                   "with the plate left on the bed, without pressing the bed down hard.\n\nThen press Resume: the printer reheats, purges at "
                   "the chute (no homing, no re-leveling, same bed mesh) and prints again from layer 1. The failure check takes a new photo "
                   "of the empty plate first, so keep your hands out of view.\n\nThe hotend turns off after 30 minutes waiting; Resume reheats "
                   "it. Resume on the printer's touchscreen does the same."));
        text->Wrap(FromDIP(520));
        top->Add(text, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(14));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->AddStretchSpacer();
        auto* keep   = new wxButton(this, wxID_ANY, _L("Don't restart"));
        auto* resume = new wxButton(this, wxID_OK, _L("Resume"));
        keep->SetToolTip(_L("The print stays paused where it stopped; a normal Resume continues it."));
        buttons->Add(keep, 0, wxRIGHT, FromDIP(8));
        buttons->Add(resume, 0);
        top->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(14));
        resume->SetDefault();
        resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_resume(); });
        keep->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::string err;
            wxBusyCursor wait;
            if (!run_script(m_api, "SHOP_QUICK_RESTART_CANCEL", 15, err))
                MessageDialog(this, from_u8(err), _L("Quick restart"), wxICON_WARNING | wxOK).ShowModal();
            Destroy();
        });
        // Closing leaves the printer waiting: the touchscreen's Resume still restarts.
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });
        SetSizerAndFit(top);
        CentreOnParent();
    }

private:
    void on_resume()
    {
        // Heating can take minutes and Moonraker answers only when the macro is done, so run it in the background.
        const std::string api = m_api;
        std::thread([api]() {
            std::string err;
            if (run_script(api, "SHOP_QUICK_RESTART_RESUME", 900, err))
                return;
            wxGetApp().CallAfter([err]() {
                MessageDialog(wxGetApp().mainframe, format_wxstr(_L("The printer did not restart the print: %1%"), from_u8(err)), _L("Quick restart"),
                              wxICON_WARNING | wxOK)
                    .ShowModal();
            });
        }).detach();
        Destroy();
    }

    std::string m_api;
};

wxWeakRef<QuickRestartBanner> s_banner;

} // namespace

bool show_quick_restart(wxWindow* parent, const std::string& api_root, const std::string& job_name)
{
    QuickRestart::PrinterState st;
    std::string                filename;
    {
        wxBusyCursor wait;
        if (api_root.empty() || !query_state(api_root, st, filename)) {
            MessageDialog(parent, _L("Could not reach the printer's Moonraker API."), _L("Quick restart"), wxICON_WARNING | wxOK).ShowModal();
            return false;
        }
    }
    if (const QuickRestart::Blocker b = QuickRestart::blocker(st); b != QuickRestart::Blocker::None) {
        MessageDialog(parent, blocker_text(b), _L("Quick restart"), wxICON_INFORMATION | wxOK).ShowModal();
        return false;
    }
    MessageDialog confirm(parent,
                          format_wxstr(_L("Print again from layer 1?\n\nThe print is on layer %1%. Next you clear the plate while it stays on "
                                          "the bed; then Resume purges the nozzle at the chute and prints layer 1 again with the same bed "
                                          "mesh, no re-leveling."),
                                       st.current_layer >= 0 ? st.current_layer : 1),
                          _L("Quick restart"), wxICON_QUESTION | wxOK | wxCANCEL);
    confirm.SetButtonLabel(wxID_OK, _L("Quick restart"));
    if (confirm.ShowModal() != wxID_OK)
        return false;
    std::string err;
    {
        wxBusyCursor wait;
        if (!run_script(api_root, "SHOP_QUICK_RESTART", 60, err)) {
            MessageDialog(parent, format_wxstr(_L("The printer refused the quick restart: %1%"), from_u8(err)), _L("Quick restart"),
                          wxICON_WARNING | wxOK)
                .ShowModal();
            return false;
        }
    }
    if (s_banner)
        s_banner->Destroy();
    auto* banner = new QuickRestartBanner(wxGetApp().mainframe, api_root,
                                          job_name.empty() ? filename.substr(filename.find_last_of('/') + 1) : job_name);
    s_banner     = banner;
    banner->Show();
    banner->Raise();
    return true;
}

}} // namespace Slic3r::GUI
