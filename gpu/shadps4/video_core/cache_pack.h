// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace Storage {

/// gow3: the whole disk cache of one game in a single append-only file, read into memory at
/// start. A file read mid-game can wait tens of ms for a drive that went to sleep; from memory,
/// the GPU thread never waits on the disk.
///
/// File: u32 magic, u32 version, then records of u32 type, u32 name_size, u32 data_size
/// (Removed = the entry was removed, no data follows), the name, the data, and the u64
/// XXH3_64bits of the record's bytes before it. All little-endian, no padding. The last record
/// for a name wins.
///
/// Not thread-safe: the caller holds one lock around the memory side. Append and Rewrite only
/// touch the file, so they can run on another thread.
class CachePack {
public:
    static constexpr u32 Magic = 0x314B5047, Version = 1, Removed = 0xFFFFFFFF;
    static constexpr size_t MaxNameBytes = 128, MaxBlobBytes = size_t{16} << 20,
                            MaxPackBytes = size_t{1} << 30;

    /// Reads `file` (missing: empty pack). Damage cuts the log at the last good record.
    explicit CachePack(std::filesystem::path file);

    const std::vector<u8>* Find(u32 type, std::string_view name) const;
    std::vector<std::string> Names(u32 type) const;
    size_t Count(u32 type) const;
    /// Record bytes of the entries in memory: Image() is the header plus this.
    size_t LiveBytes() const;
    /// Record bytes in the file that no longer count: overwritten or removed entries and the
    /// removal records themselves. Only a reopen resets it, since Rewrite never touches memory.
    size_t DeadBytes() const;

    /// Memory side. Each returns the record to hand to Append; empty if refused (limits).
    std::vector<u8> Put(u32 type, std::string_view name, std::span<const u8> data);
    std::vector<u8> Erase(u32 type, std::string_view name);
    /// Removes every entry whose type `keep` rejects.
    void EraseIf(const std::function<bool(u32 type)>& keep);

    /// The whole file for the entries now in memory (header + one record each).
    std::vector<u8> Image() const;

    /// File side: no memory access, safe on another thread than the lookups.
    bool Append(std::span<const u8> record); // false: stop appending this session
    bool Rewrite(std::span<const u8> image); // temp file + rename; false leaves the old file

private:
    // Lets Find look a name up by string_view without building a std::string.
    struct NameHash {
        using is_transparent = void;
        size_t operator()(std::string_view name) const {
            return std::hash<std::string_view>{}(name);
        }
    };
    using Entries = std::unordered_map<std::string, std::vector<u8>, NameHash, std::equal_to<>>;

    void Load();
    void Store(u32 type, std::string_view name, std::span<const u8> data);
    bool Remove(u32 type, std::string_view name);

    std::filesystem::path file;
    std::unordered_map<u32, Entries> types;
    size_t live_bytes = 0;
    size_t dead_bytes = 0;
};

} // namespace Storage
