#pragma once

// Print-shop addition: phone notifications through ntfy (https://ntfy.sh, or a self-hosted server).
// The failure check uses it so a held print reaches the phone, not only a window on the shop computer.
// Off unless a topic is set in Preferences; nothing is ever sent without one.

#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

namespace ShopNotify {

// app_config keys
constexpr const char* kTopicKey      = "shop_notify_topic";  // ntfy topic; empty = notifications off
constexpr const char* kServerKey     = "shop_notify_server"; // ntfy server; empty = kDefaultServer
constexpr const char* kDefaultServer = "https://ntfy.sh";

struct Message
{
    std::string title;
    std::string body;
    std::string tags;            // ntfy tags, e.g. "warning" (shown as emoji on the phone)
    std::string priority;        // "min", "low", "default", "high" or "urgent"; empty = default
    std::string attachment_path; // optional image sent with the message
};

// ntfy topics are 1-64 characters of A-Z a-z 0-9 _ -.
bool valid_topic(const std::string& topic);

// "<server>/<topic>", or empty when the topic is empty or invalid.
std::string topic_url(const std::string& server, const std::string& topic);

// Sends to an explicit server/topic in the background. `done` (optional) runs on a worker thread
// with an error message, empty on success. With an attachment that the server refuses (e.g. a
// self-hosted server without attachments), the message is sent again without it.
void send_to(const std::string& server, const std::string& topic, const Message& msg,
             std::function<void(const std::string& error)> done = {});

// The most recent failure-check photo (shop/failure_check/jobs/*/cpN.jpg), or empty. The test button
// sends it so the test looks like a real alert.
std::string latest_checkpoint_photo();

// Sends with the topic and server from Preferences; does nothing when the topic is empty.
// UI thread only (reads app_config).
void send(const Message& msg);

} // namespace ShopNotify
}} // namespace Slic3r::GUI
