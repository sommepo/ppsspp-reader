// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "reader/save_sync/archive.h"

namespace SaveSync {

/// The port the service listens on unless an address names another.
constexpr int DefaultPort = 8642;

/// A version of a game's save, as the service describes it.
struct Version {
    int n{};
    std::string hash;
    std::uint64_t size{};
    std::size_t files{};
    /// The device that sent it.
    std::string device;
    /// When the service took it in, and when the device's newest file was written (each in
    /// milliseconds since 1970, the second by the device's clock).
    std::int64_t time_ms{};
    std::int64_t saved_ms{};
    /// "save", "side" (put aside by a device whose user chose another save) or "restore".
    std::string kind;
};

/// What the service has of a game.
struct TitleInfo {
    /// What tells the service that answered from another one (it makes it up once and keeps
    /// it). Empty from a service of before 2026-10-04.
    std::string service_id;
    std::string title;
    std::string name;
    /// The current version, or nothing when the service has none of this game.
    std::optional<Version> head;
    /// All versions it keeps, the newest first.
    std::vector<Version> versions;
};

/// How a save is sent.
enum class UploadMode {
    Normal, ///< Becomes the current version if the one it came from still is.
    Force,  ///< Becomes the current version: the user chose this device's save.
    Side,   ///< Is kept without becoming current: the user chose the other save.
};

/// Why a request did not do what was asked.
enum class Failure {
    None,
    Offline,      ///< None of the addresses answered.
    Unauthorised, ///< The code is not the service's.
    Conflict,     ///< Another device saved in between (an upload in Normal mode).
    Refused,      ///< The service answered with an error.
};

struct RemoteError {
    Failure failure{Failure::None};
    std::string message;

    explicit operator bool() const {
        return failure != Failure::None;
    }
};

/// A service's address as the user may write it: "host", "host:port" or "http://host:port".
struct Address {
    std::string host;
    int port{DefaultPort};

    [[nodiscard]] std::string Text() const {
        return host + ":" + std::to_string(port);
    }
};
[[nodiscard]] std::optional<Address> ParseAddress(std::string_view text);
/// Several addresses, separated by commas, semicolons or spaces. What is no address is left out.
[[nodiscard]] std::vector<Address> ParseAddresses(std::string_view text);

/// A game's ID as the service names it: 16 hexadecimal digits in capitals.
[[nodiscard]] std::string TitleText(std::uint64_t program_id);

/**
 * Talks to the service. The addresses are tried in turn, beginning with the one that answered
 * last time: the same service is reached by its Tailscale name from anywhere and by its
 * address in the home network from a device that is only there. Calls block; any thread.
 */
class Remote {
public:
    Remote(std::vector<Address> addresses_, std::string code_, std::string device_);

    /// Asks the service whether it is there and takes the code.
    RemoteError Ping();
    /// The current version of every game the service has.
    RemoteError List(std::vector<TitleInfo>& out);
    /// What the service has of a game. `name` is the game's name where this device knows it:
    /// the service takes it when it has none.
    RemoteError Describe(const std::string& title, TitleInfo& out, const std::string& name = {});
    RemoteError Download(const std::string& title, int version, std::string& zip);
    /**
     * Sends a save. `base` is the version the device's save came from (0: none). On success
     * `version` is the number it has on the service. With Failure::Conflict, `head` is the
     * service's current version.
     */
    RemoteError Upload(const std::string& title, const std::string& name, const Snapshot& snapshot,
                       int base, UploadMode mode, int& version, std::optional<Version>& head);
    /// Makes an older version the current one, as a new version.
    RemoteError Restore(const std::string& title, int version, int& new_version);

    /**
     * Sends what this install has counted of reading, whole (a JSON object with "days"): the
     * service keeps one record per install, under `install` (16 hexadecimal digits), and
     * replaces it each time.
     */
    RemoteError PutStats(const std::string& install, const std::string& json);
    /// Every install's record, as the service keeps them: {"devices": [...]}.
    RemoteError Stats(std::string& json);

    /// The address that answered last, for showing; empty before any did.
    [[nodiscard]] std::string LastAddress() const;

private:
    struct Reply {
        int status{};
        std::string body;
    };
    RemoteError Request(const char* method, const std::string& path, const std::string* body,
                        Reply& reply);

    std::vector<Address> addresses;
    std::string code;
    std::string device;
    mutable std::mutex mutex;
    std::size_t preferred{};
    std::string last_address;
};

} // namespace SaveSync
