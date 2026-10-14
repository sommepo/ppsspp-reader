// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "reader/save_sync/archive.h"

namespace SaveSyncTest {

/// A folder of its own for a test, removed afterwards.
class Scratch {
public:
    Scratch() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("savesync-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path);
    }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    std::filesystem::path path;
};

inline void Write(const std::filesystem::path& file, const std::string& content) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out{file, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline std::string Read(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Every file below a folder with its content, by its path from there.
inline std::map<std::string, std::string> Tree(const std::filesystem::path& root) {
    std::map<std::string, std::string> files;
    if (!std::filesystem::exists(root)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
        if (entry.is_regular_file()) {
            files[entry.path().lexically_relative(root).generic_string()] = Read(entry.path());
        }
    }
    return files;
}

/// The two folders of a made-up game's save below `root`.
inline std::vector<SaveSync::Folder> Folders(const std::filesystem::path& root) {
    return {{"user", root / "user-save"}, {"device", root / "device-save"}};
}

} // namespace SaveSyncTest
