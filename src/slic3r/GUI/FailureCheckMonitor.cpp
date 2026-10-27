#include "FailureCheckMonitor.hpp"

#include "FailureCheckDialogs.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ShopNotify.hpp"
#include "ShopQuickRestart.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <exception>
#include <future>
#include <regex>
#include <sstream>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

constexpr int kCheckpointTimeoutS = 45;

std::string shop_dir() { return data_dir() + "/shop"; }
std::string fc_dir() { return shop_dir() + "/failure_check"; }

std::string safe_name(std::string s)
{
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.'))
            c = '_';
    // "." or ".." would name a directory, not a file inside it.
    if (!s.empty() && s.find_first_not_of('.') == std::string::npos)
        s = "_";
    return s;
}

std::string base_name(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool read_file(const std::string& path, std::string& out)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& data)
{
    boost::system::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    boost::nowide::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(data.data(), std::streamsize(data.size()));
    return out.good();
}

bool http_get(const std::string& url, std::string& body, long max_s)
{
    bool ok = false;
    Http::get(url)
        .timeout_connect(3)
        .timeout_max(max_s)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            }
        })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    return ok;
}

// A snapshot, restarting the printer's camera service once when it does not answer. Its mjpg-streamer can
// keep running with a dead capture (it froze when the Box's MCU shut down and Klipper restarted), which
// leaves Qidi Link without live view and every checkpoint without a photo until someone restarts it.
bool snapshot_or_restart_camera(const std::string& api, const std::string& print_host, std::string& jpeg)
{
    const std::string url = shop_snapshot_url(print_host);
    if (http_get(url, jpeg, 10))
        return true;
    bool restarted = false;
    Http::post(api + "/machine/services/restart?service=webcamd")
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .timeout_connect(5)
        .timeout_max(30)
        .on_complete([&](std::string, unsigned status) { restarted = status == 200; })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    BOOST_LOG_TRIVIAL(warning) << "Shop failure check: camera did not answer; webcamd restart " << (restarted ? "requested" : "FAILED");
    if (!restarted)
        return false;
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (http_get(url, jpeg, 5))
            return true;
    }
    return false;
}

std::string now_stamp(const char* fmt)
{
    const std::time_t t = std::time(nullptr);
    char              buf[64];
    std::strftime(buf, sizeof(buf), fmt, std::localtime(&t));
    return buf;
}

} // namespace

// ---- helpers ------------------------------------------------------------------------------------

std::string shop_printer_host(const std::string& print_host)
{
    static const std::regex re(R"(^(?:[a-zA-Z][a-zA-Z0-9+.-]*://)?([^/:?#]+))");
    std::smatch m;
    return std::regex_search(print_host, m, re) ? m[1].str() : print_host;
}

std::vector<std::string> shop_api_candidates(const std::string& print_host)
{
    static const std::regex re(R"(^(?:([a-zA-Z][a-zA-Z0-9+.-]*)://)?([^/:?#]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(print_host, m, re))
        return {};
    const std::string scheme = m[1].matched ? m[1].str() : "http";
    const std::string host   = m[2].str();
    std::vector<std::string> out;
    if (m[3].matched)
        out.push_back(scheme + "://" + host + ":" + m[3].str());
    out.push_back(scheme + "://" + host);
    if (!m[3].matched || m[3].str() != "7125")
        out.push_back("http://" + host + ":7125");
    return out;
}

std::string shop_snapshot_url(const std::string& print_host) { return "http://" + shop_printer_host(print_host) + "/webcam/?action=snapshot"; }

std::string shop_resolve_api(const std::string& print_host)
{
    for (const std::string& root : shop_api_candidates(print_host)) {
        std::string body;
        if (http_get(root + "/printer/objects/query?print_stats=state", body, 5) && body.find("print_stats") != std::string::npos)
            return root;
    }
    return {};
}

static std::string camera_file(const std::string& host) { return shop_dir() + "/camera/" + safe_name(host) + ".json"; }

bool load_shop_camera(const std::string& printer_host, FailureCheck::CameraModel& out)
{
    std::string text;
    return read_file(camera_file(printer_host), text) && FailureCheck::CameraModel::from_json(text, out);
}

bool save_shop_camera(const std::string& printer_host, const FailureCheck::CameraModel& cam)
{
    return write_file(camera_file(printer_host), cam.to_json());
}

void shop_send_script(const std::string& api_root, const std::string& script)
{
    BOOST_LOG_TRIVIAL(info) << "Shop failure check: sending " << script << " to " << api_root;
    Http::post(api_root + "/printer/gcode/script")
        .header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"script", script}}.dump())
        .timeout_connect(5)
        .timeout_max(600)
        .on_error([script](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "Shop failure check: " << script << " failed: " << error << " " << status << " " << body;
        })
        .perform();
}

bool shop_decode_image(const std::string& bytes, FailureCheck::Image& out)
{
    if (bytes.empty())
        return false;
    wxLogNull           no_log; // runs on the worker thread: a corrupt frame must not queue an error dialog
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage             img;
    if (!img.LoadFile(stream, wxBITMAP_TYPE_ANY) || !img.IsOk())
        return false;
    if (img.GetWidth() > 8192 || img.GetHeight() > 8192)
        return false;
    out.w = img.GetWidth();
    out.h = img.GetHeight();
    out.rgb.assign(img.GetData(), img.GetData() + size_t(out.w) * out.h * 3);
    return true;
}

// Adds the quick restart markers when the printer has the macros; the message says what happened (empty: not a
// quick-restart printer, nothing to say).
static std::string add_restart_markers(const std::string& objects, const std::string& gcode_path)
{
    if (objects.find("gcode_macro SHOP_RESTART_POINT") == std::string::npos)
        return {};
    QuickRestart::Plan plan;
    std::string        err;
    if (!QuickRestart::write_with_markers(gcode_path, gcode_path, plan, err))
        return _u8L("Quick restart is not available for this print: ") + err;
    return plan.limit_is_tool_change ? _u8L("Quick restart is available until the first filament change.") :
                                       _u8L("Quick restart is available during the first 3 layers.");
}

std::string shop_prepare_quick_restart(const std::string& print_host, const std::string& gcode_path)
{
    const std::string api = shop_resolve_api(print_host);
    std::string       objects;
    if (api.empty() || !http_get(api + "/printer/objects/list", objects, 5))
        return {};
    return add_restart_markers(objects, gcode_path);
}

// ---- monitor ------------------------------------------------------------------------------------

FailureCheckMonitor& FailureCheckMonitor::get()
{
    // Never destroyed: stop() may leave the thread finishing a request after the app has quit.
    static FailureCheckMonitor* instance = new FailureCheckMonitor();
    return *instance;
}

void FailureCheckMonitor::configure()
{
    const AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    std::string service = boost::algorithm::trim_copy(cfg->get(kServiceKey));
    while (!service.empty() && service.back() == '/')
        service.pop_back();
    if (!service.empty() && service.find("://") == std::string::npos)
        service = "http://" + service;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (service != m_service)
        m_service_seq = -1;
    m_service       = service;
    m_service_token = boost::algorithm::trim_copy(cfg->get(kServiceTokenKey));
    m_ntfy_server = boost::algorithm::trim_copy(cfg->get(ShopNotify::kServerKey));
    m_ntfy_topic  = boost::algorithm::trim_copy(cfg->get(ShopNotify::kTopicKey));
}

void FailureCheckMonitor::start()
{
    configure();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started)
        return;
    m_started = true;
    m_stop    = std::make_shared<std::atomic<bool>>(false);
    load_jobs();
    m_thread = std::thread([this]() { run(); });
}

void FailureCheckMonitor::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started)
            return;
        m_started = false;
        m_stop->store(true);
    }
    m_cv.notify_all();
    // A request in flight can take a few seconds; never hold up quitting for it. The thread checks the
    // stop flag before touching the UI, and the monitor is a static that outlives it.
    if (m_thread.joinable()) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut  = done->get_future();
        std::thread([t = std::move(m_thread), done]() mutable {
            t.join();
            done->set_value();
        }).detach();
        fut.wait_for(std::chrono::milliseconds(1500));
    }
}

void FailureCheckMonitor::set_status(const std::string& host, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (text.empty())
        m_status.erase(host);
    else
        m_status[host] = text;
}

std::string FailureCheckMonitor::status(const std::string& printer_host) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_status.find(printer_host);
    return it == m_status.end() ? std::string() : it->second;
}

void FailureCheckMonitor::save_jobs_locked() const
{
    nlohmann::json arr = nlohmann::json::array();
    for (const Job& j : m_jobs)
        arr.push_back({{"id", j.id}, {"host", j.host}, {"api", j.api}, {"job_name", j.job_name}, {"dir", j.dir}, {"gcode", j.gcode},
                       {"created", j.created}, {"started", j.started}, {"handled", j.handled}, {"has_baseline", j.has_baseline},
                       {"baseline_z", j.baseline_z}, {"reference_photo", j.reference_photo}, {"reference_z", j.reference_z},
                       {"reference_index", j.reference_index}, {"total", j.total}, {"passed", j.passed}, {"remote", j.remote}, {"restarts", j.restarts}, {"alignment", [&]() {
                           nlohmann::json a = nlohmann::json::array();
                           for (const auto& an : j.alignment.anchors)
                               a.push_back({an.x, an.y, an.dx, an.dy, an.s});
                           return a;
                       }()}});
    write_file(fc_dir() + "/jobs.json", arr.dump(2));
}

void FailureCheckMonitor::load_jobs()
{
    std::string text;
    if (!read_file(fc_dir() + "/jobs.json", text))
        return;
    const auto arr = nlohmann::json::parse(text, nullptr, false);
    if (!arr.is_array())
        return;
    for (const auto& o : arr) {
        try {
            Job j;
            j.id           = o.at("id").get<std::string>();
            j.host         = o.at("host").get<std::string>();
            j.api          = o.value("api", std::string());
            j.job_name     = o.at("job_name").get<std::string>();
            j.dir          = o.at("dir").get<std::string>();
            j.gcode        = o.at("gcode").get<std::string>();
            j.created      = o.value("created", 0LL);
            j.started      = o.value("started", false);
            j.handled      = o.value("handled", std::vector<int>());
            j.has_baseline = o.value("has_baseline", false);
            j.baseline_z   = o.value("baseline_z", 0.0);
            j.reference_photo = o.value("reference_photo", std::string());
            j.reference_z     = o.value("reference_z", 0.0);
            j.reference_index = o.value("reference_index", 0);
            j.total        = o.value("total", 0);
            j.passed       = o.value("passed", 0);
            j.remote       = o.value("remote", false);
            j.restarts     = o.value("restarts", -1);
            if (o.contains("alignment") && o["alignment"].is_array())
                for (const auto& a : o["alignment"])
                    if (a.is_array() && (a.size() == 4 || a.size() == 5))
                        j.alignment.anchors.push_back({a[0].get<double>(), a[1].get<double>(), a[2].get<double>(), a[3].get<double>(),
                                                       a.size() == 5 ? a[4].get<double>() : 1.0});
            if (fs::exists(j.gcode))
                m_jobs.push_back(j);
        } catch (...) {
        }
    }
}

std::shared_ptr<const FailureCheck::GCodeModel> FailureCheckMonitor::model_for(const Job& job)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_models.find(job.id);
        if (it != m_models.end())
            return it->second;
    }
    auto        model = std::make_shared<FailureCheck::GCodeModel>();
    std::string err;
    if (!FailureCheck::parse_gcode(job.gcode, *model, err)) {
        BOOST_LOG_TRIVIAL(warning) << "Shop failure check: cannot parse " << job.gcode << ": " << err;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_models[job.id] = model;
    return model;
}

bool FailureCheckMonitor::prepare_job(const std::string& print_host, const std::string& gcode_path, const std::string& upload_name,
                                      std::string& message)
{
    const std::string api = shop_resolve_api(print_host);
    if (api.empty()) {
        message = _u8L("Failure check skipped: could not reach the printer's Moonraker API.");
        return false;
    }
    std::string objects;
    if (!http_get(api + "/printer/objects/list", objects, 5) || objects.find("gcode_macro SHOP_CHECKPOINT") == std::string::npos) {
        message = _u8L("Failure check skipped: the printer does not have the SHOP_CHECKPOINT macro installed "
                       "(see docs/shop/plus4/failure_check.cfg).");
        return false;
    }
    FailureCheck::GCodeModel model;
    std::string              err;
    if (!FailureCheck::parse_gcode(gcode_path, model, err) || model.checkpoints.size() < 2) {
        message = _u8L("Failure check skipped: ") + (err.empty() ? std::string("no layers to check") : err);
        return false;
    }
    if (!FailureCheck::write_with_checkpoints(gcode_path, gcode_path, model.checkpoints, kCheckpointTimeoutS, err)) {
        message = _u8L("Failure check skipped: ") + err;
        return false;
    }
    // After the checkpoints (the markers carry byte offsets) and before the job copy (which must be the uploaded file).
    const std::string restart_message = add_restart_markers(objects, gcode_path);
    Job job;
    job.host     = shop_printer_host(print_host);
    job.api      = api;
    job.job_name = base_name(upload_name);
    job.id       = now_stamp("%Y%m%d-%H%M%S") + "_" + safe_name(fs::path(job.job_name).stem().string());
    job.dir      = fc_dir() + "/jobs/" + job.id;
    job.gcode    = job.dir + "/job.gcode";
    job.created  = (long long) std::time(nullptr);
    job.total    = int(model.checkpoints.size());
    boost::system::error_code ec;
    fs::create_directories(job.dir, ec);
    fs::copy_file(gcode_path, job.gcode, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        message = _u8L("Failure check: checkpoints added, but the job copy could not be saved, so they will only pause briefly.");
        return true;
    }
    // Keep the last 15 job folders.
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(fc_dir() + "/jobs", ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_directory(it->path()))
            dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end());
    for (size_t i = 0; i + 15 < dirs.size(); ++i)
        if (dirs[i].filename().string() != job.id)
            fs::remove_all(dirs[i], ec);
    std::string service, service_error;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        service = m_service;
    }
    if (!service.empty())
        job.remote = hand_off(job, service_error);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Job& j : m_jobs)
            if (j.host == job.host)
                m_models.erase(j.id);
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const Job& j) { return j.host == job.host; }), m_jobs.end());
        m_jobs.push_back(job);
        save_jobs_locked();
        m_status[job.host] = (boost::format(_u8L("Failure check: %1% checkpoints in %2%, waiting for the print to start")) %
                              (job.total - 1) % job.job_name).str();
    }
    m_cv.notify_all();
    message = (boost::format(_u8L("Failure check: %1% camera checkpoints added.")) % (model.checkpoints.size() - 1)).str();
    if (job.remote)
        message += " " + _u8L("The failure check service watches this print, also while the slicer is closed.");
    else if (!service.empty())
        message += " " + (boost::format(_u8L("The failure check service did not take the job (%1%), so this slicer watches it: keep it open.")) %
                          service_error).str();
    if (!restart_message.empty())
        message += " " + restart_message;
    return true;
}

void FailureCheckMonitor::run()
{
    auto stop = m_stop;
    while (!stop->load()) {
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            jobs = m_jobs;
        }
        // Jobs the service checks: mirror it, and drop the ones it has finished.
        std::set<std::string> finished;
        try {
            finished = poll_service(jobs);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Shop failure check: asking the service failed: " << e.what();
        }
        for (Job& job : jobs) {
            if (stop->load())
                return;
            // An exception would end this thread for good, so log it and try the job again next round.
            bool keep = true;
            try {
                keep = job.remote ? finished.count(job.id) == 0 : poll(job);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Shop failure check: polling " << job.host << " failed: " << e.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Shop failure check: polling " << job.host << " failed";
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [&](const Job& j) { return j.id == job.id; });
            if (it == m_jobs.end())
                continue; // replaced by a newer job meanwhile
            if (keep)
                *it = job;
            else {
                m_models.erase(job.id);
                m_jobs.erase(it);
            }
            save_jobs_locked();
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::seconds(jobs.empty() ? 10 : 2), [&]() { return stop->load(); });
    }
}

bool FailureCheckMonitor::poll(Job& job)
{
    const long long now = (long long) std::time(nullptr);
    if (!job.started && now - job.created > 12 * 3600) {
        set_status(job.host, "");
        return false;
    }
    if (job.api.empty())
        job.api = shop_resolve_api(job.host);
    if (job.api.empty())
        return true;
    std::string body;
    if (!http_get(job.api + "/printer/objects/query?print_stats=state,filename&toolhead=position&" +
                      Http::url_encode("gcode_macro SHOP_CHECKPOINT") + "=waiting,index,held&" +
                      Http::url_encode("gcode_macro SHOP_RESTART_POINT") + "=restarts",
                  body, 5))
        return true; // printer unreachable: try again
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return true;
    std::string state, filename;
    int         waiting = 0, index = -1, restarts = -1;
    double      z       = 0;
    try {
        const auto& st = j.at("result").at("status");
        state          = st.at("print_stats").value("state", std::string());
        filename       = st.at("print_stats").value("filename", std::string());
        if (st.contains("gcode_macro SHOP_CHECKPOINT")) {
            waiting = st["gcode_macro SHOP_CHECKPOINT"].value("waiting", 0);
            index   = st["gcode_macro SHOP_CHECKPOINT"].value("index", -1);
        }
        if (st.contains("gcode_macro SHOP_RESTART_POINT"))
            restarts = st["gcode_macro SHOP_RESTART_POINT"].value("restarts", -1);
        if (st.contains("toolhead") && st["toolhead"].contains("position") && st["toolhead"]["position"].size() >= 3)
            z = st["toolhead"]["position"][2].get<double>();
    } catch (...) {
        return true;
    }
    const bool ours   = base_name(filename) == job.job_name;
    const bool active = ours && (state == "printing" || state == "paused");
    if (active && !job.started) {
        job.started = true;
        set_status(job.host, (boost::format(_u8L("Failure check: watching %1%")) % job.job_name).str());
    }
    if (job.started && !active) {
        set_status(job.host, (boost::format(_u8L("Failure check: %1% finished (%2% of %3% checks passed)")) % job.job_name % job.passed %
                              std::max(0, job.total - 1)).str());
        return false;
    }
    // The printer counts quick restarts; one since the last poll means the job runs again from layer 1, with
    // every checkpoint (the reference photo first) still to come.
    if (active && restarts >= 0) {
        if (job.restarts >= 0 && restarts > job.restarts)
            restart_job(job);
        job.restarts = restarts;
    }
    if (active && waiting == 1 && index >= 0 && std::find(job.handled.begin(), job.handled.end(), index) == job.handled.end())
        handle_checkpoint(job, index, z);
    return true;
}

void FailureCheckMonitor::restart_job(Job& job)
{
    // The first attempt's photos and results move to attemptN/, so cpN.* are always the current attempt's.
    boost::system::error_code ec;
    int                       attempt = 1;
    while (fs::exists(job.dir + "/attempt" + std::to_string(attempt), ec))
        ++attempt;
    const fs::path archive = job.dir + "/attempt" + std::to_string(attempt);
    for (fs::directory_iterator it(job.dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("cp", 0) == 0 && (it->path().extension() == ".jpg" || it->path().extension() == ".json")) {
            boost::system::error_code ec2;
            fs::create_directories(archive, ec2);
            fs::rename(it->path(), archive / name, ec2);
        }
    }
    job.handled.clear();
    job.has_baseline    = false;
    job.baseline_z      = 0;
    job.reference_photo.clear();
    job.reference_z     = 0;
    job.reference_index = 0;
    job.passed          = 0;
    job.camera_warned   = false;
    job.alignment       = FailureCheck::Alignment();
    set_status(job.host, (boost::format(_u8L("Failure check: quick restart of %1%, checking again from layer 1")) % job.job_name).str());
    BOOST_LOG_TRIVIAL(info) << "Shop failure check: quick restart of " << job.id << ", earlier photos in " << archive.string();
}

// ---- failure check service ----------------------------------------------------------------------

bool FailureCheckMonitor::hand_off(const Job& job, std::string& error)
{
    std::string service, token, ntfy_server, ntfy_topic;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        service     = m_service;
        token       = m_service_token;
        ntfy_server = m_ntfy_server;
        ntfy_topic  = m_ntfy_topic;
    }
    nlohmann::json meta = {{"id", job.id},        {"host", job.host}, {"api", job.api}, {"job_name", job.job_name},
                           {"ntfy_server", ntfy_server}, {"ntfy_topic", ntfy_topic}};
    std::string cam_text;
    if (read_file(camera_file(job.host), cam_text)) {
        const auto cam = nlohmann::json::parse(cam_text, nullptr, false);
        if (cam.is_object())
            meta["camera"] = cam;
    }
    auto request = [&error](Http&& http) {
        bool ok = false;
        http.on_complete([&](std::string body, unsigned status) {
                ok = status == 200;
                if (!ok)
                    error = "HTTP " + std::to_string(status) + " " + body;
            })
            .on_error([&](std::string body, std::string err, unsigned status) {
                error = err + (status ? " (HTTP " + std::to_string(status) + " " + body + ")" : std::string());
            })
            .perform_sync();
        return ok;
    };
    const std::string auth = "Bearer " + token;
    const bool ok = request(std::move(Http::post(service + "/jobs")
                                          .header("Authorization", auth)
                                          .header("Content-Type", "application/json")
                                          .set_post_body(meta.dump())
                                          .timeout_connect(4)
                                          .timeout_max(15))) &&
                    request(std::move(Http::put(service + "/jobs/" + job.id + "/gcode")
                                          .header("Authorization", auth)
                                          .set_put_body(fs::path(job.gcode))
                                          .timeout_connect(4)
                                          .timeout_max(300)));
    BOOST_LOG_TRIVIAL(info) << "Shop failure check: handing " << job.id << " to " << service << (ok ? ": ok" : ": " + error);
    return ok;
}

bool FailureCheckMonitor::service_get(const std::string& path, std::string& body)
{
    std::string service, token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        service = m_service;
        token   = m_service_token;
    }
    if (service.empty())
        return false;
    bool ok = false;
    Http::get(service + path)
        .header("Authorization", "Bearer " + token)
        .timeout_connect(3)
        .timeout_max(10)
        .on_complete([&](std::string b, unsigned status) {
            if (status == 200) {
                body = std::move(b);
                ok   = true;
            }
        })
        .on_error([](std::string, std::string, unsigned) {})
        .perform_sync();
    return ok;
}

std::set<std::string> FailureCheckMonitor::poll_service(const std::vector<Job>& jobs)
{
    std::set<std::string> finished;
    if (std::none_of(jobs.begin(), jobs.end(), [](const Job& j) { return j.remote; }))
        return finished;
    std::string service;
    long long   since;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        service = m_service;
        since   = m_service_seq;
    }
    std::string body;
    if (service.empty() || !service_get("/status?since=" + std::to_string(std::max(0LL, since)), body)) {
        for (const Job& job : jobs)
            if (job.remote)
                set_status(job.host, (boost::format(_u8L("Failure check: the service at %1% is not answering (or the token is wrong); it checks "
                                                         "%2% on its own as long as it runs")) %
                                      (service.empty() ? std::string("(not set)") : service) % job.job_name).str());
        return finished;
    }
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object())
        return finished;
    const long long now = (long long) std::time(nullptr);
    for (const Job& job : jobs) {
        if (!job.remote)
            continue;
        const auto hosts = j.value("hosts", nlohmann::json::object());
        if (!hosts.contains(job.host) || !hosts[job.host].is_object() || hosts[job.host].value("job_id", std::string()) != job.id) {
            // The service no longer has this job (a newer one on the printer, or the service lost it).
            if (now - job.created > 12 * 3600)
                finished.insert(job.id);
            continue;
        }
        const auto& h = hosts[job.host];
        set_status(job.host, h.value("text", std::string()) + " " + _u8L("(checked by the service)"));
        if (!h.value("active", true))
            finished.insert(job.id);
    }
    // Hold windows for checkpoints the service held. Right after the slicer starts, only recent holds: an older
    // one has long been answered or has turned the hotend off.
    for (const auto& ev : j.value("events", nlohmann::json::array())) {
        if (!ev.is_object() || ev.value("mode", std::string()) != "hold")
            continue;
        if (since < 0 && now - ev.value("time", 0LL) > 30 * 60)
            continue;
        auto job = std::find_if(jobs.begin(), jobs.end(), [&](const Job& jb) { return jb.remote && jb.id == ev.value("job_id", std::string()); });
        if (job != jobs.end())
            show_service_hold(*job, ev.value("index", -1), ev.value("label", std::string()));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_service_seq = j.value("seq", 0LL);
    return finished;
}

void FailureCheckMonitor::show_service_hold(const Job& job, int index, const std::string& label)
{
    // Recompute the service's comparison from its photos with the same engine, so the window shows the same
    // overlay as a hold found by the slicer itself.
    const std::string        url = "/jobs/" + job.id + "/cp" + std::to_string(index);
    std::string              result_text, jpeg, base_jpeg;
    auto                     model = model_for(job);
    const FailureCheck::Checkpoint* cp = model ? model->checkpoint(index) : nullptr;
    FailureCheck::CameraModel cam;
    FailureCheck::Image       cur, base;
    nlohmann::json            r;
    bool ok = cp != nullptr && load_shop_camera(job.host, cam) && service_get(url + ".json", result_text) && service_get(url + ".jpg", jpeg);
    if (ok) {
        r                     = nlohmann::json::parse(result_text, nullptr, false);
        const std::string base_path = r.is_object() ? r.value("base", std::string()) : std::string();
        ok = !base_path.empty() && service_get("/jobs/" + base_path, base_jpeg) && shop_decode_image(jpeg, cur) &&
             shop_decode_image(base_jpeg, base) && cur.w == base.w && cur.h == base.h;
    }
    if (!ok) {
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" FAILED, printer paused (the service sent the photo to your phone)")) %
                              label).str());
        return;
    }
    FailureCheck::Alignment prior;
    for (const auto& a : r.value("prior_alignment", nlohmann::json::array()))
        if (a.is_array() && a.size() >= 4)
            prior.anchors.push_back({a[0].get<double>(), a[1].get<double>(), a[2].get<double>(), a[3].get<double>(),
                                     a.size() >= 5 ? a[4].get<double>() : 1.0});
    const double z     = r.value("z", 0.0);
    const std::string photo = job.dir + "/cp" + std::to_string(index) + ".jpg";
    write_file(photo, jpeg);
    FailureCheckEvent ev;
    ev.mode          = FailureCheckEvent::Mode::Hold;
    ev.host          = job.host;
    ev.api           = job.api;
    ev.job_name      = job.job_name;
    ev.label         = label;
    ev.index         = index;
    ev.total         = job.total - 1;
    ev.photo_path    = photo;
    ev.z             = z;
    ev.model         = model;
    ev.segments_done = cp->segments_done;
    ev.camera        = cam;
    ev.has_result    = true;
    const FailureCheck::Checkpoint* base_cp = r.value("base_checkpoint", 0) > 0 ? model->checkpoint(r.value("base_checkpoint", 0)) : nullptr;
    ev.result = FailureCheck::compare(*model, *cp, cam, base, r.value("base_z", 0.0), cur, z, prior, base_cp ? base_cp->segments_done : 0);
    post_event(ev);
}

void FailureCheckMonitor::post_event(const FailureCheckEvent& ev)
{
    auto stop = m_stop;
    if (stop->load() || wxTheApp == nullptr)
        return;
    wxGetApp().CallAfter([ev, stop]() {
        if (!stop->load())
            show_failure_check_event(ev);
    });
}

enum class ParkState { Parked, Resumed, Elsewhere };

// Parked while the checkpoint holds the printer at the park position (from the camera calibration); Elsewhere
// when it waits but the toolhead never parked (Klipper drops the park moves of a checkpoint inside an excluded
// object's G-code).
static ParkState park_state(const std::string& api)
{
    std::string body;
    if (!http_get(api + "/printer/objects/query?toolhead=position&" + Http::url_encode("gcode_macro SHOP_CHECKPOINT") + "=waiting", body, 5))
        return ParkState::Parked; // cannot tell: keep today's behaviour
    const auto j = nlohmann::json::parse(body, nullptr, false);
    try {
        const auto& st  = j.at("result").at("status");
        const auto& pos = st.at("toolhead").at("position");
        if (st.at("gcode_macro SHOP_CHECKPOINT").value("waiting", 0) != 1)
            return ParkState::Resumed;
        return std::hypot(pos.at(0).get<double>() - FailureCheck::kShopParkX, pos.at(1).get<double>() - FailureCheck::kShopParkY) < 5.0 ?
                   ParkState::Parked :
                   ParkState::Elsewhere;
    } catch (...) {
        return ParkState::Parked;
    }
}

// The reference photo (cp0) of the most recent earlier job that has one: the empty plate as it was before
// that print, normally the same plate and side (the camera calibration's photos may show another, whose
// printed markings then read as material). `z` is that job's toolhead Z at the reference, which equals its
// first-layer checkpoints' (cp1.json).
static bool previous_reference(const std::string& this_dir, FailureCheck::Image& img, double& z)
{
    boost::system::error_code ec;
    std::vector<fs::path>     dirs;
    for (fs::directory_iterator it(fc_dir() + "/jobs", ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_directory(it->path()) && fs::path(this_dir).filename() != it->path().filename())
            dirs.push_back(it->path());
    std::sort(dirs.rbegin(), dirs.rend()); // newest first (folders start with the upload time)
    for (const fs::path& d : dirs) {
        if (d.filename().string() > fs::path(this_dir).filename().string())
            continue; // a later job: not "before" this one
        std::string bytes, text;
        if (!read_file((d / "cp0.jpg").string(), bytes) || !read_file((d / "cp1.json").string(), text))
            continue;
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.contains("z") || !j["z"].is_number() || !shop_decode_image(bytes, img))
            continue;
        z = j["z"].get<double>();
        return true;
    }
    return false;
}

void FailureCheckMonitor::notify_hold(const FailureCheckEvent& ev)
{
    ShopNotify::Message msg;
    msg.title    = (boost::format(_u8L("Print held: %1%")) % ev.job_name).str();
    msg.body     = (boost::format(_u8L("Checkpoint \"%1%\" (%2% of %3%) failed on %4%")) % ev.label % ev.index % ev.total % ev.host).str();
    if (!ev.result.reasons.empty())
        msg.body += ": " + ev.result.reasons.front();
    msg.body += "\n" + _u8L("The printer is paused and beeping; the hotend turns off after 15 minutes.");
    msg.tags            = "warning";
    msg.priority        = "high";
    msg.attachment_path = ev.photo_path;
    notify(msg);
}

void FailureCheckMonitor::notify(const ShopNotify::Message& msg)
{
    // ShopNotify reads the topic from app_config, which belongs to the UI thread.
    auto stop = m_stop;
    if (stop->load() || wxTheApp == nullptr)
        return;
    wxGetApp().CallAfter([msg, stop]() {
        if (!stop->load())
            ShopNotify::send(msg);
    });
}

void FailureCheckMonitor::handle_checkpoint(Job& job, int index, double z)
{
    job.handled.push_back(index);
    auto model = model_for(job);
    const FailureCheck::Checkpoint* cp = model ? model->checkpoint(index) : nullptr;
    if (!cp) {
        shop_send_script(job.api, "SHOP_CHECK_OK");
        return;
    }
    const std::string label = FailureCheck::checkpoint_label(*cp);
    // A slicer that starts while the printer is already parked can see the checkpoint just before its 45 s
    // timeout resumes the print, and photograph the toolhead back over the plate. Give the comparison a
    // fresh minute first, and use the photo only if the toolhead was still parked when it was taken.
    shop_send_script(job.api, "UPDATE_DELAYED_GCODE ID=_SHOP_CHECK_TIMEOUT DURATION=60");
    std::string jpeg;
    if (!snapshot_or_restart_camera(job.api, job.host, jpeg)) {
        shop_send_script(job.api, "SHOP_CHECK_OK"); // fail open
        set_status(job.host, (boost::format(_u8L("Failure check: camera did not answer at \"%1%\"; print continues")) % label).str());
        if (!job.camera_warned) {
            job.camera_warned = true;
            ShopNotify::Message msg;
            msg.title = (boost::format(_u8L("Camera not answering: %1%")) % job.job_name).str();
            msg.body  = (boost::format(_u8L("The camera on %1% did not answer at \"%2%\", even after restarting it. The print continues "
                                            "without failure checks; power-cycling the printer usually brings the camera back.")) %
                         job.host % label).str();
            msg.tags = "warning";
            notify(msg);
        }
        return;
    }
    const ParkState parked = park_state(job.api);
    if (parked == ParkState::Resumed) {
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" missed (the print had already resumed)")) % label).str());
        return;
    }
    if (parked == ParkState::Elsewhere) {
        shop_send_script(job.api, "SHOP_CHECK_OK"); // the toolhead hides the print: fail open
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" skipped (the toolhead did not park); print continues")) % label).str());
        ShopNotify::Message msg;
        msg.title = (boost::format(_u8L("Check skipped: %1%")) % job.job_name).str();
        msg.body  = (boost::format(_u8L("The printer paused for \"%1%\" but the toolhead did not park, so the plate could not be "
                                        "photographed. This happens when the checkpoint falls inside an object excluded during the "
                                        "print. The print continues.")) % label).str();
        msg.tags = "warning";
        notify(msg);
        return;
    }
    const std::string photo = job.dir + "/cp" + std::to_string(index) + ".jpg";
    write_file(photo, jpeg);

    if (cp->kind == FailureCheck::CheckpointKind::Baseline) {
        job.has_baseline = true;
        job.baseline_z   = z;
        shop_send_script(job.api, "SHOP_CHECK_OK");
        set_status(job.host, (boost::format(_u8L("Failure check: reference photo taken for %1%")) % job.job_name).str());
        return;
    }

    FailureCheckEvent ev;
    ev.host          = job.host;
    ev.api           = job.api;
    ev.job_name      = job.job_name;
    ev.label         = label;
    ev.index         = index;
    ev.total         = job.total - 1;
    ev.photo_path    = photo;
    ev.z             = z;
    ev.model         = model;
    ev.segments_done = cp->segments_done;

    FailureCheck::CameraModel cam;
    FailureCheck::Image       base, cur;
    std::string               base_bytes, review_reason;
    double                    base_z         = job.baseline_z;
    bool                      stand_in_reference = false;
    size_t                    base_segments  = 0;
    if (!load_shop_camera(job.host, cam))
        review_reason = _u8L("the camera is not calibrated yet");
    else if (!shop_decode_image(jpeg, cur) || cur.w != cam.width || cur.h != cam.height)
        review_reason = _u8L("the camera photo does not match the calibration's size");
    else if (!job.reference_photo.empty() && read_file(job.dir + "/" + job.reference_photo, base_bytes) &&
             shop_decode_image(base_bytes, base) && base.w == cur.w && base.h == cur.h) {
        // a first-layer photo that replaced a reference which no longer matched
        base_z = job.reference_z;
        if (const FailureCheck::Checkpoint* b = model->checkpoint(job.reference_index))
            base_segments = b->segments_done;
    } else if (job.has_baseline && read_file(job.dir + "/cp0.jpg", base_bytes) && shop_decode_image(base_bytes, base) && base.w == cur.w &&
             base.h == cur.h) {
        // the job's own reference photo
    } else if (previous_reference(job.dir, base, base_z) && base.w == cur.w && base.h == cur.h) {
        // The printer parked for the reference photo while the slicer was closed: compare with the previous
        // job's empty plate instead of leaving the whole print unchecked.
        stand_in_reference = true;
    } else
        review_reason = _u8L("this job has no reference photo of the empty plate");
    if (!review_reason.empty()) {
        // Nothing to compare with: let the user judge, resuming after a countdown.
        ev.mode          = FailureCheckEvent::Mode::Review;
        ev.review_reason = review_reason;
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" photo shown for review (%2%)")) % label % review_reason).str());
        post_event(ev);
        return;
    }
    // First-layer checks measure each part's offset (calibration error); later checks start from it.
    const bool first_layer = cp->kind == FailureCheck::CheckpointKind::Early || cp->kind == FailureCheck::CheckpointKind::FirstLayer;
    FailureCheck::CheckResult res = FailureCheck::compare(*model, *cp, cam, base, base_z, cur, z,
                                                          first_layer ? FailureCheck::Alignment() : job.alignment, base_segments);
    if (stand_in_reference)
        res.reasons.push_back("compared with the previous job's empty-plate photo (this job has no reference photo)");
    // The view around the bed changed (a hand in the reference, say): the verdict is unsure. During the first layer,
    // when the plate holds little material, this photo becomes the reference for the rest of the job.
    if (res.reference_mismatch && first_layer) {
        job.reference_photo = "cp" + std::to_string(index) + ".jpg";
        job.reference_z     = z;
        job.reference_index = index;
        res.reasons.push_back("later checkpoints compare with this photo instead");
    }
    if (first_layer && res.alignment.valid())
        job.alignment = res.alignment;
    {
        nlohmann::json rj = {{"checkpoint", index}, {"label", label}, {"z", z}, {"unexpected_mm2", res.unexpected_mm2},
                             {"missing_fraction", res.missing_fraction}, {"reasons", res.reasons},
                             {"scene_change", res.scene_change}, {"reference_mismatch", res.reference_mismatch},
                             {"verdict", res.verdict == FailureCheck::Verdict::Pass ? "pass" : res.verdict == FailureCheck::Verdict::Fail ? "fail" : "unsure"}};
        write_file(job.dir + "/cp" + std::to_string(index) + ".json", rj.dump(2));
    }
    ev.has_result = true;
    ev.result     = res;
    ev.camera     = cam;
    switch (res.verdict) {
    case FailureCheck::Verdict::Pass:
        ++job.passed;
        shop_send_script(job.api, "SHOP_CHECK_OK");
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" passed (%2% of %3%)")) % label % job.passed % (job.total - 1)).str() +
                                 (stand_in_reference ? " " + _u8L("(reference: previous job's photo)") : std::string()));
        break;
    case FailureCheck::Verdict::Fail:
        shop_send_script(job.api, "SHOP_CHECK_HOLD");
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" FAILED, printer paused")) % label).str());
        ev.mode = FailureCheckEvent::Mode::Hold;
        post_event(ev);
        notify_hold(ev);
        break;
    case FailureCheck::Verdict::Unsure:
        // No popup: the check exists so nobody has to watch. An uncertain reading lets the print go on
        // and is noted in the status line and the job folder (cpN.jpg / cpN.json).
        ++job.passed;
        shop_send_script(job.api, "SHOP_CHECK_OK");
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" uncertain (%2%), print continues")) % label %
                              (res.reasons.empty() ? std::string() : res.reasons.front())).str());
        break;
    }
}

}} // namespace Slic3r::GUI
