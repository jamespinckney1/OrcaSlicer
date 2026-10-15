#include "StartGCodeLibrary.hpp"
#include "ShopFiles.hpp"

#include <optional>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

StartGCodeLibrary& StartGCodeLibrary::get()
{
    static StartGCodeLibrary instance;
    if (!instance.m_loaded)
        instance.load();
    return instance;
}

std::string StartGCodeLibrary::path()
{
    return shop_path("start_gcode_library.json").string();
}

std::vector<const StartGCodeSnippet*> StartGCodeLibrary::for_printer(const std::string& printer_model) const
{
    std::vector<const StartGCodeSnippet*> out;
    for (const auto& s : m_snippets)
        if (s.printer_model.empty() || s.printer_model == printer_model)
            out.push_back(&s);
    return out;
}

const StartGCodeSnippet* StartGCodeLibrary::find(const std::string& name, const std::string& printer_model) const
{
    for (const auto& s : m_snippets)
        if (s.name == name && s.printer_model == printer_model)
            return &s;
    return nullptr;
}

void StartGCodeLibrary::add_or_replace(StartGCodeSnippet snippet)
{
    for (auto& s : m_snippets)
        if (s.name == snippet.name && s.printer_model == snippet.printer_model) {
            s = std::move(snippet);
            return;
        }
    m_snippets.push_back(std::move(snippet));
}

bool StartGCodeLibrary::remove(const std::string& name, const std::string& printer_model)
{
    for (auto it = m_snippets.begin(); it != m_snippets.end(); ++it)
        if (it->name == name && it->printer_model == printer_model) {
            m_snippets.erase(it);
            return true;
        }
    return false;
}

void StartGCodeLibrary::load()
{
    m_loaded = true;
    m_snippets.clear();
    const std::optional<nlohmann::json> j = read_json_file(path());
    if (!j || !j->is_object())
        return;
    auto snippets = j->find("snippets");
    if (snippets == j->end() || !snippets->is_array())
        return;
    for (const auto& item : *snippets) {
        if (!item.is_object())
            continue;
        StartGCodeSnippet s;
        s.name          = json_get(item, "name", "");
        s.printer_model = json_get(item, "printer_model", "");
        s.gcode         = json_get(item, "gcode", "");
        if (!s.name.empty())
            m_snippets.push_back(std::move(s));
    }
}

bool StartGCodeLibrary::save() const
{
    nlohmann::json j;
    j["version"]  = 1;
    j["snippets"] = nlohmann::json::array();
    for (const auto& s : m_snippets)
        j["snippets"].push_back({{"name", s.name}, {"printer_model", s.printer_model}, {"gcode", s.gcode}});
    return write_json_file(path(), j);
}

}} // namespace Slic3r::GUI
