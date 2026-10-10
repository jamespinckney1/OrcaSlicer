#pragma once

// Print-shop addition: a small library of named machine start G-code snippets,
// saved per printer model, offered as a dropdown on the Machine G-code page.
// Persisted as JSON under data_dir()/shop/start_gcode_library.json.

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct StartGCodeSnippet
{
    std::string name;
    std::string printer_model; // empty = offered for every printer
    std::string gcode;
};

class StartGCodeLibrary
{
public:
    static StartGCodeLibrary& get();

    // Snippets saved for this printer model plus the model-independent ones.
    std::vector<const StartGCodeSnippet*> for_printer(const std::string& printer_model) const;
    const StartGCodeSnippet* find(const std::string& name, const std::string& printer_model) const;

    void add_or_replace(StartGCodeSnippet snippet);
    bool remove(const std::string& name, const std::string& printer_model);

    void load();
    bool save() const;
    static std::string path();

private:
    StartGCodeLibrary() = default;
    std::vector<StartGCodeSnippet> m_snippets;
    bool m_loaded = false;
};

}} // namespace Slic3r::GUI
