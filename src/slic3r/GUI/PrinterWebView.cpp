#include "PrinterWebView.hpp"

#include "PrinterWebViewHandler.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r_version.h"

#include <boost/filesystem/path.hpp>
#include <utility>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include <memory>
#include <wx/log.h>
#include "libslic3r/Utils.hpp"
#include <wx/event.h>
#include <boost/log/trivial.hpp>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/numdlg.h>
#include <wx/collpane.h>
#include "slic3r/GUI/BedMeshView.hpp"
#include "slic3r/GUI/FailureCheckDialogs.hpp"
#include "slic3r/GUI/FailureCheckMonitor.hpp"
#include "slic3r/GUI/ShopQuickRestartDialog.hpp"
#include <wx/timer.h>
#include <regex>
#include "slic3r/Utils/Http.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <nlohmann/json.hpp>

class wxWindow;

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

namespace Slic3r {
namespace GUI {

#ifdef __linux__
// Workaround for #7210: WebKitGTK crashes on vue-resize's hidden <object> probe used by
// older Fluidd/Mainsail pages. Swap that <object> for a <div> shim at appendChild time
// and bridge resize events through a fake contentDocument.defaultView so vue-resize keeps
// working. Workaround proposed by @VittC.
static void inject_vue_resize_workaround(wxWebView *webView)
{
    webView->AddUserScript(
        "(function() {"
        "  'use strict';"
        "  function isVueResizeObject(el) {"
        "    return el && el.tagName === 'OBJECT'"
        "        && el.type === 'text/html'"
        "        && el.getAttribute('aria-hidden') === 'true'"
        "        && el.getAttribute('tabindex') === '-1';"
        "  }"
        "  function isResizeObserverParent(p) {"
        "    return p && p.classList && p.classList.contains('resize-observer');"
        "  }"
        "  function makeShim(orig, parentForRO) {"
        "    var shim = document.createElement('div');"
        "    shim.setAttribute('aria-hidden', 'true');"
        "    shim.setAttribute('tabindex', '-1');"
        "    shim.style.display = 'none';"
        "    var fakeWin = document.createElement('div');"
        "    var ro = null;"
        "    var origRemoveEL = fakeWin.removeEventListener.bind(fakeWin);"
        "    fakeWin.removeEventListener = function(type, fn, opts) {"
        "      origRemoveEL(type, fn, opts);"
        "      if (type === 'resize' && ro) { ro.disconnect(); ro = null; }"
        "    };"
        "    Object.defineProperty(shim, 'contentDocument', {"
        "      configurable: true,"
        "      get: function() { return { defaultView: fakeWin }; }"
        "    });"
        "    Object.defineProperty(shim, 'contentWindow', {"
        "      configurable: true,"
        "      get: function() { return fakeWin; }"
        "    });"
        "    if (typeof orig.onload === 'function') { shim.onload = orig.onload; }"
        "    queueMicrotask(function() {"
        "      if (parentForRO && typeof ResizeObserver !== 'undefined') {"
        "        ro = new ResizeObserver(function() {"
        "          fakeWin.dispatchEvent(new Event('resize'));"
        "        });"
        "        ro.observe(parentForRO);"
        "      }"
        "      if (typeof shim.onload === 'function') {"
        "        try { shim.onload(new Event('load')); } catch (e) {}"
        "      }"
        "      shim.dispatchEvent(new Event('load'));"
        "    });"
        "    return shim;"
        "  }"
        "  var origAppend = Node.prototype.appendChild;"
        "  Node.prototype.appendChild = function(child) {"
        "    if (isResizeObserverParent(this) && isVueResizeObject(child)) {"
        "      return origAppend.call(this, makeShim(child, this));"
        "    }"
        "    return origAppend.call(this, child);"
        "  };"
        "  var origInsertBefore = Node.prototype.insertBefore;"
        "  Node.prototype.insertBefore = function(child, ref) {"
        "    if (isResizeObserverParent(this) && isVueResizeObject(child)) {"
        "      return origInsertBefore.call(this, makeShim(child, this), ref);"
        "    }"
        "    return origInsertBefore.call(this, child, ref);"
        "  };"
        "  console.log('[vr-fix] vue-resize WebKitGTK patch active');"
        "})();",
        wxWEBVIEW_INJECT_AT_DOCUMENT_START
    );
}
#endif

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_browser(nullptr)
    , m_zoomFactor(100)
    , m_apikey()
    , m_apikey_sent(false)
    , m_url_deferred()
    , m_handler(std::make_unique<PrinterWebViewHandler>(*this))
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

      // Create the webview
    create_browser();
    m_reset_on_show = WebView::NeedsRecreateOnShow();

    SetSizer(topsizer);

    build_shop_bar(topsizer);
    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    /* m_browser->GetUserAgent() may lead crash
    if (wxGetApp().get_mode() == comDevelop) {
        wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("Backend: %s Version: %s", m_browser->GetClassInfo()->GetClassName(),
            wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("User Agent: %s", m_browser->GetUserAgent());
    }
    */

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    if (m_fc_timer) {
        m_fc_timer->Stop();
        delete m_fc_timer;
        m_fc_timer = nullptr;
    }
    SetEvtHandlerEnabled(false);
    m_handler.reset();

    // Destroy the webview
    if(m_browser){
        m_browser->Destroy();
        m_browser = nullptr;
    }


    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}

void PrinterWebView::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");

#ifdef __linux__
    inject_vue_resize_workaround(m_browser);

    auto cookiesPath = boost::filesystem::path(data_dir() + "/cache/cookies.db");
    auto wv = static_cast<WebKitWebView*>(m_browser->GetNativeBackend());
    auto wv_ctx = webkit_web_view_get_context(wv);
    auto cookieManager = webkit_web_context_get_cookie_manager(wv_ctx);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesPath.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#endif

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &PrinterWebView::OnNewWindow, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this);
    update_mode();
}

void PrinterWebView::reset_browser()
{
    m_browser->Destroy(); // also removes it from the sizer
    create_browser();
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    Layout();

    // OnLoaded may have cleared m_url_deferred already, so requeue the last url for Show().
    m_apikey_sent  = false;
    m_url_deferred = m_url;
}

void PrinterWebView::load_url(wxString& url, wxString apikey)
{
//    this->Show();
//    this->Raise();
    m_url = url;
    m_apikey = apikey;
    m_apikey_sent = false;
    m_handler = create_printer_webview_handler(*this);

    if (this->IsShown()) {
        //ORCA: m_url_deferred will be cleared on load success
        //m_url_deferred.clear();
        m_browser->LoadURL(url);
    } else {
        m_url_deferred = url;
    }
    //m_browser->SetFocus();
    UpdateState();
}

bool PrinterWebView::Show(bool show)
{
    if (show && std::exchange(m_reset_on_show, false))
        reset_browser();
    if (show && !m_url_deferred.empty()) {
        m_browser->LoadURL(m_url_deferred);
        //ORCA: m_url_deferred will be cleared on load success
        //m_url_deferred.clear();
    }
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

void PrinterWebView::update_mode()
{
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey_sent || m_apikey.IsEmpty())
        return;
    m_apikey_sent   = true;
    wxString script = wxString::Format(R"(
    // Check if window.fetch exists before overriding
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();
    
    // RemoveAllUserScripts causes WebView to forget about our script message handler, 
    // so re-add it here.
    m_browser->RemoveScriptMessageHandler("wx");
    if (m_browser->AddScriptMessageHandler("wx"))
        WebView::MarkScriptMessageHandlerAdded(m_browser);
    else
        wxLogError("Could not add script message handler");

#ifdef __linux__
    // Re-inject the vue-resize/WebKitGTK workaround that RemoveAllUserScripts just cleared.
    inject_vue_resize_workaround(m_browser);
#endif

    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent& evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    //ORCA: url loaded successfully, safe to clear
    m_url_deferred.clear();
    SendAPIKey();
  
    if (m_handler != nullptr) {
        m_handler->on_loaded(evt);
        return;
    }
}

void PrinterWebView::OnNewWindow(wxWebViewEvent& evt)
{
  const wxString url = evt.GetURL();
  if (!url.empty())
    wxLaunchDefaultBrowser(url);
  evt.Veto();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt)
{
  if (m_handler != nullptr)
    m_handler->on_script_message(evt);
}


} // GUI
} // Slic3r

namespace Slic3r { namespace GUI {

// ---- Print-shop: shop toolbar ------------------------------------------------------------------

void PrinterWebView::build_shop_bar(wxBoxSizer* sizer)
{
    m_shop_bar = new wxPanel(this, wxID_ANY);
    auto* bar  = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(m_shop_bar, wxID_ANY, _L("Shop") + ":");
    title->SetFont(title->GetFont().Bold());
    m_btn_full_mesh = new wxButton(m_shop_bar, wxID_ANY, _L("Rebuild full-bed mesh..."));
    m_btn_full_mesh->SetToolTip(_L("Runs SHOP_FULL_MESH on the printer: home, heat the bed, probe the whole bed and save it as the "
                                   "\"default\" mesh that Quick start loads. Takes several minutes."));
    m_btn_full_mesh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_rebuild_full_mesh(); });
    m_shop_status = new wxStaticText(m_shop_bar, wxID_ANY, "");
    m_btn_camera_cal = new wxButton(m_shop_bar, wxID_ANY, _L("Camera calibration..."));
    m_btn_camera_cal->SetToolTip(_L("Teaches the slicer how the printer's camera sees the bed, so the print failure check can compare "
                                    "photos with the G-code automatically. Needs the shop failure-check macros on the printer."));
    m_btn_camera_cal->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string root = moonraker_base_url();
        if (root.empty()) {
            MessageDialog(this, _L("No printer page is loaded."), _L("Camera calibration"), wxICON_INFORMATION | wxOK).ShowModal();
            return;
        }
        show_camera_calibration(this, root);
    });
    m_btn_camera_print = new wxButton(m_shop_bar, wxID_ANY, _L("Calibrate with a test print..."));
    m_btn_camera_print->SetToolTip(_L("Calibrates the camera from a quick print of small dots spread over the bed: no clicking, and "
                                      "more exact than clicking (the G-code says where every dot is)."));
    m_btn_camera_print->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string root = moonraker_base_url();
        if (root.empty()) {
            MessageDialog(this, _L("No printer page is loaded."), _L("Camera calibration"), wxICON_INFORMATION | wxOK).ShowModal();
            return;
        }
        show_camera_test_print_calibration(this, root);
    });
    m_btn_quick_restart = new wxButton(m_shop_bar, wxID_ANY, _L("Quick restart..."));
    m_btn_quick_restart->SetToolTip(_L("While a print is paused in its first 3 layers: clear the plate while it stays on the bed, then print "
                                       "again from layer 1 with a quick purge and no re-leveling. Needs the shop quick restart macros."));
    m_btn_quick_restart->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string root = moonraker_base_url();
        if (root.empty()) {
            MessageDialog(this, _L("No printer page is loaded."), _L("Quick restart"), wxICON_INFORMATION | wxOK).ShowModal();
            return;
        }
        std::string api;
        {
            wxBusyCursor wait;
            api = shop_resolve_api(root);
        }
        show_quick_restart(this, api, "");
    });
    m_fc_status = new wxStaticText(m_shop_bar, wxID_ANY, "");
    bar->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    bar->Add(m_btn_full_mesh, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));
    bar->Add(m_btn_camera_cal, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));
    bar->Add(m_btn_camera_print, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));
    bar->Add(m_btn_quick_restart, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));
    bar->Add(m_shop_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
    bar->Add(m_fc_status, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(10));
    // Failure-check status for this printer, refreshed every few seconds.
    m_fc_timer = new wxTimer(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        const std::string root = moonraker_base_url();
        const wxString    text = root.empty() ? wxString() : from_u8(FailureCheckMonitor::get().status(shop_printer_host(root)));
        if (m_fc_status && m_fc_status->GetLabel() != text) {
            m_fc_status->SetLabel(text);
            m_shop_bar->Layout();
        }
    }, m_fc_timer->GetId());
    m_fc_timer->Start(3000);
    m_shop_bar->SetSizer(bar);
    sizer->Add(m_shop_bar, 0, wxEXPAND);

    // Bed mesh health, collapsed by default; fetches the mesh when first expanded.
    m_mesh_pane = new wxCollapsiblePane(this, wxID_ANY, _L("Bed mesh health"), wxDefaultPosition, wxDefaultSize,
                                        wxCP_DEFAULT_STYLE | wxCP_NO_TLW_RESIZE);
    auto* pane_sizer = new wxBoxSizer(wxVERTICAL);
    m_mesh_panel     = new BedMeshPanel(m_mesh_pane->GetPane(), [this]() { return moonraker_base_url(); });
    pane_sizer->Add(m_mesh_panel, 1, wxEXPAND);
    m_mesh_pane->GetPane()->SetSizer(pane_sizer);
    m_mesh_pane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, [this](wxCollapsiblePaneEvent& e) {
        if (!e.GetCollapsed())
            m_mesh_panel->refresh_from_printer();
        Layout();
    });
    sizer->Add(m_mesh_pane, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));
}

// The printer's API root: scheme://host[:port] of the page shown here (Moonraker is proxied there
// on QIDI and Fluidd/Mainsail installs); port 7125 is tried as a fallback.
std::string PrinterWebView::moonraker_base_url() const
{
    static const std::regex root_re(R"(^(https?://[^/?#]+))", std::regex::icase);
    std::smatch m;
    const std::string url = m_url.ToStdString();
    return std::regex_search(url, m, root_re) ? m[1].str() : std::string();
}

void PrinterWebView::on_rebuild_full_mesh()
{
    if (m_full_mesh_running)
        return;
    const std::string page_root = moonraker_base_url();
    if (page_root.empty()) {
        MessageDialog(this, _L("No printer page is loaded."), _L("Rebuild full-bed mesh"), wxICON_INFORMATION | wxOK).ShowModal();
        return;
    }

    // Find the API and make sure the printer is idle.
    auto query_state = [](const std::string& base, std::string& state) {
        bool ok = false;
        Http::get(base + "/printer/objects/query?print_stats=state")
            .timeout_connect(4)
            .timeout_max(8)
            .on_complete([&](std::string body, unsigned status) {
                if (status != 200)
                    return;
                auto j = nlohmann::json::parse(body, nullptr, false);
                if (!j.is_discarded() && j.contains("result"))
                    try { state = j["result"]["status"]["print_stats"]["state"].get<std::string>(); ok = true; } catch (...) {}
            })
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
        return ok;
    };
    std::string base = page_root, state;
    {
        wxBusyCursor wait;
        if (!query_state(base, state)) {
            static const std::regex host_re(R"(^(https?://[^/:]+))", std::regex::icase);
            std::smatch m;
            if (std::regex_search(page_root, m, host_re))
                base = m[1].str() + ":7125";
            if (!query_state(base, state)) {
                MessageDialog(this, _L("Could not reach the printer's Moonraker API."), _L("Rebuild full-bed mesh"), wxICON_WARNING | wxOK).ShowModal();
                return;
            }
        }
    }
    if (state == "printing" || state == "paused") {
        MessageDialog(this, _L("The printer is busy printing. Rebuild the mesh when it is idle."), _L("Rebuild full-bed mesh"),
                      wxICON_INFORMATION | wxOK).ShowModal();
        return;
    }

    // Bed temperature: default to the current filament's bed temperature, else 80 C.
    long bed_default = 80;
    if (auto* bundle = wxGetApp().preset_bundle) {
        const DynamicPrintConfig cfg = bundle->full_config();
        for (const char* key : {"textured_plate_temp", "hot_plate_temp"})
            if (auto* opt = cfg.option<ConfigOptionInts>(key); opt && !opt->values.empty() && opt->values.front() > 0) {
                bed_default = opt->values.front();
                break;
            }
    }
    const long bed = wxGetNumberFromUser(_L("The mesh is most accurate at printing temperature."), _L("Bed temperature (C):"),
                                         _L("Rebuild full-bed mesh"), bed_default, 0, 120, this);
    if (bed < 0)
        return;
    MessageDialog confirm(this,
                          format_wxstr(_L("Rebuild the full-bed mesh now?\n\nThe printer will home, heat the bed to %1% C and probe the "
                                          "whole bed (81 points, several minutes), then save it as the mesh Quick start uses. "
                                          "Keep the bed clear."), bed),
                          _L("Rebuild full-bed mesh"), wxICON_QUESTION | wxOK | wxCANCEL);
    confirm.SetButtonLabel(wxID_OK, _L("Rebuild"));
    if (confirm.ShowModal() != wxID_OK)
        return;

    // Moonraker answers the script request when the macro finishes, so run it in the background.
    m_full_mesh_running = true;
    m_btn_full_mesh->Disable();
    m_shop_status->SetLabel(format_wxstr(_L("Rebuilding full-bed mesh at %1% C... the printer is probing."), bed));
    m_shop_bar->Layout();
    const std::string script = "SHOP_FULL_MESH BED=" + std::to_string(bed);
    wxWeakRef<PrinterWebView> self(this);
    auto finish = [self](wxString msg) {
        wxGetApp().CallAfter([self, msg]() {
            if (!self)
                return;
            self->m_full_mesh_running = false;
            self->m_btn_full_mesh->Enable();
            self->m_shop_status->SetLabel(msg);
            self->m_shop_bar->Layout();
        });
    };
    Http::post(base + "/printer/gcode/script")
        .header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"script", script}}.dump())
        .timeout_connect(5)
        .timeout_max(1800)
        .on_complete([finish, self](std::string body, unsigned status) {
            if (status == 200) {
                finish(_L("Full-bed mesh rebuilt and saved. Quick start will use it."));
                wxGetApp().CallAfter([self]() { if (self && self->m_mesh_pane && !self->m_mesh_pane->IsCollapsed()) self->m_mesh_panel->refresh_from_printer(); });
            }
            else
                finish(format_wxstr(_L("The printer reported an error (HTTP %1%). Is SHOP_FULL_MESH installed?"), status));
        })
        .on_error([finish](std::string body, std::string error, unsigned status) {
            std::string detail = error;
            try {
                auto j = nlohmann::json::parse(body, nullptr, false);
                if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") &&
                    j["error"]["message"].is_string())
                    detail = j["error"]["message"].get<std::string>();
            } catch (...) {}
            finish(_L("Mesh rebuild failed") + ": " + from_u8(detail));
        })
        .perform();
}

}} // namespace Slic3r::GUI
