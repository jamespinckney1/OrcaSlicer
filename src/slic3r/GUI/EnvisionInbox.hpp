#pragma once

// Print-shop addition: hand-off folder for 3D Envision's "Send to Slicer".
//
// 3D Envision writes a model (.3mf or .stl) into data_dir()/shop/inbox together with an optional
// sidecar "<same name>.json" ({"name": "...", "machine": "..."}) and asks macOS to open the model
// in OrcaSlicer. Files that arrive from this folder open as a new project named after the part;
// files from anywhere else open exactly as before.

#include <string>

namespace Slic3r { namespace GUI { namespace EnvisionInbox {

struct Request
{
    std::string model_path;
    std::string name;    // project name; falls back to the file name
    std::string machine; // optional shop machine name (see ShopPrinters)
};

std::string inbox_dir();
bool        is_inbox_file(const std::string& path);
Request     read_request(const std::string& model_path);

}}} // namespace Slic3r::GUI::EnvisionInbox
