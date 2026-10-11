#include "FailureCheckMonitor.hpp"

#include "FailureCheckDialogs.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/image.h>
#include <wx/mstream.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
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
    wxMemoryInputStream stream(bytes.data(), bytes.size());
    wxImage             img;
    if (!img.LoadFile(stream, wxBITMAP_TYPE_ANY) || !img.IsOk())
        return false;
    out.w = img.GetWidth();
    out.h = img.GetHeight();
    out.rgb.assign(img.GetData(), img.GetData() + size_t(out.w) * out.h * 3);
    return true;
}

// ---- monitor ------------------------------------------------------------------------------------

FailureCheckMonitor& FailureCheckMonitor::get()
{
    // Never destroyed: stop() may leave the thread finishing a request after the app has quit.
    static FailureCheckMonitor* instance = new FailureCheckMonitor();
    return *instance;
}

void FailureCheckMonitor::start()
{
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
                       {"baseline_z", j.baseline_z}, {"total", j.total}, {"passed", j.passed}});
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
            j.total        = o.value("total", 0);
            j.passed       = o.value("passed", 0);
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const Job& j) { return j.host == job.host; }), m_jobs.end());
        m_jobs.push_back(job);
        save_jobs_locked();
        m_status[job.host] = (boost::format(_u8L("Failure check: %1% checkpoints in %2%, waiting for the print to start")) %
                              (job.total - 1) % job.job_name).str();
    }
    m_cv.notify_all();
    message = (boost::format(_u8L("Failure check: %1% camera checkpoints added.")) % (model.checkpoints.size() - 1)).str();
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
        for (Job& job : jobs) {
            if (stop->load())
                return;
            const bool keep = poll(job);
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
                      Http::url_encode("gcode_macro SHOP_CHECKPOINT") + "=waiting,index,held",
                  body, 5))
        return true; // printer unreachable: try again
    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return true;
    std::string state, filename;
    int         waiting = 0, index = -1;
    double      z       = 0;
    try {
        const auto& st = j.at("result").at("status");
        state          = st.at("print_stats").value("state", std::string());
        filename       = st.at("print_stats").value("filename", std::string());
        if (st.contains("gcode_macro SHOP_CHECKPOINT")) {
            waiting = st["gcode_macro SHOP_CHECKPOINT"].value("waiting", 0);
            index   = st["gcode_macro SHOP_CHECKPOINT"].value("index", -1);
        }
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
    if (active && waiting == 1 && index >= 0 && std::find(job.handled.begin(), job.handled.end(), index) == job.handled.end())
        handle_checkpoint(job, index, z);
    return true;
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
    std::string       jpeg;
    if (!http_get(shop_snapshot_url(job.host), jpeg, 10)) {
        shop_send_script(job.api, "SHOP_CHECK_OK"); // fail open
        set_status(job.host, (boost::format(_u8L("Failure check: camera did not answer at \"%1%\"; print continues")) % label).str());
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
    std::string               base_bytes;
    const bool comparable = load_shop_camera(job.host, cam) && job.has_baseline && read_file(job.dir + "/cp0.jpg", base_bytes) &&
                            shop_decode_image(base_bytes, base) && shop_decode_image(jpeg, cur) && base.w == cur.w && base.h == cur.h &&
                            cur.w == cam.width && cur.h == cam.height;
    if (!comparable) {
        // Not calibrated (or no reference photo): let the user judge, resuming after a countdown.
        ev.mode = FailureCheckEvent::Mode::Review;
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" photo shown for review")) % label).str());
        post_event(ev);
        return;
    }
    const FailureCheck::CheckResult res = FailureCheck::compare(*model, *cp, cam, base, job.baseline_z, cur, z);
    {
        nlohmann::json rj = {{"checkpoint", index}, {"label", label}, {"z", z}, {"unexpected_mm2", res.unexpected_mm2},
                             {"missing_fraction", res.missing_fraction}, {"reasons", res.reasons},
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
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" passed (%2% of %3%)")) % label % job.passed % (job.total - 1)).str());
        break;
    case FailureCheck::Verdict::Fail:
        shop_send_script(job.api, "SHOP_CHECK_HOLD");
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" FAILED, printer paused")) % label).str());
        ev.mode = FailureCheckEvent::Mode::Hold;
        post_event(ev);
        break;
    case FailureCheck::Verdict::Unsure:
        shop_send_script(job.api, "SHOP_CHECK_OK");
        set_status(job.host, (boost::format(_u8L("Failure check: \"%1%\" uncertain, print continues")) % label).str());
        ev.mode = FailureCheckEvent::Mode::Notice;
        post_event(ev);
        break;
    }
}

}} // namespace Slic3r::GUI
