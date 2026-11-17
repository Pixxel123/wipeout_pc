// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/types.h"

namespace Storage {

/// gow3: the whole disk cache of one game in a single append-only file, read into memory at
/// start. A file read mid-game can wait tens of ms for a drive that went to sleep; from memory,
/// the GPU thread never waits on the disk.
///
/// File: u32 magic, u32 version, then records of u32 type, u32 name_size, u32 data_size, the
/// name, the data, and the u64 XXH3_64bits of the record's bytes before it. All little-endian, no
/// padding. The last record for a name wins. Three data sizes mark records without data:
/// - Removed: the entry was removed.
/// - Used: the entry was used in the current launch.
/// - Launch: a new launch of the game starts; type 0 and no name, the only record without one.
///
/// gow3: an entry's age is the number of launches since it was last put or used. It lets the
/// cache drop what no build asks for any more.
///
/// Not thread-safe: the caller holds one lock around the memory side. Append and Rewrite only
/// touch the file, so they can run on another thread.
class CachePack {
public:
    static constexpr u32 Magic = 0x314B5047, Version = 1;
    static constexpr u32 Removed = 0xFFFFFFFF, Used = 0xFFFFFFFE, Launch = 0xFFFFFFFD;
    /// Older entries keep this age through Image(), which keeps its launch records few. An
    /// EraseUnused limit must stay below it.
    static constexpr u32 MaxAge = 100;
    static constexpr size_t MaxNameBytes = 128, MaxBlobBytes = size_t{16} << 20,
                            MaxPackBytes = size_t{1} << 30;

    /// Reads `file` (missing: empty pack). Damage cuts the log at the last good record.
    explicit CachePack(std::filesystem::path file);

    const std::vector<u8>* Find(u32 type, std::string_view name) const;
    std::vector<std::string> Names(u32 type) const;
    size_t Count(u32 type) const;
    /// Launches since the entry was last put or used; nullopt if absent.
    std::optional<u32> Age(u32 type, std::string_view name) const;
    /// Record bytes of the entries in memory: Image() is the header, this and the launch
    /// records that keep the ages.
    size_t LiveBytes() const;
    /// Record bytes in the file that no longer count: overwritten or removed entries and the
    /// removed, used and launch records. Only a reopen resets it, since Rewrite never touches
    /// memory.
    size_t DeadBytes() const;

    /// Memory side. Each returns the record to hand to Append; empty if refused (limits).
    std::vector<u8> Put(u32 type, std::string_view name, std::span<const u8> data);
    std::vector<u8> Erase(u32 type, std::string_view name);
    /// Removes every entry whose type `keep` rejects.
    void EraseIf(const std::function<bool(u32 type)>& keep);
    /// Starts the next launch.
    std::vector<u8> StartLaunch();
    /// Marks the entry used in this launch. Empty if it is absent or already marked (a Put
    /// marks it too), so each entry costs at most one record a launch.
    std::vector<u8> Use(u32 type, std::string_view name);
    /// Removes the entries of `type` older than `max_age` launches and returns how many. Like
    /// EraseIf it writes nothing: a reopen finds them again until a Rewrite.
    size_t EraseUnused(u32 type, u32 max_age);

    /// The whole file for the entries now in memory: header, then one record each, oldest
    /// first, with launch records between them so that a reopen gives the same ages.
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
    struct Entry {
        std::vector<u8> data;
        u32 last_launch; ///< the launch it was last put or used in
    };
    using Entries = std::unordered_map<std::string, Entry, NameHash, std::equal_to<>>;

    void Load();
    const Entry* FindEntry(u32 type, std::string_view name) const;
    Entry* FindEntry(u32 type, std::string_view name) {
        return const_cast<Entry*>(std::as_const(*this).FindEntry(type, name));
    }
    void Store(u32 type, std::string_view name, std::span<const u8> data);
    bool Remove(u32 type, std::string_view name);

    std::filesystem::path file;
    std::unordered_map<u32, Entries> types;
    u32 launch = 0; ///< the number of launch records read or started
    size_t live_bytes = 0;
    size_t dead_bytes = 0;
};

} // namespace Storage
