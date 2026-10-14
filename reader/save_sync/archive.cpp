// SPDX-FileCopyrightText: PPSSPP Reader fork contributors (from the Eden Reader fork)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>
#include <zlib.h>
#include "Common/Crypto/sha256.h"
#include "reader/save_sync/archive.h"

namespace SaveSync {

namespace {

namespace fs = std::filesystem;

// ---- SHA-256 ----

class Sha256 {
public:
    Sha256() {
        sha256_starts(&context);
    }

    void Update(std::string_view data) {
        // The emulator's routine counts a piece's length in 32 bits.
        constexpr std::size_t Piece = std::size_t{1} << 30;
        for (std::size_t at = 0; at < data.size(); at += Piece) {
            const std::size_t length = std::min(Piece, data.size() - at);
            sha256_update(&context, reinterpret_cast<const uint8_t*>(data.data() + at),
                          static_cast<uint32_t>(length));
        }
    }

    [[nodiscard]] std::array<unsigned char, 32> Finish() {
        std::array<unsigned char, 32> digest{};
        sha256_finish(&context, digest.data());
        return digest;
    }

private:
    sha256_context context{};
};

std::string Hex(const std::array<unsigned char, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const unsigned char byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}

/// Gathers a save's state file by file; the files must come in the order of their names.
class StateBuilder {
public:
    void Add(const std::string& name, std::string_view content) {
        Sha256 of_content;
        of_content.Update(content);
        const auto digest = of_content.Finish();
        total.Update(name);
        total.Update(std::string_view("\0", 1));
        total.Update(std::to_string(content.size()));
        total.Update(std::string_view("\0", 1));
        total.Update({reinterpret_cast<const char*>(digest.data()), digest.size()});
        ++state.files;
        state.size += content.size();
    }

    [[nodiscard]] State Finish(std::int64_t newest_ms) {
        state.hash = Hex(total.Finish());
        state.newest_ms = state.files == 0 ? 0 : newest_ms;
        return state;
    }

private:
    Sha256 total;
    State state;
};

// ---- the files of a save ----

std::string Utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

fs::path FromUtf8(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::int64_t WrittenMs(const fs::path& path) {
    std::error_code error;
    const auto written = fs::last_write_time(path, error);
    if (error) {
        return 0;
    }
    // The file clock's start is not the same everywhere: go by the distance from now.
    const auto ago = fs::file_time_type::clock::now() - written;
    const auto when = std::chrono::system_clock::now() -
                      std::chrono::duration_cast<std::chrono::system_clock::duration>(ago);
    return std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count();
}

struct Entry {
    /// As in the archive: the label, then the path below the folder; a folder's ends in '/'.
    std::string name;
    fs::path path;
    bool folder{};
};

/// Everything in the folders, in the order of the names.
std::vector<Entry> Entries(const std::vector<Folder>& folders) {
    std::vector<Entry> entries;
    for (const Folder& folder : folders) {
        std::error_code error;
        if (!fs::is_directory(folder.path, error)) {
            continue;
        }
        entries.push_back({folder.label + "/", folder.path, true});
        fs::recursive_directory_iterator walk{folder.path, error};
        if (error) {
            throw ArchiveError("The save folder cannot be read: " + error.message());
        }
        for (const fs::recursive_directory_iterator end; walk != end; walk.increment(error)) {
            if (error) {
                throw ArchiveError("The save folder cannot be read: " + error.message());
            }
            const fs::directory_entry& found = *walk;
            if (found.is_symlink(error)) {
                walk.disable_recursion_pending();
                continue;
            }
            const std::string name =
                folder.label + "/" + Utf8(found.path().lexically_relative(folder.path));
            if (found.is_directory(error)) {
                entries.push_back({name + "/", found.path(), true});
            } else if (found.is_regular_file(error)) {
                entries.push_back({name, found.path(), false});
            }
        }
        // A folder that could not be gone through would make a save with files missing.
        if (error) {
            throw ArchiveError("The save folder cannot be read: " + error.message());
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return entries;
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        throw ArchiveError("A save file cannot be read: " + Utf8(path.filename()));
    }
    std::string content;
    std::array<char, 1 << 16> block;
    while (in.read(block.data(), static_cast<std::streamsize>(block.size())) || in.gcount() > 0) {
        content.append(block.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (in.bad()) {
        throw ArchiveError("A save file cannot be read: " + Utf8(path.filename()));
    }
    return content;
}

void WriteFile(const fs::path& path, std::string_view content) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
        throw ArchiveError("A save file cannot be written: " + Utf8(path.filename()));
    }
}

// ---- zip ----

constexpr std::uint32_t LocalSignature = 0x04034b50;
constexpr std::uint32_t CentralSignature = 0x02014b50;
constexpr std::uint32_t EndSignature = 0x06054b50;
constexpr std::uint16_t Utf8Names = 0x0800;
constexpr std::uint16_t Encrypted = 0x0001;
constexpr std::uint16_t Stored = 0;
constexpr std::uint16_t Deflated = 8;

void Put16(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void Put32(std::string& out, std::uint64_t value) {
    Put16(out, static_cast<std::uint32_t>(value & 0xffff));
    Put16(out, static_cast<std::uint32_t>((value >> 16) & 0xffff));
}

std::uint32_t Get16(std::string_view data, std::size_t at) {
    if (at + 2 > data.size()) {
        throw ArchiveError("The save archive is cut short");
    }
    return static_cast<std::uint8_t>(data[at]) | (static_cast<std::uint8_t>(data[at + 1]) << 8);
}

std::uint32_t Get32(std::string_view data, std::size_t at) {
    return Get16(data, at) | (Get16(data, at + 2) << 16);
}

std::string Deflate(std::string_view content) {
    z_stream stream{};
    if (deflateInit2(&stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw ArchiveError("The save cannot be compressed");
    }
    std::string out;
    out.resize(deflateBound(&stream, static_cast<uLong>(content.size())));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
    stream.avail_in = static_cast<uInt>(content.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const std::size_t written = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        throw ArchiveError("The save cannot be compressed");
    }
    out.resize(written);
    return out;
}

std::string Inflate(std::string_view packed, std::size_t size) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw ArchiveError("The save archive cannot be unpacked");
    }
    std::string out;
    out.resize(size);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.data()));
    stream.avail_in = static_cast<uInt>(packed.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&stream, Z_FINISH);
    const std::size_t written = stream.total_out;
    inflateEnd(&stream);
    if ((result != Z_STREAM_END && !(result == Z_BUF_ERROR && size == 0)) || written != size) {
        throw ArchiveError("The save archive is damaged");
    }
    return out;
}

std::uint32_t Crc(std::string_view content) {
    return static_cast<std::uint32_t>(
        crc32(crc32(0, nullptr, 0), reinterpret_cast<const Bytef*>(content.data()),
              static_cast<uInt>(content.size())));
}

/// A file's time as zip keeps it (to two seconds, from 1980).
std::pair<std::uint16_t, std::uint16_t> DosTime(std::int64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    if (parts.tm_year < 80) {
        return {0, static_cast<std::uint16_t>((1 << 5) | 1)};
    }
    const auto time =
        static_cast<std::uint16_t>((parts.tm_hour << 11) | (parts.tm_min << 5) | (parts.tm_sec / 2));
    const auto date = static_cast<std::uint16_t>(((parts.tm_year - 80) << 9) |
                                                 ((parts.tm_mon + 1) << 5) | parts.tm_mday);
    return {time, date};
}

class ZipWriter {
public:
    void Add(const std::string& name, std::string_view content, bool folder, std::int64_t ms) {
        if (name.size() > 0xffff || content.size() >= 0xffffffffULL || count >= 0xffff) {
            throw ArchiveError("The save is too large to be synced");
        }
        std::string packed;
        std::uint16_t method = Stored;
        if (!content.empty()) {
            packed = Deflate(content);
            if (packed.size() < content.size()) {
                method = Deflated;
            }
        }
        const std::string_view data = method == Deflated ? std::string_view{packed} : content;
        const std::uint32_t crc = Crc(content);
        const auto [time, date] = DosTime(ms);
        const std::size_t offset = out.size();

        const auto header = [&](std::string& to) {
            Put16(to, 20); // version needed
            Put16(to, Utf8Names);
            Put16(to, method);
            Put16(to, time);
            Put16(to, date);
            Put32(to, crc);
            Put32(to, data.size());
            Put32(to, content.size());
            Put16(to, static_cast<std::uint32_t>(name.size()));
            Put16(to, 0); // extra field
        };
        Put32(out, LocalSignature);
        header(out);
        out += name;
        out += data;

        Put32(central, CentralSignature);
        Put16(central, 20); // made by
        header(central);
        Put16(central, 0);                  // comment
        Put16(central, 0);                  // disk
        Put16(central, 0);                  // internal attributes
        Put32(central, folder ? 0x10 : 0);  // external attributes
        Put32(central, offset);
        central += name;
        ++count;
        if (out.size() + central.size() >= 0xffffffffULL) {
            throw ArchiveError("The save is too large to be synced");
        }
    }

    [[nodiscard]] std::string Finish() {
        const std::size_t offset = out.size();
        out += central;
        Put32(out, EndSignature);
        Put16(out, 0);
        Put16(out, 0);
        Put16(out, static_cast<std::uint32_t>(count));
        Put16(out, static_cast<std::uint32_t>(count));
        Put32(out, central.size());
        Put32(out, offset);
        Put16(out, 0);
        return std::move(out);
    }

private:
    std::string out;
    std::string central;
    std::size_t count{};
};

struct Packed {
    std::string name;
    bool folder{};
    std::string content;
};

/// A name as the archive may have it: below a label, with no way out of it.
void CheckName(const std::string& name) {
    std::size_t start = 0;
    const std::string_view body =
        !name.empty() && name.back() == '/' ? std::string_view{name}.substr(0, name.size() - 1)
                                            : std::string_view{name};
    bool fine = !body.empty() && body.find('\\') == std::string_view::npos &&
                body.find('\0') == std::string_view::npos;
    while (fine && start <= body.size()) {
        const std::size_t slash = std::min(body.find('/', start), body.size());
        const std::string_view part = body.substr(start, slash - start);
        fine = !part.empty() && part != "." && part != "..";
        start = slash + 1;
    }
    if (!fine) {
        throw ArchiveError("The save archive has a name that is not a save's");
    }
}

/// An archive's entries in the order of their names, unpacked and checked.
std::vector<Packed> ReadZip(std::string_view zip) {
    constexpr std::size_t EndSize = 22;
    if (zip.size() < EndSize) {
        throw ArchiveError("This is not a save archive");
    }
    std::size_t end = zip.size() - EndSize;
    const std::size_t lowest = zip.size() > EndSize + 0xffff ? zip.size() - EndSize - 0xffff : 0;
    while (Get32(zip, end) != EndSignature) {
        if (end == lowest) {
            throw ArchiveError("This is not a save archive");
        }
        --end;
    }
    const std::size_t count = Get16(zip, end + 10);
    const std::size_t central_size = Get32(zip, end + 12);
    std::size_t at = Get32(zip, end + 16);
    if (count == 0xffff || at == 0xffffffff || at + central_size > zip.size()) {
        throw ArchiveError("The save archive is of a kind that is not read");
    }

    std::vector<Packed> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (Get32(zip, at) != CentralSignature) {
            throw ArchiveError("The save archive is damaged");
        }
        const std::uint32_t flags = Get16(zip, at + 8);
        const std::uint32_t method = Get16(zip, at + 10);
        const std::uint32_t crc = Get32(zip, at + 16);
        const std::size_t packed_size = Get32(zip, at + 20);
        const std::size_t size = Get32(zip, at + 24);
        const std::size_t name_size = Get16(zip, at + 28);
        const std::size_t extra_size = Get16(zip, at + 30);
        const std::size_t comment_size = Get16(zip, at + 32);
        const std::size_t local = Get32(zip, at + 42);
        if (at + 46 + name_size > zip.size()) {
            throw ArchiveError("The save archive is cut short");
        }
        Packed entry;
        entry.name = std::string{zip.substr(at + 46, name_size)};
        at += 46 + name_size + extra_size + comment_size;

        if ((flags & Encrypted) != 0 || (method != Stored && method != Deflated) ||
            packed_size == 0xffffffff || size == 0xffffffff || local == 0xffffffff) {
            throw ArchiveError("The save archive is of a kind that is not read");
        }
        CheckName(entry.name);
        entry.folder = entry.name.back() == '/';
        if (Get32(zip, local) != LocalSignature) {
            throw ArchiveError("The save archive is damaged");
        }
        const std::size_t data = local + 30 + Get16(zip, local + 26) + Get16(zip, local + 28);
        if (data + packed_size > zip.size()) {
            throw ArchiveError("The save archive is cut short");
        }
        if (!entry.folder) {
            const std::string_view packed = zip.substr(data, packed_size);
            if (method == Deflated) {
                entry.content = Inflate(packed, size);
            } else if (packed_size == size) {
                entry.content = std::string{packed};
            } else {
                throw ArchiveError("The save archive is damaged");
            }
            if (Crc(entry.content) != crc) {
                throw ArchiveError("The save archive is damaged");
            }
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Packed& a, const Packed& b) { return a.name < b.name; });
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].name == entries[i - 1].name) {
            throw ArchiveError("The save archive has a name twice");
        }
    }
    return entries;
}

State StateOfEntries(const std::vector<Packed>& entries) {
    StateBuilder builder;
    for (const Packed& entry : entries) {
        if (!entry.folder) {
            builder.Add(entry.name, entry.content);
        }
    }
    return builder.Finish(0);
}

std::string_view LabelOf(std::string_view name) {
    return name.substr(0, name.find('/'));
}

/// Moves a folder, also to another drive.
void MoveFolder(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    fs::rename(from, to, error);
    if (!error) {
        return;
    }
    error.clear();
    fs::copy(from, to, fs::copy_options::recursive, error);
    if (error) {
        fs::remove_all(to);
        throw ArchiveError("The old save cannot be moved aside: " + error.message());
    }
    fs::remove_all(from, error);
    if (error) {
        throw ArchiveError("The old save cannot be moved aside: " + error.message());
    }
}

fs::path Beside(const fs::path& folder, const char* suffix) {
    fs::path beside = folder;
    beside += suffix;
    return beside;
}

} // namespace

std::string Sha256Hex(std::string_view data) {
    Sha256 hash;
    hash.Update(data);
    return Hex(hash.Finish());
}

State StateOf(const std::vector<Folder>& folders) {
    StateBuilder builder;
    std::int64_t newest = 0;
    for (const Entry& entry : Entries(folders)) {
        if (entry.folder) {
            continue;
        }
        builder.Add(entry.name, ReadFile(entry.path));
        newest = std::max(newest, WrittenMs(entry.path));
    }
    return builder.Finish(newest);
}

Snapshot Pack(const std::vector<Folder>& folders) {
    StateBuilder builder;
    ZipWriter writer;
    std::int64_t newest = 0;
    for (const Entry& entry : Entries(folders)) {
        const std::int64_t written = WrittenMs(entry.path);
        if (entry.folder) {
            writer.Add(entry.name, {}, true, written);
            continue;
        }
        const std::string content = ReadFile(entry.path);
        builder.Add(entry.name, content);
        writer.Add(entry.name, content, false, written);
        newest = std::max(newest, written);
    }
    Snapshot snapshot;
    snapshot.state = builder.Finish(newest);
    snapshot.zip = writer.Finish();
    return snapshot;
}

State StateOfArchive(std::string_view zip) {
    return StateOfEntries(ReadZip(zip));
}

std::vector<std::string> LabelsOfArchive(std::string_view zip) {
    std::vector<std::string> labels;
    for (const Packed& entry : ReadZip(zip)) {
        const std::string label{LabelOf(entry.name)};
        if (std::find(labels.begin(), labels.end(), label) == labels.end()) {
            labels.push_back(label);
        }
    }
    return labels;
}

void Unpack(std::string_view zip, const std::vector<Folder>& folders, const fs::path& backup) {
    const std::vector<Packed> entries = ReadZip(zip);
    const State wanted = StateOfEntries(entries);

    std::map<std::string, const Folder*, std::less<>> by_label;
    for (const Folder& folder : folders) {
        by_label.emplace(folder.label, &folder);
    }
    for (const Packed& entry : entries) {
        if (by_label.find(LabelOf(entry.name)) == by_label.end()) {
            throw ArchiveError("The save has a part this app has no place for: " +
                               std::string{LabelOf(entry.name)});
        }
    }

    // The new save, beside the old one.
    constexpr const char* NewSuffix = ".savesync-new";
    std::vector<Folder> staged;
    const auto discard = [&staged] {
        for (const Folder& folder : staged) {
            std::error_code error;
            fs::remove_all(folder.path, error);
        }
    };
    try {
        for (const Folder& folder : folders) {
            const fs::path stage = Beside(folder.path, NewSuffix);
            std::error_code error;
            fs::remove_all(stage, error);
            staged.push_back({folder.label, stage});
        }
        for (const Packed& entry : entries) {
            const std::string_view label = LabelOf(entry.name);
            const auto stage = std::find_if(staged.begin(), staged.end(),
                                            [&](const Folder& f) { return f.label == label; });
            std::string_view below = std::string_view{entry.name}.substr(label.size());
            while (!below.empty() && below.front() == '/') {
                below.remove_prefix(1);
            }
            const fs::path target = below.empty() ? stage->path : stage->path / FromUtf8(below);
            std::error_code error;
            fs::create_directories(entry.folder ? target : target.parent_path(), error);
            if (error) {
                throw ArchiveError("A save folder cannot be made: " + error.message());
            }
            if (!entry.folder) {
                WriteFile(target, entry.content);
            }
        }
        // Two names that differ only in the case of their letters are one file on Windows.
        if (StateOf(staged).hash != wanted.hash) {
            throw ArchiveError("The save cannot be kept on this device as it is "
                               "(its files' names do not go together here)");
        }
    } catch (...) {
        discard();
        throw;
    }

    // Old out, new in, folder by folder. What was moved is moved back when a step fails.
    std::vector<std::pair<fs::path, fs::path>> moved; // where it is now, where it was
    try {
        for (std::size_t i = 0; i < folders.size(); ++i) {
            const fs::path& place = folders[i].path;
            std::error_code error;
            if (fs::exists(place, error)) {
                const fs::path aside = backup / FromUtf8(folders[i].label);
                MoveFolder(place, aside);
                moved.emplace_back(aside, place);
            }
            if (!fs::exists(staged[i].path, error)) {
                continue;
            }
            fs::create_directories(place.parent_path(), error);
            fs::rename(staged[i].path, place, error);
            if (error) {
                throw ArchiveError("The save cannot be put in place: " + error.message());
            }
        }
    } catch (...) {
        for (auto step = moved.rbegin(); step != moved.rend(); ++step) {
            std::error_code error;
            fs::remove_all(step->second, error);
            fs::rename(step->first, step->second, error);
            if (error) {
                error.clear();
                fs::copy(step->first, step->second, fs::copy_options::recursive, error);
            }
        }
        discard();
        throw;
    }
}

void PruneBackups(const fs::path& backups, std::size_t keep) {
    std::error_code error;
    std::vector<fs::path> found;
    for (fs::directory_iterator walk{backups, error}, end; !error && walk != end;
         walk.increment(error)) {
        if (walk->is_directory(error)) {
            found.push_back(walk->path());
        }
    }
    std::sort(found.begin(), found.end());
    for (std::size_t i = 0; i + keep < found.size(); ++i) {
        fs::remove_all(found[i], error);
    }
}

} // namespace SaveSync
