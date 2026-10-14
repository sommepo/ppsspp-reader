// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include "reader/save_sync/remote.h"

namespace SaveSync {

namespace {

using Json = nlohmann::json;

/// How long an address may take to answer at all. A device away from home waits this long
/// for the home network's address before the game starts.
constexpr auto ConnectTimeout = std::chrono::milliseconds(2500);
/// How long a transfer may stand still. Not its whole duration: a large save over a slow
/// connection takes as long as it takes.
constexpr auto TransferTimeout = std::chrono::seconds(20);

std::string Escaped(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.') {
            out.push_back(c);
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", byte);
            out += buffer;
        }
    }
    return out;
}

std::string_view Trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '/')) {
        text.remove_suffix(1);
    }
    return text;
}

Version VersionOf(const Json& json) {
    Version version;
    version.n = json.value("n", 0);
    version.hash = json.value("hash", std::string{});
    version.size = json.value("size", std::uint64_t{0});
    version.files = json.value("files", std::size_t{0});
    version.device = json.value("device", std::string{});
    version.time_ms = json.value("time", std::int64_t{0});
    version.saved_ms = json.value("saved", std::int64_t{0});
    version.kind = json.value("kind", std::string{});
    return version;
}

std::string ErrorOf(const std::string& body, int status) {
    const Json json = Json::parse(body, nullptr, false);
    if (json.is_object() && json.contains("error") && json["error"].is_string()) {
        return json["error"].get<std::string>();
    }
    return "The service answered with status " + std::to_string(status);
}

} // namespace

std::optional<Address> ParseAddress(std::string_view text) {
    text = Trimmed(text);
    for (const std::string_view scheme : {"http://", "HTTP://"}) {
        if (text.substr(0, scheme.size()) == scheme) {
            text.remove_prefix(scheme.size());
        }
    }
    text = Trimmed(text);
    if (text.empty() || text.find_first_of(" /?#@") != std::string_view::npos) {
        return std::nullopt;
    }
    Address address;
    std::string_view port;
    if (text.front() == '[') {
        // An IPv6 address in brackets.
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        address.host = std::string{text.substr(1, close - 1)};
        const std::string_view rest = text.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return std::nullopt;
            }
            port = rest.substr(1);
        }
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon != std::string_view::npos && text.find(':') == colon) {
            address.host = std::string{text.substr(0, colon)};
            port = text.substr(colon + 1);
            if (port.empty()) {
                return std::nullopt;
            }
        } else {
            address.host = std::string{text};
        }
    }
    if (!port.empty()) {
        const auto [end, error] =
            std::from_chars(port.data(), port.data() + port.size(), address.port);
        if (error != std::errc{} || end != port.data() + port.size() || address.port < 1 ||
            address.port > 65535) {
            return std::nullopt;
        }
    }
    if (address.host.empty()) {
        return std::nullopt;
    }
    return address;
}

std::vector<Address> ParseAddresses(std::string_view text) {
    std::vector<Address> addresses;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find_first_of(",; \n\t", start), text.size());
        if (const auto address = ParseAddress(text.substr(start, end - start))) {
            const bool known = std::any_of(addresses.begin(), addresses.end(), [&](const Address& a) {
                return a.host == address->host && a.port == address->port;
            });
            if (!known) {
                addresses.push_back(*address);
            }
        }
        start = end + 1;
    }
    return addresses;
}

std::string TitleText(std::uint64_t program_id) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(program_id));
    return buffer;
}

Remote::Remote(std::vector<Address> addresses_, std::string code_, std::string device_)
    : addresses{std::move(addresses_)}, code{std::move(code_)}, device{std::move(device_)} {}

std::string Remote::LastAddress() const {
    std::scoped_lock lock{mutex};
    return last_address;
}

RemoteError Remote::Request(const char* method, const std::string& path, const std::string* body,
                            Reply& reply) {
    if (addresses.empty()) {
        return {Failure::Offline, "No address of the service is set"};
    }
    std::size_t first;
    {
        std::scoped_lock lock{mutex};
        first = preferred % addresses.size();
    }
    const httplib::Headers headers{{"Authorization", "Bearer " + code}};
    const std::string_view verb{method};
    std::string trouble;
    for (std::size_t turn = 0; turn < addresses.size(); ++turn) {
        const std::size_t index = (first + turn) % addresses.size();
        const Address& address = addresses[index];
        httplib::Client client{address.host, address.port};
        client.set_connection_timeout(ConnectTimeout);
        client.set_read_timeout(TransferTimeout);
        client.set_write_timeout(TransferTimeout);
        client.set_keep_alive(false);
        httplib::Result result = [&] {
            if (verb == "PUT") {
                return client.Put(path, headers, body->data(), body->size(), "application/zip");
            }
            if (verb == "POST") {
                return client.Post(path, headers, std::string{}, "application/json");
            }
            return client.Get(path, headers);
        }();
        if (!result) {
            trouble = address.Text() + ": " + httplib::to_string(result.error());
            continue;
        }
        reply.status = result->status;
        reply.body = std::move(result->body);
        {
            std::scoped_lock lock{mutex};
            preferred = index;
            last_address = address.Text();
        }
        if (reply.status == 401) {
            return {Failure::Unauthorised, "The code is not the service's"};
        }
        if (reply.status == 409) {
            return {Failure::Conflict, ErrorOf(reply.body, reply.status)};
        }
        if (reply.status < 200 || reply.status >= 300) {
            return {Failure::Refused, ErrorOf(reply.body, reply.status)};
        }
        return {};
    }
    return {Failure::Offline, "The service cannot be reached (" + trouble + ")"};
}

RemoteError Remote::Ping() {
    Reply reply;
    if (const RemoteError error = Request("GET", "/v1/ping", nullptr, reply)) {
        return error;
    }
    const Json json = Json::parse(reply.body, nullptr, false);
    if (!json.is_object() || json.value("service", std::string{}) != "eden-reader-savesync") {
        return {Failure::Refused, "What answers at this address is not the save sync service"};
    }
    return {};
}

RemoteError Remote::PutStats(const std::string& install, const std::string& json) {
    Reply reply;
    return Request("PUT", "/v1/stats/" + install, &json, reply);
}

RemoteError Remote::Stats(std::string& json) {
    Reply reply;
    if (const RemoteError error = Request("GET", "/v1/stats", nullptr, reply)) {
        return error;
    }
    json = std::move(reply.body);
    return {};
}

RemoteError Remote::List(std::vector<TitleInfo>& out) {
    Reply reply;
    if (const RemoteError error = Request("GET", "/v1/saves", nullptr, reply)) {
        return error;
    }
    const Json json = Json::parse(reply.body, nullptr, false);
    if (!json.is_object() || !json.contains("saves") || !json["saves"].is_array()) {
        return {Failure::Refused, "The service's answer cannot be read"};
    }
    out.clear();
    for (const Json& entry : json["saves"]) {
        TitleInfo info;
        info.title = entry.value("title", std::string{});
        info.name = entry.value("name", std::string{});
        info.head = VersionOf(entry);
        out.push_back(std::move(info));
    }
    return {};
}

RemoteError Remote::Describe(const std::string& title, TitleInfo& out, const std::string& name) {
    Reply reply;
    const std::string path =
        "/v1/saves/" + title + (name.empty() ? std::string{} : "?name=" + Escaped(name));
    if (const RemoteError error = Request("GET", path, nullptr, reply)) {
        return error;
    }
    const Json json = Json::parse(reply.body, nullptr, false);
    if (!json.is_object() || !json.contains("versions") || !json["versions"].is_array()) {
        return {Failure::Refused, "The service's answer cannot be read"};
    }
    out = {};
    out.title = title;
    out.service_id = json.contains("id") && json["id"].is_string() ? json["id"].get<std::string>()
                                                                    : std::string{};
    out.name = json.value("name", std::string{});
    if (json.contains("head") && json["head"].is_object()) {
        out.head = VersionOf(json["head"]);
    }
    for (const Json& entry : json["versions"]) {
        out.versions.push_back(VersionOf(entry));
    }
    return {};
}

RemoteError Remote::Download(const std::string& title, int version, std::string& zip) {
    Reply reply;
    if (const RemoteError error =
            Request("GET", "/v1/saves/" + title + "/" + std::to_string(version), nullptr, reply)) {
        return error;
    }
    zip = std::move(reply.body);
    return {};
}

RemoteError Remote::Upload(const std::string& title, const std::string& name,
                           const Snapshot& snapshot, int base, UploadMode mode, int& version,
                           std::optional<Version>& head) {
    const char* mode_text =
        mode == UploadMode::Force ? "force" : mode == UploadMode::Side ? "side" : "normal";
    const std::string path = "/v1/saves/" + title + "?base=" + std::to_string(base) +
                             "&hash=" + snapshot.state.hash + "&device=" + Escaped(device) +
                             "&name=" + Escaped(name) +
                             "&saved=" + std::to_string(snapshot.state.newest_ms) +
                             "&mode=" + mode_text;
    Reply reply;
    const RemoteError error = Request("PUT", path, &snapshot.zip, reply);
    const Json json = Json::parse(reply.body, nullptr, false);
    if (error.failure == Failure::Conflict) {
        if (json.is_object() && json.contains("head") && json["head"].is_object()) {
            head = VersionOf(json["head"]);
        }
        return error;
    }
    if (error) {
        return error;
    }
    if (!json.is_object() || !json.contains("version")) {
        return {Failure::Refused, "The service's answer cannot be read"};
    }
    version = json.value("version", 0);
    return {};
}

RemoteError Remote::Restore(const std::string& title, int version, int& new_version) {
    Reply reply;
    const std::string path = "/v1/saves/" + title + "/" + std::to_string(version) +
                             "/restore?device=" + Escaped(device);
    if (const RemoteError error = Request("POST", path, nullptr, reply)) {
        return error;
    }
    const Json json = Json::parse(reply.body, nullptr, false);
    if (!json.is_object() || !json.contains("version")) {
        return {Failure::Refused, "The service's answer cannot be read"};
    }
    new_version = json.value("version", 0);
    return {};
}

} // namespace SaveSync
