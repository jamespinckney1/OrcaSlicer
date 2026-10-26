#pragma once

// Print-shop addition: Quick restart windows (see ShopQuickRestart.hpp).

#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

// Offers a quick restart of the print paused on the printer at `api_root` (a Moonraker API root): checks it is
// possible, asks, prepares the printer (SHOP_QUICK_RESTART) and shows the "clear the plate" banner whose Resume
// prints the job again from layer 1. True once the printer is prepared. UI thread only.
bool show_quick_restart(wxWindow* parent, const std::string& api_root, const std::string& job_name);

}} // namespace Slic3r::GUI
