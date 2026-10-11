#pragma once

// Print-shop addition: the slicer side of the G-code-aware failure check (see FailureCheck.hpp).
//
// When a job is uploaded with "Failure check" ticked, prepare_job() inserts the checkpoints, keeps a copy
// of the final G-code and starts watching the printer. A background thread polls Moonraker; when the
// printer is parked at a checkpoint it photographs the bed, compares the photo with the G-code and answers
// SHOP_CHECK_OK or SHOP_CHECK_HOLD. Watched jobs are saved, so a restarted slicer picks them up again.

#include "FailureCheck.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
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

private:
    struct Job
    {
        std::string      id, host, api, job_name, dir, gcode;
        long long        created = 0;
        bool             started = false;
        std::vector<int> handled;
        bool             has_baseline = false;
        double           baseline_z   = 0;
        int              total        = 0;
        int              passed       = 0;
        FailureCheck::Alignment alignment; // per-part offsets measured on the first layer
    };

    FailureCheckMonitor() = default;
    void run();
    bool poll(Job& job); // false: stop watching
    void handle_checkpoint(Job& job, int index, double z);
    void set_status(const std::string& host, const std::string& text);
    void save_jobs_locked() const;
    void load_jobs();
    std::shared_ptr<const FailureCheck::GCodeModel> model_for(const Job& job);
    void post_event(const FailureCheckEvent& ev);

    mutable std::mutex                 m_mutex;
    std::condition_variable            m_cv;
    std::vector<Job>                   m_jobs;
    std::map<std::string, std::string> m_status;
    std::map<std::string, std::shared_ptr<const FailureCheck::GCodeModel>> m_models;
    std::shared_ptr<std::atomic<bool>> m_stop;
    std::thread                        m_thread;
    bool                               m_started = false;
};

}} // namespace Slic3r::GUI
