#pragma once

// Print-shop addition: shared file helpers for the shop stores under data_dir()/shop/.
// Atomic writes (temp file + rename) so a crash or full disk cannot truncate a store, and a
// JSON reader that never throws and moves an unparsable file aside instead of letting the next
// save silently overwrite it.

#include <optional>
#include <string>
#include <type_traits>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace GUI {

// data_dir()/shop/<file_name>
boost::filesystem::path shop_path(const std::string& file_name);

// Writes bytes to "<path>.tmp" and renames it over path. Creates the parent directory.
// Returns false (and leaves path untouched) on any failure. Unlike Slic3r::write_file_atomically
// this never falls back to writing the target in place. Text mode (CRLF on Windows) unless binary.
bool write_file_atomic(const boost::filesystem::path& path, const std::string& bytes, bool binary = true);

// j.dump(2) written in text mode with write_file_atomic, as the stores always wrote. Returns false on any failure (including a dump
// that throws on invalid UTF-8).
bool write_json_file(const boost::filesystem::path& path, const nlohmann::json& j);

// nullopt if the file is missing or unreadable. A file that does not parse is renamed to
// "<name>.corrupt-YYYYmmdd-HHMMSS" and nullopt is returned. Never throws.
std::optional<nlohmann::json> read_json_file(const boost::filesystem::path& path);

// j[key] converted to T, or def if j is not an object, key is missing or has the wrong type.
// Never throws.
template<class T> T json_get(const nlohmann::json& j, const char* key, T def)
{
    try {
        if (!j.is_object())
            return def;
        auto it = j.find(key);
        if (it == j.end())
            return def;
        const nlohmann::json& v = *it;
        if constexpr (std::is_same_v<T, std::string>) {
            if (!v.is_string())
                return def;
        } else if constexpr (std::is_same_v<T, bool>) {
            if (!v.is_boolean())
                return def;
        } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
            // Any number converts, as json::value() did (a hand-edited 210.0 still reads as 210).
            if (!v.is_number())
                return def;
        }
        return v.get<T>();
    } catch (...) {
        return def;
    }
}

// So json_get(j, "name", "") yields a std::string rather than a const char*.
inline std::string json_get(const nlohmann::json& j, const char* key, const char* def)
{
    return json_get<std::string>(j, key, std::string(def));
}

}} // namespace Slic3r::GUI
