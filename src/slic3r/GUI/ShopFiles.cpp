#include "ShopFiles.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <ctime>
#include <exception>
#include <ios>
#include <system_error>

namespace Slic3r { namespace GUI {

boost::filesystem::path shop_path(const std::string& file_name)
{
    return boost::filesystem::path(data_dir()) / "shop" / file_name;
}

bool write_file_atomic(const boost::filesystem::path& path, const std::string& bytes, bool binary)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "write_file_atomic: cannot create " << parent.string() << ": " << ec.message();
            return false;
        }
    }
    fs::path tmp = path;
    tmp += ".tmp";
    bool ok = false;
    try {
        const std::ios::openmode mode = std::ios::out | std::ios::trunc | (binary ? std::ios::binary : std::ios::openmode(0));
        boost::nowide::ofstream  out(tmp.string(), mode);
        if (out) {
            out.write(bytes.data(), std::streamsize(bytes.size()));
            out.flush();
            ok = bool(out);
            out.close();
            ok = ok && !out.fail();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "write_file_atomic: cannot write " << tmp.string() << ": " << e.what();
        ok = false;
    }
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "write_file_atomic: writing " << tmp.string() << " failed";
        fs::remove(tmp, ec);
        return false;
    }
    // rename_file retries while Windows briefly locks the target
    if (const std::error_code rec = rename_file(tmp.string(), path.string())) {
        BOOST_LOG_TRIVIAL(error) << "write_file_atomic: cannot rename " << tmp.string() << " to " << path.string() << ": "
                                 << rec.message();
        boost::system::error_code ec2;
        fs::remove(tmp, ec2);
        return false;
    }
    return true;
}

bool write_json_file(const boost::filesystem::path& path, const nlohmann::json& j)
{
    std::string bytes;
    try {
        bytes = j.dump(2);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "write_json_file: cannot serialize " << path.string() << ": " << e.what();
        return false;
    }
    return write_file_atomic(path, bytes, false);
}

std::optional<nlohmann::json> read_json_file(const boost::filesystem::path& path)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return std::nullopt;
    try {
        boost::nowide::ifstream in(path.string(), std::ios::in | std::ios::binary);
        if (!in) {
            BOOST_LOG_TRIVIAL(warning) << "read_json_file: cannot open " << path.string();
            return std::nullopt;
        }
        try {
            return nlohmann::json::parse(in);
        } catch (const nlohmann::json::parse_error& e) {
            std::time_t t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", std::localtime(&t));
            in.close();
            fs::path aside = path;
            aside += std::string(".corrupt-") + buf;
            fs::rename(path, aside, ec);
            if (ec)
                BOOST_LOG_TRIVIAL(warning) << "read_json_file: " << path.string() << " does not parse (" << e.what()
                                           << ") and could not be moved aside: " << ec.message();
            else
                BOOST_LOG_TRIVIAL(warning) << "read_json_file: " << path.string() << " does not parse (" << e.what()
                                           << "), moved to " << aside.string();
            return std::nullopt;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "read_json_file: cannot read " << path.string() << ": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "read_json_file: cannot read " << path.string();
    }
    return std::nullopt;
}

}} // namespace Slic3r::GUI
