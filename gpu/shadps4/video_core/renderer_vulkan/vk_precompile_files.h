// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_LIST with <list>.files): the program code of a list that holds none,
// read out of the player's own game files, so the list that ships holds no game code. The index
// names the files that hold the list's programs, one line each: eboot.bin, or the archive's file
// name, a TAB and the path inside the archive. The format rules are those of
// tools/deck/precompile-list-info.py (game_files, psarc_files, find_code, program_code).
// Standard library, zlib, the basic types and vk_precompile_list.h (the program code layout and
// its crc32) only, so host tests build it alone.

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <tsl/robin_map.h>
#include "common/types.h"

namespace Vulkan::Precompile {

/// An archive entry's unpacked size, and an archive's block size and table of contents, at most.
constexpr u64 MaxGameEntryBytes = 64ull << 20;
/// An eboot.bin read whole, at most.
constexpr u64 MaxEbootBytes = 256ull << 20;

struct GameCode {
    /// Program hash -> its code, from the start token through its OrbShdr header (or through the
    /// length the header gives when that is longer): every program the files hold whose code
    /// passes its crc32, the first found kept.
    tsl::robin_map<u64, std::vector<u8>> by_program;
    u32 files_read = 0; ///< eboot.bin files and archive entries read
    /// Each file of the index that was not read, and why: a file in none of the folders, an
    /// archive that is not one or is cut off, an entry not in the archive or cut off.
    std::vector<std::string> missing;
};

/// The code the files `index` names hold, read from `dirs` (the first first: the update folder,
/// then the game's): eboot.bin of every folder that has one, then the archives, the highest
/// dataNN first. An archive entry is found by the MD5 of its name (upper-cased when the archive
/// ignores case) in the table of contents, never by position; the first readable entry with
/// that hash is read. An archive name must be a plain file name; an index over 1 MiB is refused.
/// `progress(done, total)`: once at the start, after each file and at the end; `total` is the
/// eboot.bin files found plus the archive entries indexed, per archive file found.
[[nodiscard]] GameCode ReadGameCode(std::span<const std::filesystem::path> dirs,
                                    const std::filesystem::path& index,
                                    const std::function<void(u32 done, u32 total)>& progress);

/// MD5 (RFC 1321) of `bytes`.
[[nodiscard]] std::array<u8, 16> Md5(std::string_view bytes);

} // namespace Vulkan::Precompile
