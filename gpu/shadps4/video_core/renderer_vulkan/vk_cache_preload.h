// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "common/types.h"

namespace Vulkan::CachePreload {

/// gow3: the first use of a pipeline or a saved shader reads small files from the cache
/// (user/cache/<serial>). After a few idle seconds an NVMe drive can be in a power-saving
/// state and such a read takes about 34 ms, a visible hitch in the race. recent.txt there lists
/// the files the game used lately (paths relative to the cache, newest last); a low-priority
/// thread reads them into memory at start, and those uses take the bytes from there.
/// One track driven for several laps uses about 4,100 files and 31.6 MB; a session across
/// several tracks uses more. Past either cap the oldest listed files are left out and read
/// from the disk as before.
constexpr size_t MaxFiles = 8192;
constexpr size_t MaxBytes = size_t{64} << 20;
/// A list grown past this (no start ever got to trim it) is dropped and begins again.
constexpr size_t MaxListBytes = size_t{16} << 20;
constexpr std::string_view ListName = "recent.txt";

/// An entry of recent.txt: a relative path such as "pipeline-binaries/0a1b.bin" or
/// "0x0123456789abcdef_2.spv". Anything that could leave the cache is not turned into a path.
inline bool ValidEntry(std::string_view entry) {
    return !entry.empty() && entry.size() <= 128 && entry.front() != '/' &&
           entry.find("..") == std::string_view::npos &&
           entry.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-./") ==
               std::string_view::npos;
}

/// The last `limit` distinct valid entries of recent.txt, newest first.
inline std::vector<std::string> Recent(std::string_view list, size_t limit) {
    std::vector<std::string> entries;
    std::unordered_set<std::string_view> seen;
    size_t end = list.size();
    while (end > 0 && entries.size() < limit) {
        const size_t newline = list.rfind('\n', end - 1);
        const size_t first = newline == std::string_view::npos ? 0 : newline + 1;
        auto entry = list.substr(first, end - first);
        end = newline == std::string_view::npos ? 0 : newline;
        if (!entry.empty() && entry.back() == '\r') {
            entry.remove_suffix(1);
        }
        if (ValidEntry(entry) && seen.insert(entry).second) {
            entries.emplace_back(entry);
        }
    }
    return entries;
}

/// The preloaded bytes and the entries used this session, by list entry; the caller locks.
/// Stale bytes are never served because an entry in `used` is never (re)inserted into `bytes`,
/// and every writer calls Saved before writing: bytes read before the write are dropped or
/// refused, and a read after it finds none and goes to the disk.
class Store {
public:
    /// The preloaded bytes of `entry`, if there are any.
    const std::vector<u8>* Find(const std::string& entry) const {
        const auto it = bytes.find(entry);
        return it != bytes.end() ? &it->second : nullptr;
    }
    /// `entry` was read: listed for the next start.
    void Used(const std::string& entry) {
        if (used.insert(entry).second) {
            pending.push_back(entry);
        }
    }
    /// `entry` is about to be written: listed, and its old bytes are dropped.
    void Saved(const std::string& entry) {
        Used(entry);
        bytes.erase(entry);
    }
    /// Keeps the bytes read at start, unless `entry` was already used or saved.
    bool Keep(const std::string& entry, std::vector<u8>&& data) {
        if (used.contains(entry)) {
            return false;
        }
        bytes.emplace(entry, std::move(data));
        return true;
    }
    /// The entries used since the last call, each listed once.
    std::vector<std::string> TakePending() {
        return std::exchange(pending, {});
    }

private:
    std::unordered_map<std::string, std::vector<u8>> bytes;
    std::unordered_set<std::string> used;
    std::vector<std::string> pending; ///< used, not yet appended to the list
};

/// Starts the thread that trims recent.txt in the cache directory `root`, reads the listed
/// files and appends the files this session uses. With `read_list` false it starts from an
/// empty list and opens none of the listed files. Until then the functions below do nothing,
/// and they ignore files outside `root`. Only the first call does anything.
/// GOW3_CACHE_PRELOAD=0 starts nothing: no thread, no list, and the functions below do nothing.
void Start(const std::filesystem::path& root, bool read_list);
/// A copy of the preloaded bytes of `file`, if it was read at start.
std::optional<std::vector<u8>> Find(const std::filesystem::path& file);
/// `file` was read: listed for the next start.
void Used(const std::filesystem::path& file);
/// `file` is about to be written: listed, and its old bytes are no longer served from memory.
void Saved(const std::filesystem::path& file);

} // namespace Vulkan::CachePreload
