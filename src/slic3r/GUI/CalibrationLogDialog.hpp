#pragma once

// Print-shop addition: browse, record and apply calibration results per printer and filament.

#include "GUI_Utils.hpp"
#include "ShopCalibration.hpp"
#include "Widgets/Label.hpp"

#include <wx/listctrl.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

class CalibrationLogDialog : public DPIDialog
{
public:
    explicit CalibrationLogDialog(wxWindow* parent);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void fill_list(int select = -1);
    void load_fields(int idx);
    void add_from_current();
    void save_fields();
    void apply_to_filament();
    void remove_selected();
    int  selected() const;

    wxListCtrl* m_list = nullptr;
    wxTextCtrl* m_printer = nullptr;
    wxTextCtrl* m_filament = nullptr;
    wxTextCtrl* m_machine = nullptr;
    wxTextCtrl* m_pa = nullptr;
    wxTextCtrl* m_flow = nullptr;
    wxTextCtrl* m_mvs = nullptr;
    wxTextCtrl* m_temp = nullptr;
    wxTextCtrl* m_notes = nullptr;
    Label*      m_status = nullptr;
};

}} // namespace Slic3r::GUI
