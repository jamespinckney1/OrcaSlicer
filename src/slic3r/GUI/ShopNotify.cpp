#include "ShopNotify.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <cctype>
#include <ctime>
#include <utility>

namespace Slic3r { namespace GUI { namespace ShopNotify {

bool valid_topic(const std::string& topic)
{
    if (topic.empty() || topic.size() > 64)
        return false;
    for (char c : topic)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'))
            return false;
    return true;
}

std::string topic_url(const std::string& server, const std::string& topic)
{
    const std::string t = boost::algorithm::trim_copy(topic);
    if (!valid_topic(t))
        return {};
    std::string s = boost::algorithm::trim_copy(server);
    if (s.empty())
        s = kDefaultServer;
    if (s.find("://") == std::string::npos)
        s = "https://" + s;
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s + "/" + t;
}

namespace {

using DoneFn = std::function<void(const std::string&)>;

// Title, tags and priority go in the query string so non-ASCII job names survive (ntfy reads
// every header as a query parameter too).
std::string query(const Message& msg, bool with_body)
{
    std::string q;
    auto add = [&](const char* key, const std::string& value) {
        if (!value.empty())
            q += (q.empty() ? "?" : "&") + std::string(key) + "=" + Http::url_encode(value);
    };
    add("title", msg.title);
    add("tags", msg.tags);
    add("priority", msg.priority);
    if (with_body)
        add("message", msg.body);
    return q;
}

void post_text(const std::string& url, const Message& msg, DoneFn done)
{
    Http::post(url + query(msg, false))
        .header("Content-Type", "text/plain; charset=utf-8")
        .set_post_body(msg.body)
        .timeout_connect(5)
        .timeout_max(30)
        .on_complete([done](std::string, unsigned) {
            if (done)
                done({});
        })
        .on_error([url, done](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "Shop notify: " << url << " failed: " << status << " " << error << " " << body;
            if (done)
                done(status ? "HTTP " + std::to_string(status) + " " + body : error);
        })
        .perform();
}

} // namespace

void send_to(const std::string& server, const std::string& topic, const Message& msg, DoneFn done)
{
    const std::string url = topic_url(server, topic);
    if (url.empty()) {
        if (done)
            done("no valid topic");
        return;
    }
    boost::system::error_code ec;
    if (msg.attachment_path.empty() || !boost::filesystem::is_regular_file(msg.attachment_path, ec)) {
        post_text(url, msg, std::move(done));
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "Shop notify: sending \"" << msg.title << "\" with " << msg.attachment_path;
    Http::put(url + query(msg, true))
        .header("Filename", boost::filesystem::path(msg.attachment_path).filename().string())
        .set_put_body(boost::filesystem::path(msg.attachment_path))
        .timeout_connect(5)
        .timeout_max(60)
        .on_complete([done](std::string, unsigned) {
            if (done)
                done({});
        })
        .on_error([url, msg, done](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "Shop notify: attachment to " << url << " failed (" << status << " " << error << " " << body
                                       << "), sending text only";
            post_text(url, msg, done);
        })
        .perform();
}

std::string latest_checkpoint_photo()
{
    namespace fs = boost::filesystem;
    std::string               best;
    std::time_t               best_time = 0;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(data_dir() + "/shop/failure_check/jobs", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!boost::algorithm::starts_with(name, "cp") || it->path().extension() != ".jpg")
            continue;
        const std::time_t t = fs::last_write_time(it->path(), ec);
        if (!ec && t >= best_time) {
            best      = it->path().string();
            best_time = t;
        }
        ec.clear();
    }
    return best;
}

void send(const Message& msg)
{
    const AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || boost::algorithm::trim_copy(cfg->get(kTopicKey)).empty())
        return;
    send_to(cfg->get(kServerKey), cfg->get(kTopicKey), msg);
}

}}} // namespace Slic3r::GUI::ShopNotify
