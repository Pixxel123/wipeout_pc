// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "common/types.h"

namespace Vulkan::PipelineBinaryPreload {

/// gow3: a restore reads one small file from pipeline-binaries/. After a few idle seconds an
/// NVMe drive can be in a power-saving state and that read takes about 34 ms, a visible hitch
/// in the race. recent.txt lists the files the game used lately (one name per line, newest
/// last); a low-priority thread reads them into memory at start, and restores take the bytes
/// from there. The caps bound the start-up reads and the memory: a session uses up to about
/// 1,500 files of about 3-5 KB, so 4,096 files hold several sessions and 32 MB is reached only
/// if some files are unusually large.
constexpr size_t MaxFiles = 4096;
constexpr size_t MaxBytes = size_t{32} << 20;
constexpr std::string_view ListName = "recent.txt";

/// A file name from recent.txt, without ".bin": the driver's pipeline key in lowercase hex
/// (at most 32 bytes). Anything else is not turned into a path.
inline bool ValidName(std::string_view name) {
    return !name.empty() && name.size() <= 64 &&
           name.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

/// The last `limit` distinct valid names of recent.txt, newest first.
inline std::vector<std::string> Recent(std::string_view list, size_t limit) {
    std::vector<std::string> names;
    std::unordered_set<std::string_view> seen;
    size_t end = list.size();
    while (end > 0 && names.size() < limit) {
        const size_t newline = list.rfind('\n', end - 1);
        const size_t first = newline == std::string_view::npos ? 0 : newline + 1;
        auto name = list.substr(first, end - first);
        end = newline == std::string_view::npos ? 0 : newline;
        if (!name.empty() && name.back() == '\r') {
            name.remove_suffix(1);
        }
        if (ValidName(name) && seen.insert(name).second) {
            names.emplace_back(name);
        }
    }
    return names;
}

/// Starts the thread that trims recent.txt in `directory`, reads the listed files and appends
/// the names this session uses. Until then the functions below do nothing.
/// GOW3_PIPELINE_PRELOAD=0 keeps the list but reads no file and serves no restore from memory.
void Start(const std::filesystem::path& directory);
/// The preloaded bytes of `file`, if it was read at start.
std::optional<std::vector<u8>> Find(const std::filesystem::path& file);
/// `file` was restored: listed for the next start.
void Used(const std::filesystem::path& file);
/// A new `file` was written: listed, and its old bytes are no longer served from memory.
void Saved(const std::filesystem::path& file);

} // namespace Vulkan::PipelineBinaryPreload
