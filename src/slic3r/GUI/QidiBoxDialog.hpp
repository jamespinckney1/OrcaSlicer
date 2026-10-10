#pragma once

// Print-shop addition: slot controls for a QIDI Box reached over Moonraker. Shows the slots
// the filament sync fetched and sends the printer's own macros (EXTRUDER_LOAD, EXTRUDER_UNLOAD,
// MULTI_COLOR_BOX_UNLOAD, MULTI_COLOR_READ_RFID) through the connected device.

#include "GUI_Utils.hpp"
#include "DeviceManager.hpp"
#include "Widgets/Label.hpp"

#include <wx/sizer.h>
#include <wx/scrolwin.h>

namespace Slic3r {
namespace GUI {

class QidiBoxDialog : public DPIDialog
{
public:
    QidiBoxDialog(wxWindow* parent, MachineObject* obj);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void refresh(bool fetch);
    void rebuild_rows();
    void send(const wxString& what, const std::string& gcode, bool confirm);

    MachineObject*     m_obj = nullptr;
    wxScrolledWindow*  m_rows_panel = nullptr;
    wxBoxSizer*        m_rows_sizer = nullptr;
    Label*             m_status = nullptr;
};

}} // namespace Slic3r::GUI
