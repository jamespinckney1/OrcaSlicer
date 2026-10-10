#include "EnvisionInbox.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI { namespace EnvisionInbox {

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
            if (j.contains("name") && j["name"].is_string() && !j["name"].get<std::string>().empty())
                req.name = j["name"].get<std::string>();
            if (j.contains("machine") && j["machine"].is_string())
                req.machine = j["machine"].get<std::string>();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "EnvisionInbox: ignoring unreadable " << sidecar.string() << ": " << e.what();
        }
    }
    return req;
}

}}} // namespace Slic3r::GUI::EnvisionInbox
