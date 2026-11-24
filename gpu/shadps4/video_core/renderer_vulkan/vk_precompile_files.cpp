// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>
#include "video_core/renderer_vulkan/vk_precompile_files.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"

// The gpu target links miniz (zlib's API under mz_ names); host tests link zlib itself.
#if __has_include(<miniz.h>)
#include <miniz.h>
#else
#include <zlib.h>
#endif

namespace Vulkan::Precompile {

namespace {

namespace fs = std::filesystem;

/// Inflates zlib `in` into `out` (its size the room, then what it holds); false when it is not
/// a whole zlib stream that fits.
bool Inflate(std::span<const u8> in, std::vector<u8>& out) {
#if __has_include(<miniz.h>)
    mz_ulong size = out.size();
    const bool ok = mz_uncompress(out.data(), &size, in.data(), mz_ulong(in.size())) == MZ_OK;
#else
    uLongf size = out.size();
    const bool ok = uncompress(out.data(), &size, in.data(), uLong(in.size())) == Z_OK;
#endif
    out.resize(ok ? size_t(size) : 0);
    return ok;
}

/// The index file, at most.
constexpr u64 MaxIndexBytes = 1u << 20;
/// A name in a missing line, at most.
constexpr size_t MaxShownChars = 120;

template <typename T>
T Load(std::span<const u8> data, u64 at) {
    T value;
    std::memcpy(&value, data.data() + at, sizeof(T));
    return value;
}

/// Big-endian, `bytes` wide.
u64 LoadBig(const u8* at, u32 bytes) {
    u64 value = 0;
    for (u32 i = 0; i < bytes; ++i) {
        value = value << 8 | at[i];
    }
    return value;
}

/// The code of the program whose OrbShdr header is at `header` in `data` (32 bytes of it in
/// range), cut as ReadProgramCode does, or empty: the nearest start token below whose second
/// word puts the header there and whose code passes its crc32 (program_code).
std::vector<u8> ProgramCode(std::span<const u8> data, u64 header) {
    const u64 lowest = header > MaxCodeBytes ? header - MaxCodeBytes : 0;
    u64 end = header;
    while (true) {
        // The highest token from `lowest` on that ends at or before `end` (rfind).
        if (end < lowest + sizeof(CodeStartToken)) {
            return {};
        }
        u64 start = end - sizeof(CodeStartToken);
        while (Load<u32>(data, start) != CodeStartToken) {
            if (start == lowest) {
                return {};
            }
            --start;
        }
        end = start;
        const u64 at = (u64(Load<u32>(data, start + 4)) + 1) * 2;
        if (at * 4 != header - start || (at + CodeHeaderWords) * 4 > MaxCodeBytes) {
            continue;
        }
        const u64 length = Load<u32>(data, header + CodeHeaderLengthAt) >> 8;
        const u64 size = std::max(at + CodeHeaderWords, (length + 3) / 4) * 4;
        if (size > MaxCodeBytes || start + size > data.size()) {
            continue;
        }
        if (length == 0 || length > size) {
            continue;
        }
        const auto code = data.subspan(start, size);
        // The crc32 covers the code through the header's first 16 bytes (CheckCodeBody).
        if (Crc32(code.first(at * 4 + 16)) != Load<u32>(code, at * 4 + CodeHeaderCrcAt)) {
            continue;
        }
        return {code.begin(), code.end()};
    }
}

/// Whether `found` holds every program `wanted` names; never with no filter (`wanted` empty).
bool AllFound(const std::set<u64>& wanted, const GameCode& found) {
    return !wanted.empty() && found.by_program.size() == wanted.size();
}

/// Adds the code of each program `data` holds, `wanted` names (any when empty) and `found` has
/// not to it; stops once all are found (find_code).
void FindCode(std::span<const u8> data, const std::set<u64>& wanted, GameCode& found) {
    const std::string_view text{reinterpret_cast<const char*>(data.data()), data.size()};
    const std::string_view signature{reinterpret_cast<const char*>(CodeSignature.data()),
                                     CodeSignature.size()};
    for (size_t at = text.find(signature); at != text.npos && !AllFound(wanted, found);
         at = text.find(signature, at + 1)) {
        if (at + CodeHeaderWords * 4 > data.size()) {
            continue;
        }
        const u64 program = Load<u64>(data, at + CodeHeaderHashAt);
        if ((wanted.empty() || wanted.contains(program)) && !found.by_program.contains(program)) {
            if (auto code = ProgramCode(data, at); !code.empty()) {
                found.by_program.emplace(program, std::move(code));
            }
        }
    }
}

/// A name as a missing line shows it: at most MaxShownChars, cut with "...".
std::string Shown(std::string_view name) {
    return name.size() <= MaxShownChars ? std::string{name}
                                        : std::string{name.substr(0, MaxShownChars)} + "...";
}

/// A path as a missing line shows it: its last MaxShownChars, which hold the file name, cut
/// with a leading "...".
std::string Name(const fs::path& path) {
    const auto u8 = path.u8string();
    const std::string name{u8.begin(), u8.end()};
    return name.size() <= MaxShownChars ? name
                                        : "..." + name.substr(name.size() - MaxShownChars);
}

/// A PSARC (zlib blocks, 30-byte TOC entries) open for reading its entries (psarc_files).
class Archive {
public:
    /// Why it cannot be read, or null.
    const char* Open(const fs::path& path) {
        std::error_code error;
        file_size = fs::file_size(path, error);
        file.open(path, std::ios::binary);
        if (error || !file) {
            return "cannot open";
        }
        std::array<u8, 32> head;
        if (!Read(0, head.data(), head.size())) {
            return "cut off";
        }
        const auto field = [&](u32 at) { return u32(LoadBig(head.data() + at, 4)); };
        const u32 toc_len = field(12), entry_size = field(16);
        count = field(20);
        block = field(24);
        flags = field(28);
        if (std::memcmp(head.data(), "PSAR", 4) || std::memcmp(head.data() + 8, "zlib", 4) ||
            entry_size != EntrySize) {
            return "not a zlib PSARC";
        }
        if (block == 0 || block > MaxGameEntryBytes || toc_len > MaxGameEntryBytes ||
            32 + u64(count) * EntrySize > toc_len || toc_len > file_size) {
            return "bad table of contents";
        }
        toc.resize(toc_len - 32);
        if (!Read(32, toc.data(), toc.size())) {
            return "cut off";
        }
        // A block size table entry is as wide as a block's size needs.
        width = block <= 1u << 16 ? 2 : block <= 1u << 24 ? 3 : 4;
        sizes = (toc.size() - u64(count) * EntrySize) / width;
        return nullptr;
    }

    u32 Count() const {
        return count;
    }
    bool IgnoresCase() const {
        return flags & 1;
    }
    /// Entry `i`'s name hash: the MD5 of its name.
    std::span<const u8> Hash(u32 i) const {
        return {toc.data() + u64(i) * EntrySize, 16};
    }

    /// Entry `i` unpacked into `out`; why it cannot be, or null. A block whose size is not the
    /// block size is inflated, and kept as it is when it is not zlib (a short last block).
    const char* Entry(u32 i, std::vector<u8>& out) {
        const u8* entry = toc.data() + u64(i) * EntrySize;
        u64 index = LoadBig(entry + 16, 4);
        const u64 size = LoadBig(entry + 20, 5);
        u64 at = LoadBig(entry + 25, 5);
        if (size > MaxGameEntryBytes) {
            return "too large";
        }
        out.clear();
        out.reserve(size);
        std::vector<u8> stored, inflated;
        while (out.size() < size) {
            if (index >= sizes) {
                return "cut off";
            }
            const u64 field = LoadBig(toc.data() + u64(count) * EntrySize + index * width, width);
            const u64 length = field ? field : block;
            if (length > block) {
                return "bad block size";
            }
            stored.resize(length);
            if (!Read(at, stored.data(), stored.size())) {
                return "cut off";
            }
            inflated.resize(block);
            const auto& part = length != block && Inflate(stored, inflated) ? inflated : stored;
            if (part.empty()) {
                return "cut off";
            }
            out.insert(out.end(), part.begin(),
                       part.begin() + std::min<u64>(part.size(), size - out.size()));
            at += length;
            ++index;
        }
        return nullptr;
    }

private:
    static constexpr u32 EntrySize = 30;

    bool Read(u64 at, u8* out, u64 size) {
        if (at > file_size || size > file_size - at) {
            return false;
        }
        file.clear();
        file.seekg(std::streamoff(at));
        file.read(reinterpret_cast<char*>(out), std::streamsize(size));
        return u64(file.gcount()) == size;
    }

    std::ifstream file;
    u64 file_size = 0;
    u32 count = 0, block = 0, flags = 0, width = 2;
    u64 sizes = 0; ///< entries in the block size table
    std::vector<u8> toc; ///< after the header: the entries, then the block size table
};

using Hash = std::array<u8, 16>;

/// Why an indexed entry was not read until a copy of its archive says otherwise.
constexpr std::string_view NotInArchive = "not in the archive";

std::string Upper(std::string name) {
    for (char& c : name) {
        if (c >= 'a' && c <= 'z') {
            c = char(c - 'a' + 'A');
        }
    }
    return name;
}

/// The archive entries `names` read from one copy of an archive into `found`: for each name the
/// first entry with its hash that can be read, until all wanted `programs` are found. Erases the
/// names read from `left` and gives those it holds that were not read their reason; a name not
/// reached because all were found is erased unless a copy failed to read it. False when the
/// archive cannot be read at all (it is named in missing).
bool ReadArchive(const fs::path& path, const std::set<std::string>& names,
                 const std::set<u64>& programs, std::map<std::string, std::string>& left,
                 GameCode& found, const std::function<void()>& file_done) {
    Archive archive;
    if (const char* why = archive.Open(path)) {
        found.missing.push_back(Name(path) + ": " + why);
        for (size_t i = 0; i < names.size(); ++i) {
            file_done();
        }
        return false;
    }
    std::map<Hash, std::vector<const std::string*>> by_hash;
    for (const auto& name : names) {
        by_hash[Md5(archive.IgnoresCase() ? Upper(name) : name)].push_back(&name);
    }
    std::vector<u8> data;
    // Entry 0 is the manifest.
    for (u32 i = 1; i < archive.Count() && !by_hash.empty() && !AllFound(programs, found); ++i) {
        Hash hash;
        std::ranges::copy(archive.Hash(i), hash.begin());
        const auto wanted = by_hash.find(hash);
        if (wanted == by_hash.end()) {
            continue;
        }
        // On a failure the next entry with the hash is tried: any of them will do.
        const char* why = archive.Entry(i, data);
        for (const std::string* name : wanted->second) {
            if (const auto unread = left.find(*name); unread != left.end()) {
                if (why) {
                    unread->second = why;
                } else {
                    left.erase(unread);
                }
            }
        }
        if (why) {
            continue;
        }
        ++found.files_read;
        FindCode(data, programs, found);
        for (size_t n = 0; n < wanted->second.size(); ++n) {
            file_done();
        }
        by_hash.erase(wanted);
    }
    const bool all_found = AllFound(programs, found);
    for (const auto& [hash, unread] : by_hash) {
        for (const std::string* name : unread) {
            if (const auto it = left.find(*name);
                all_found && it != left.end() && it->second == NotInArchive) {
                left.erase(it);
            }
            file_done();
        }
    }
    return true;
}

} // namespace

std::array<u8, 16> Md5(std::string_view bytes) {
    static constexpr std::array<u32, 64> K = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391,
    };
    static constexpr std::array<u32, 16> Shifts = {7, 12, 17, 22, 5, 9,  14, 20,
                                                   4, 11, 16, 23, 6, 10, 15, 21};
    std::vector<u8> message(bytes.begin(), bytes.end());
    message.push_back(0x80);
    message.resize((message.size() + 8 + 63) / 64 * 64 - 8);
    const u64 bits = u64(bytes.size()) * 8;
    for (u32 i = 0; i < 8; ++i) {
        message.push_back(u8(bits >> (8 * i)));
    }
    std::array<u32, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t chunk = 0; chunk < message.size(); chunk += 64) {
        std::array<u32, 16> m;
        for (u32 i = 0; i < 16; ++i) {
            const u8* word = message.data() + chunk + i * 4;
            m[i] = u32(word[0]) | u32(word[1]) << 8 | u32(word[2]) << 16 | u32(word[3]) << 24;
        }
        auto [a, b, c, d] = state;
        for (u32 i = 0; i < 64; ++i) {
            u32 f, g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            f += a + K[i] + m[g];
            a = d;
            d = c;
            c = b;
            const u32 shift = Shifts[i / 16 * 4 + i % 4];
            b += (f << shift) | (f >> (32 - shift));
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
    std::array<u8, 16> digest;
    for (u32 i = 0; i < 16; ++i) {
        digest[i] = u8(state[i / 4] >> (8 * (i % 4)));
    }
    return digest;
}

GameCode ReadGameCode(std::span<const fs::path> dirs, const fs::path& index,
                      const std::function<void(u32 done, u32 total)>& progress,
                      const std::set<u64>& wanted) {
    GameCode found;
    // The index (read_index): eboot.bin, and the entry names per archive file name.
    std::error_code error;
    const u64 index_size = fs::file_size(index, error);
    std::ifstream in{index, std::ios::binary};
    if (error || !in) {
        found.missing.push_back(Name(index) + ": cannot read the index");
        return found;
    }
    if (index_size > MaxIndexBytes) {
        found.missing.push_back(Name(index) + ": an index larger than 1 MiB is refused");
        return found;
    }
    bool eboot = false;
    std::map<std::string, std::set<std::string>> archives;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "eboot.bin") {
            eboot = true;
        } else if (!line.empty()) {
            const size_t tab = line.find('\t');
            archives[line.substr(0, tab)].insert(tab == line.npos ? "" : line.substr(tab + 1));
        }
    }
    // The files in read order (game_files): eboot.bin of each folder, then the archives, the
    // highest dataNN first (one without a number last), each in the folders' order.
    std::vector<fs::path> eboots;
    for (const auto& dir : dirs) {
        if (eboot && fs::is_regular_file(dir / "eboot.bin", error)) {
            eboots.push_back(dir / "eboot.bin");
        }
    }
    if (eboot && eboots.empty()) {
        found.missing.push_back("eboot.bin: in none of the game folders");
    }
    struct Source {
        s64 number;
        size_t dir;
        fs::path path;
        const std::string* archive;
        const std::set<std::string>* names;
    };
    std::vector<Source> sources;
    u32 total = u32(eboots.size());
    for (const auto& [archive, names] : archives) {
        // A plain file name only: nothing outside the game folders is opened.
        if (archive.empty() || archive == "." || archive == ".." ||
            archive.find_first_of("/\\:") != archive.npos) {
            found.missing.push_back(Shown(archive) + ": not a file name");
            continue;
        }
        s64 number = -1;
        if (archive.ends_with(".psarc")) {
            const size_t end = archive.size() - 6;
            size_t start = end;
            while (start > 0 && archive[start - 1] >= '0' && archive[start - 1] <= '9') {
                --start;
            }
            if (start < end && end - start <= 18) {
                number = std::stoll(archive.substr(start, end - start));
            }
        }
        const size_t before = sources.size();
        for (size_t dir = 0; dir < dirs.size(); ++dir) {
            if (fs::is_regular_file(dirs[dir] / archive, error)) {
                sources.push_back({number, dir, dirs[dir] / archive, &archive, &names});
                total += u32(names.size());
            }
        }
        if (sources.size() == before) {
            found.missing.push_back(Shown(archive) + ": in none of the game folders");
        }
    }
    std::ranges::stable_sort(sources, [](const Source& a, const Source& b) {
        return a.number != b.number ? a.number > b.number : a.dir < b.dir;
    });

    u32 done = 0;
    const auto file_done = [&] {
        ++done;
        if (progress) {
            progress(done, total);
        }
    };
    if (progress) {
        progress(0, total);
    }
    // Once every wanted program is found no further file is read: the first folder still wins.
    // Only the first eboot.bin that can be read: with an update folder the game runs the update's,
    // so a later folder's is read only when the earlier ones cannot be.
    bool eboot_read = false;
    for (const auto& path : eboots) {
        if (eboot_read || AllFound(wanted, found)) {
            file_done();
            continue;
        }
        std::vector<u8> data;
        const u64 size = fs::file_size(path, error);
        std::ifstream file{path, std::ios::binary};
        if (!error && size <= MaxEbootBytes && file) {
            data.resize(size);
            file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
        }
        if (!data.empty() && u64(file.gcount()) == size) {
            ++found.files_read;
            FindCode(data, wanted, found);
            eboot_read = true;
        } else {
            found.missing.push_back(Name(path) + ": cannot read");
        }
        file_done();
    }
    // Per archive file name: its entries not read from any copy yet, and why. An archive no copy
    // of which could be read is named once (ReadArchive), not each of its entries.
    std::map<std::string, std::map<std::string, std::string>> left;
    std::set<std::string> opened;
    for (const auto& source : sources) {
        if (AllFound(wanted, found)) {
            for (size_t i = 0; i < source.names->size(); ++i) {
                file_done();
            }
            continue;
        }
        const std::string& archive = *source.archive;
        if (!left.contains(archive)) {
            for (const auto& name : *source.names) {
                left[archive][name] = NotInArchive;
            }
        }
        if (ReadArchive(source.path, *source.names, wanted, left[archive], found, file_done)) {
            opened.insert(archive);
        }
    }
    for (const auto& [archive, names] : left) {
        for (const auto& [name, why] : names) {
            if (opened.contains(archive)) {
                found.missing.push_back(Shown(archive) + ": " + Shown(name) + ": " + why);
            }
        }
    }
    return found;
}

} // namespace Vulkan::Precompile
