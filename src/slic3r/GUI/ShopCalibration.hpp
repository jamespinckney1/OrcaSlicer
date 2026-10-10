#pragma once

// Print-shop addition: a log of calibration results per machine and filament (pressure advance,
// flow ratio, max volumetric speed, temperature), with one-click apply to the filament preset.
// Persisted as JSON under data_dir()/shop/calibration_results.json.

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct CalibResult
{
    std::string printer_preset;   // printer preset name the test was run on
    std::string filament_preset;  // filament preset name
    std::string machine;          // optional shop machine name
    double      pressure_advance = -1;  // <0 = not set
    double      flow_ratio       = -1;
    double      max_volumetric   = -1;  // mm3/s
    int         temperature      = -1;  // nozzle C
    std::string notes;
    std::string date;             // YYYY-MM-DD
};

class ShopCalibration
{
public:
    static ShopCalibration& get();

    const std::vector<CalibResult>& results() const { return m_results; }
    std::vector<CalibResult>&       results() { return m_results; }

    void load();
    bool save() const;
    static std::string path();
    static std::string today();

private:
    ShopCalibration() = default;
    std::vector<CalibResult> m_results;
    bool m_loaded = false;
};

}} // namespace Slic3r::GUI
