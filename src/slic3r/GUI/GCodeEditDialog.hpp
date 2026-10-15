#pragma once

// Print-shop addition: view and edit the current plate's sliced G-code in place. Saving writes
// the plate's temp G-code file (the one Export and Send use); the caller re-processes it so the
// preview reflects the edit.

#include "GUI_Utils.hpp"
#include "Widgets/Label.hpp"

#include <string>
#include <wx/textctrl.h>
#include <wx/spinctrl.h>

namespace Slic3r { namespace GUI {

class GCodeEditDialog : public DPIDialog
{
public:
    GCodeEditDialog(wxWindow* parent, const std::string& gcode_path);

    bool saved() const { return m_saved; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void find_next();
    void go_to_layer();
    bool save();

    std::string  m_path;
    bool         m_saved = false;
    bool         m_load_failed = false; // file unreadable or not UTF-8: saving disabled
    wxTextCtrl*  m_editor = nullptr;
    wxTextCtrl*  m_find = nullptr;
    wxSpinCtrl*  m_layer = nullptr;
    Label*       m_status = nullptr;
    long         m_last_find_pos = 0;
};

}} // namespace Slic3r::GUI
