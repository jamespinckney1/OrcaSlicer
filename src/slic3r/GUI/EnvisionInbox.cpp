#include "EnvisionInbox.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI { namespace EnvisionInbox {

namespace {

// The sidecar's name ends up in file and job names: no path separators or control characters, no leading
// dots, at most 120 bytes (cut on a UTF-8 boundary). Empty when nothing usable is left.
std::string sanitize_name(const std::string& raw)
{
    std::string out;
    for (char c : raw)
        if (c != '/' && c != '\\' && static_cast<unsigned char>(c) >= 0x20)
            out += c;
    boost::algorithm::trim(out);
    const size_t start = out.find_first_not_of('.');
    out                = start == std::string::npos ? std::string() : out.substr(start);
    if (out.size() > 120) {
        size_t cut = 120;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
    }
    boost::algorithm::trim(out);
    return out;
}

} // namespace

std::string inbox_dir()
{
    return (fs::path(data_dir()) / "shop" / "inbox").string();
}

bool is_inbox_file(const std::string& path)
{
    boost::system::error_code ec;
    const fs::path file = fs::weakly_canonical(fs::path(path), ec);
    if (ec)
        return false;
    const fs::path dir = fs::weakly_canonical(fs::path(inbox_dir()), ec);
    if (ec)
        return false;
    const std::string ext = boost::algorithm::to_lower_copy(file.extension().string());
    return file.parent_path() == dir && (ext == ".3mf" || ext == ".stl") && fs::is_regular_file(file, ec);
}

Request read_request(const std::string& model_path)
{
    Request req;
    req.model_path = model_path;
    const fs::path model(model_path);
    req.name = model.stem().string();
    const fs::path sidecar = model.parent_path() / (model.stem().string() + ".json");
    boost::system::error_code ec;
    if (fs::is_regular_file(sidecar, ec)) {
        try {
            boost::nowide::ifstream in(sidecar.string());
            const nlohmann::json j = nlohmann::json::parse(in);
            if (j.contains("name") && j["name"].is_string()) {
                const std::string name = sanitize_name(j["name"].get<std::string>());
                if (!name.empty())
                    req.name = name;
            }
            if (j.contains("machine") && j["machine"].is_string())
                req.machine = j["machine"].get<std::string>();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "EnvisionInbox: ignoring unreadable " << sidecar.string() << ": " << e.what();
        }
    }
    return req;
}

}}} // namespace Slic3r::GUI::EnvisionInbox
