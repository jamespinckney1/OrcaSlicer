#pragma once

// Print-shop addition: windows for the failure check (see FailureCheckMonitor.hpp).

#include "FailureCheck.hpp"

#include <memory>
#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

struct FailureCheckEvent
{
    enum class Mode {
        Hold,   // the check failed and the printer is held: Resume / Cancel print
        Notice, // uncertain: the print continues, the user should look
        Review  // no camera calibration yet: the user judges; resumes after a countdown
    };
    Mode        mode = Mode::Review;
    std::string host, api, job_name, label;
    int         index = 0, total = 0;
    std::string photo_path;
    double      z = 0;
    bool        has_result = false;
    FailureCheck::CheckResult                       result;
    FailureCheck::CameraModel                       camera;
    std::shared_ptr<const FailureCheck::GCodeModel> model;
    size_t                                          segments_done = 0;
};

// Shows (or replaces) the failure-check window. UI thread only.
void show_failure_check_event(const FailureCheckEvent& ev);

// Guided camera calibration for a Moonraker printer. UI thread only.
void show_camera_calibration(wxWindow* parent, const std::string& print_host);

// Camera calibration from a test print of dots (modeless; one at a time). UI thread only.
void show_camera_test_print_calibration(wxWindow* parent, const std::string& print_host);

}} // namespace Slic3r::GUI
