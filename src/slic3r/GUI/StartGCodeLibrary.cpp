#include "StartGCodeLibrary.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
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
    return (boost::filesystem::path(data_dir()) / "shop" / "start_gcode_library.json").string();
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
    const std::string file = path();
    if (!boost::filesystem::exists(file))
        return;
    try {
        boost::nowide::ifstream in(file);
        nlohmann::json j = nlohmann::json::parse(in);
        for (const auto& item : j.value("snippets", nlohmann::json::array())) {
            StartGCodeSnippet s;
            s.name          = item.value("name", "");
            s.printer_model = item.value("printer_model", "");
            s.gcode         = item.value("gcode", "");
            if (!s.name.empty())
                m_snippets.push_back(std::move(s));
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "StartGCodeLibrary: cannot read " << file << ": " << e.what();
    }
}

bool StartGCodeLibrary::save() const
{
    const std::string file = path();
    try {
        boost::filesystem::create_directories(boost::filesystem::path(file).parent_path());
        nlohmann::json j;
        j["version"]  = 1;
        j["snippets"] = nlohmann::json::array();
        for (const auto& s : m_snippets)
            j["snippets"].push_back({{"name", s.name}, {"printer_model", s.printer_model}, {"gcode", s.gcode}});
        boost::nowide::ofstream out(file);
        out << j.dump(2);
        return bool(out);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "StartGCodeLibrary: cannot write " << file << ": " << e.what();
        return false;
    }
}

}} // namespace Slic3r::GUI
