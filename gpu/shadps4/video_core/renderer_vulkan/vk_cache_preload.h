// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "common/types.h"

namespace Vulkan::CachePreload {

/// gow3: the first use of a pipeline or a saved shader reads small files from the cache
/// (user/cache/<serial>). After a few idle seconds an NVMe drive can be in a power-saving
/// state and such a read takes about 34 ms, a visible hitch in the race. recent.txt there lists
/// the files the game used lately (paths relative to the cache, newest last); a low-priority
/// thread reads them into memory at start, and those uses take the bytes from there.
/// A race uses about 1,500 pipeline binaries (3-5 KB) and 1,700 shader files (840 meta and
/// SPIR-V pairs, about 10 MB of SPIR-V), some 3,200 files and 15-20 MB: the caps hold about
/// two such sessions, well short of the whole shader store (61 MB of SPIR-V).
constexpr size_t MaxFiles = 6144;
constexpr size_t MaxBytes = size_t{40} << 20;
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

/// Starts the thread that trims recent.txt in the cache directory `root`, reads the listed
/// files and appends the files this session uses. Until then the functions below do nothing,
/// and they ignore files outside `root`.
/// GOW3_PIPELINE_PRELOAD=0 keeps the list but reads no file and serves nothing from memory.
void Start(const std::filesystem::path& root);
/// The preloaded bytes of `file`, if it was read at start.
std::optional<std::vector<u8>> Find(const std::filesystem::path& file);
/// `file` was read: listed for the next start.
void Used(const std::filesystem::path& file);
/// A new `file` was written: listed, and its old bytes are no longer served from memory.
void Saved(const std::filesystem::path& file);

} // namespace Vulkan::CachePreload
