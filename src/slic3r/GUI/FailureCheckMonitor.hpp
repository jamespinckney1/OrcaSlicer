#pragma once

// Print-shop addition: the slicer side of the G-code-aware failure check (see FailureCheck.hpp).
//
// When a job is uploaded with "Failure check" ticked, prepare_job() inserts the checkpoints, keeps a copy
// of the final G-code and starts watching the printer. A background thread polls Moonraker; when the
// printer is parked at a checkpoint it photographs the bed, compares the photo with the G-code and answers
// SHOP_CHECK_OK or SHOP_CHECK_HOLD. Watched jobs are saved, so a restarted slicer picks them up again.
//
// With a failure check service set (Preferences, scripts/shop_checkd), the job is handed to the service
// instead, which keeps checking while the slicer is closed. The slicer then only mirrors the service: its
// status line, and the Hold window (recomputed here from the service's photos, so it looks the same).
// A service that cannot be reached at upload leaves the job to the slicer, as before.

#include "FailureCheck.hpp"
#include "ShopNotify.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI {

// Host of a print-host setting such as "192.168.1.50", "http://192.168.1.50:7125/" or "host:7125".
std::string shop_printer_host(const std::string& print_host);
// Moonraker API roots worth trying for a print host, best first.
std::vector<std::string> shop_api_candidates(const std::string& print_host);
// The camera snapshot URL of a Moonraker printer (Qidi and Fluidd/Mainsail serve it on port 80).
std::string shop_snapshot_url(const std::string& print_host);
// First API root that answers, or empty.
std::string shop_resolve_api(const std::string& print_host);

bool load_shop_camera(const std::string& printer_host, FailureCheck::CameraModel& out);
bool save_shop_camera(const std::string& printer_host, const FailureCheck::CameraModel& cam);

// Runs a G-code script on the printer without waiting (Moonraker answers when the script has finished).
void shop_send_script(const std::string& api_root, const std::string& script);

// Decodes a JPEG/PNG into an RGB image (any thread).
bool shop_decode_image(const std::string& bytes, FailureCheck::Image& out);

struct FailureCheckEvent;

class FailureCheckMonitor
{
public:
    static FailureCheckMonitor& get();

    void start();
    void stop();

    // Called from the upload worker thread with the final G-code: inserts the checkpoints in place and
    // starts watching. On false the file is untouched and `message` says why (e.g. macros not installed).
    bool prepare_job(const std::string& print_host, const std::string& gcode_path, const std::string& upload_name, std::string& message);

    // One line for the Device tab, or empty when nothing is being watched on that printer.
    std::string status(const std::string& printer_host) const;

    // Rereads the service address and the ntfy settings from app_config. UI thread only.
    void configure();

    static constexpr const char* kServiceKey      = "shop_check_service";       // e.g. http://host:8765; empty = the slicer checks
    static constexpr const char* kServiceTokenKey = "shop_check_service_token"; // the service's API token

private:
    struct Job
    {
        std::string      id, host, api, job_name, dir, gcode;
        long long        created = 0;
        bool             started = false;
        std::vector<int> handled;
        bool             has_baseline = false;
        double           baseline_z   = 0;
        // A first-layer photo that replaced a reference which no longer matched the scene (a hand in view):
        // file name in dir, toolhead Z and checkpoint index. Empty: the reference is cp0.
        std::string      reference_photo;
        double           reference_z     = 0;
        int              reference_index = 0;
        int              total        = 0;
        int              passed       = 0;
        bool             camera_warned = false; // phone told the camera is not answering (once per job)
        bool             remote        = false; // checked by the service; this slicer only mirrors it
        FailureCheck::Alignment alignment; // per-part offsets measured on the first layer
    };

    FailureCheckMonitor() = default;
    void run();
    bool poll(Job& job); // false: stop watching
    bool hand_off(const Job& job, std::string& error);  // gives the job to the service
    bool service_get(const std::string& path, std::string& body); // GET <service><path> with the token
    std::set<std::string> poll_service(const std::vector<Job>& jobs); // mirrors the service; returns finished job ids
    void show_service_hold(const Job& job, int index, const std::string& label); // a held checkpoint's window
    void handle_checkpoint(Job& job, int index, double z);
    void set_status(const std::string& host, const std::string& text);
    void save_jobs_locked() const;
    void load_jobs();
    std::shared_ptr<const FailureCheck::GCodeModel> model_for(const Job& job);
    void post_event(const FailureCheckEvent& ev);
    void notify_hold(const FailureCheckEvent& ev); // phone notification, when a topic is set
    void notify(const ShopNotify::Message& msg);   // on the UI thread, which owns app_config

    mutable std::mutex                 m_mutex;
    std::condition_variable            m_cv;
    std::vector<Job>                   m_jobs;
    std::map<std::string, std::string> m_status;
    std::map<std::string, std::shared_ptr<const FailureCheck::GCodeModel>> m_models;
    std::shared_ptr<std::atomic<bool>> m_stop;
    std::thread                        m_thread;
    bool                               m_started = false;
    std::string                        m_service, m_service_token, m_ntfy_server, m_ntfy_topic; // copies of app_config (see configure)
    long long                          m_service_seq = -1; // last service event seen; -1 = not asked yet
};

}} // namespace Slic3r::GUI
