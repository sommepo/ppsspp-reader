// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace SaveSync {

/// One of the folders a game's save is made of, and the name it has in an archive
/// ("user" for the profile's save, "device" for the one all profiles share).
struct Folder {
    std::string label;
    std::filesystem::path path;
};

/**
 * What tells two saves apart: a hash over the files' names, sizes and contents, in the order
 * of their names. Folders without files and the files' times do not count. The service
 * computes the same for what it is sent (tools/reader/savesync/server.py, state_hash).
 */
struct State {
    /// 64 hexadecimal digits; the same for every save without files.
    std::string hash;
    std::size_t files{};
    std::uint64_t size{};
    /// When the newest file was last written, in milliseconds since 1970 by this device's
    /// clock. 0 without files.
    std::int64_t newest_ms{};

    [[nodiscard]] bool Empty() const {
        return files == 0;
    }
};

/// A save as it travels: a zip archive whose names begin with the folders' labels.
struct Snapshot {
    State state;
    std::string zip;
};

/// A save could not be read, packed or put in place. The message is for the user.
class ArchiveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The state of the save in `folders`. Folders that do not exist are empty ones.
[[nodiscard]] State StateOf(const std::vector<Folder>& folders);

/// The save in `folders` as an archive, with its state.
[[nodiscard]] Snapshot Pack(const std::vector<Folder>& folders);

/// The state of the save an archive holds. Checks every file against its checksum; throws
/// for what is not an archive this code made.
[[nodiscard]] State StateOfArchive(std::string_view zip);

/// The folders' labels an archive has something for, each once.
[[nodiscard]] std::vector<std::string> LabelsOfArchive(std::string_view zip);

/**
 * Puts the save of an archive in place of the one in `folders`. The new files are written
 * beside the old folders and checked first; then each old folder is moved to `backup` (under
 * its label) and the new one takes its name, so that a failure on the way leaves the old save
 * where it was. A folder the archive has nothing for is moved away and not made again.
 */
void Unpack(std::string_view zip, const std::vector<Folder>& folders,
            const std::filesystem::path& backup);

/// Removes the oldest folders in `backups` until `keep` are left (they are named by time).
void PruneBackups(const std::filesystem::path& backups, std::size_t keep);

/// SHA-256 as 64 hexadecimal digits.
[[nodiscard]] std::string Sha256Hex(std::string_view data);

} // namespace SaveSync
