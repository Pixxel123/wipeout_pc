// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_RECORD / GOW3_PRECOMPILE_LIST): the file a launch records its new
// pipelines' guest inputs into (vk_precompile_record.cpp) and a later launch's start-up rebuild
// replays them from (vk_precompile_boot.cpp). Only the format and its checks here. An assert in
// the selection ends the game, so every count, index and offset the replay relies on is checked
// on load, and an entry that fails is dropped and counted.
//
// Layout (native byte order, x86-64 only):
//   header: magic, version, Regs words, user-data words, stages
//   entries: body size, FNV-1a 64 of the body, body
//   body: its kind, then
//     a key: recorded key hash, draw-indirect sgpr offsets, the nonzero register words, then per
//            stage its program's hash, the digest of the spec the draw selected, user data and
//            user-data pointers, then the guest words and the table pointers
//            (vk_precompile_shadow.h);
//     a program's code (once per program hash, before the first key that uses it): the hash
//            and the code words, from the start token through the OrbShdr header.
// No translator output: the list outlives a shader store reset. The code is the game's: a
// list stays on the player's machine.

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan::Precompile {

constexpr std::array<char, 8> ListMagic = {'G', 'O', 'W', '3', 'P', 'C', 'L', '\n'};
constexpr u32 ListVersion = 3;

/// The build's layout, checked in the header (the recorder static_asserts them).
constexpr u32 ListRegsWords = 0xD000;  ///< AmdGpu::Regs::NumRegs
constexpr u32 ListUserDataWords = 16;  ///< AmdGpu::NUM_USER_DATA
constexpr u32 ListStages = 6;          ///< MaxShaderStages
constexpr u32 ListHwStages = 6;        ///< Regs::ProgramForStage takes 0..5
/// Registers the selection indexes arrays with (the recorder static_asserts them).
constexpr u32 ListPsInControlReg = 0xA1B6; ///< SPI_PS_IN_CONTROL, num_interp in bits 0-5
constexpr u32 ListMaxPsInputs = 32;        ///< HwFragmentRuntimeInfo::inputs
/// A draw-indirect sgpr offset names a vertex-stage user-data register from this one on; the
/// translator asserts that it is one of the 16 (UdRegFromShOffset), 0 for none.
constexpr u32 ListVsUserDataSgpr = 0x4C;

constexpr size_t MaxListBytes = 128u << 20;
/// Key entries a list holds: the recorder stops at it, the rebuild reads no more. A list grows
/// over launches and tracks (about 500 new keys per track, the rebuild ~1 ms each).
constexpr u32 MaxListEntries = 16000;
constexpr size_t MaxListEntryBytes = 4u << 20;
constexpr u32 MaxWordOffset = (1u << 30) + 4; ///< WalkRecorder stops at 1 << 30, plus a pointer
constexpr size_t MaxCodeBytes = 1u << 20;

enum class ListRecord : u32 { Key = 0, Code = 1 };

/// Program code as AmdGpu::GetParams finds it without searching: the start token, then the
/// OrbShdr header (AmdGpu::BinaryInfo, 32 bytes) at (code[1] + 1) * 2 dwords.
constexpr u32 CodeStartToken = 0xBEEB03FF;
constexpr std::array<u8, 7> CodeSignature = {'O', 'r', 'b', 'S', 'h', 'd', 'r'};
constexpr u32 CodeHeaderWords = 8;
constexpr u32 CodeHeaderLengthAt = 8; ///< bytes into the header: a u32, the length in bits 8-31
constexpr u32 CodeHeaderHashAt = 16;  ///< bytes into the header: the u64 shader hash

[[nodiscard]] inline bool operator==(const GuestWord& a, const GuestWord& b) {
    return a.base == b.base && a.offset == b.offset && a.value == b.value;
}
[[nodiscard]] inline bool operator==(const GuestPointer& a, const GuestPointer& b) {
    return a.base == b.base && a.offset == b.offset && a.target == b.target;
}

struct RecordedStage {
    u32 sw_stage;
    u32 hw_stage;
    u64 hash;
    /// FNV-1a 64 of the serialized StageSpecialization of the permutation the recording draw
    /// selected: what the rebuild writes to the store's meta for this stage.
    u64 spec_digest;
    std::array<u32, ListUserDataWords> user_data;
    std::vector<std::pair<u32, u64>> roots; ///< user-data pointers followed: first sgpr, target
    bool operator==(const RecordedStage&) const = default;
};

/// One new pipeline of the recording launch.
struct RecordedKey {
    u64 key_hash; ///< in the recording launch, for reference: permutation numbers change
    u32 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
    std::vector<std::pair<u32, u32>> regs; ///< nonzero register words: index, value
    std::vector<RecordedStage> stages;
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
    bool operator==(const RecordedKey&) const = default;
};

[[nodiscard]] inline u64 Fnv1a(std::span<const u8> bytes) {
    u64 hash = 0xCBF29CE484222325ULL;
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * 0x100000001B3ULL;
    }
    return hash;
}

/// Dword index of the code's header; past the end when there is none.
[[nodiscard]] inline u64 CodeHeaderAt(std::span<const u32> code) {
    return code.size() >= 2 ? (u64(code[1]) + 1) * 2 : code.size();
}

/// Why `code` cannot be the program `hash` as AmdGpu::GetParams reads it, or null.
inline const char* CheckCode(u64 hash, std::span<const u32> code) {
    if (code.size() < 2 || code[0] != CodeStartToken) {
        return "code without its start token";
    }
    const u64 header = CodeHeaderAt(code);
    if (header + CodeHeaderWords > code.size()) {
        return "code header out of range";
    }
    const auto* bytes = reinterpret_cast<const u8*>(code.data() + header);
    if (!std::equal(CodeSignature.begin(), CodeSignature.end(), bytes)) {
        return "code header without its signature";
    }
    u64 header_hash;
    std::memcpy(&header_hash, bytes + CodeHeaderHashAt, sizeof(header_hash));
    if (header_hash != hash) {
        return "code hash mismatch";
    }
    u32 length;
    std::memcpy(&length, bytes + CodeHeaderLengthAt, sizeof(length));
    length >>= 8;
    if (length == 0 || length > code.size_bytes()) {
        return "code length out of range";
    }
    return nullptr;
}

constexpr u32 CodeHeaderCrcAt = 24; ///< bytes into the header: the u32 crc32 of the code

/// CRC-32 as zlib's crc32() computes it (reflected polynomial 0xEDB88320).
[[nodiscard]] inline u32 Crc32(std::span<const u8> bytes) {
    u32 crc = 0xFFFFFFFF;
    for (const u8 byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
        }
    }
    return ~crc;
}

/// Why the body of `code` (CheckCode passed) is not what its header's crc32 says, or null: the
/// crc32 covers the code from the start token through the header's first 16 bytes. The start
/// token and the header alone may survive in memory the game has partly overwritten.
inline const char* CheckCodeBody(std::span<const u32> code) {
    const u64 header = CodeHeaderAt(code) * 4;
    if (header + CodeHeaderWords * 4 > code.size_bytes()) {
        return "code header out of range";
    }
    const auto* bytes = reinterpret_cast<const u8*>(code.data());
    u32 crc;
    std::memcpy(&crc, bytes + header + CodeHeaderCrcAt, sizeof(crc));
    return Crc32({bytes, size_t(header) + 16}) == crc ? nullptr : "code body differs from its crc";
}

/// A program's code from its start token through its OrbShdr header, or through the length
/// the header gives when that is longer, read through `read(address, out, size)` (false when
/// it cannot); empty when a read fails or it would be longer than MaxCodeBytes. CheckCode
/// says whether it is the program.
template <typename Read>
[[nodiscard]] std::vector<u32> ReadProgramCode(u64 address, Read&& read) {
    std::array<u32, 2> start;
    if (!read(address, start.data(), sizeof(start)) || start[0] != CodeStartToken) {
        return {};
    }
    const u64 header = CodeHeaderAt(start);
    std::array<u32, CodeHeaderWords> info;
    if ((header + CodeHeaderWords) * 4 > MaxCodeBytes ||
        !read(address + header * 4, info.data(), sizeof(info))) {
        return {};
    }
    u32 length;
    std::memcpy(&length, reinterpret_cast<const u8*>(info.data()) + CodeHeaderLengthAt,
                sizeof(length));
    const u64 words = std::max<u64>(header + CodeHeaderWords, ((length >> 8) + 3) / 4);
    if (words * 4 > MaxCodeBytes) {
        return {};
    }
    std::vector<u32> code(words);
    if (!read(address, code.data(), words * 4)) {
        return {};
    }
    return code;
}

namespace ListDetail {

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

/// An entry: body size, checksum, body.
inline std::vector<u8> Frame(const std::vector<u8>& body) {
    std::vector<u8> out;
    Put(out, u32(body.size()));
    Put(out, Fnv1a(body));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// Reads fixed-size values; once a read runs past the end, every later read gives zero.
class Reader {
public:
    explicit Reader(std::span<const u8> bytes_) : bytes{bytes_} {}

    template <typename T>
    T Get() {
        T value{};
        if (!ok || bytes.size() - at < sizeof(T)) {
            ok = false;
            return value;
        }
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }

    /// Whether `count` items of `each` bytes can still be read: checked before allocating.
    [[nodiscard]] bool Fits(u64 count, size_t each) {
        ok = ok && count <= (bytes.size() - at) / each;
        return ok;
    }

    [[nodiscard]] std::span<const u8> Take(size_t size) {
        if (!ok || bytes.size() - at < size) {
            ok = false;
            return {};
        }
        const auto taken = bytes.subspan(at, size);
        at += size;
        return taken;
    }

    [[nodiscard]] bool Ok() const {
        return ok;
    }
    [[nodiscard]] bool AtEnd() const {
        return at == bytes.size();
    }
    [[nodiscard]] size_t Position() const {
        return at;
    }

private:
    std::span<const u8> bytes;
    size_t at = 0;
    bool ok = true;
};

/// Decodes a code body (after its kind); false when it ends early or has bytes left over.
inline bool DecodeCode(Reader& in, u64& hash, std::vector<u32>& code) {
    hash = in.Get<u64>();
    const u32 words = in.Get<u32>();
    if (words > MaxCodeBytes / sizeof(u32) || !in.Fits(words, sizeof(u32))) {
        return false;
    }
    code.resize(words);
    for (auto& word : code) {
        word = in.Get<u32>();
    }
    return in.Ok() && in.AtEnd();
}

/// Whether a draw-indirect sgpr offset is none or a vertex-stage user-data register.
[[nodiscard]] inline bool ValidSgprOffset(u32 offset) {
    return offset == 0 ||
           (offset >= ListVsUserDataSgpr && offset < ListVsUserDataSgpr + ListUserDataWords);
}

/// Why a decoded key cannot be replayed safely, or null.
inline const char* Check(const RecordedKey& key) {
    for (const auto& [index, value] : key.regs) {
        if (index >= ListRegsWords) {
            return "register index out of range";
        }
        if (index == ListPsInControlReg && (value & 0x3F) > ListMaxPsInputs) {
            return "too many pixel shader inputs";
        }
    }
    if (!ValidSgprOffset(key.vertex_sgpr_offset) || !ValidSgprOffset(key.instance_sgpr_offset)) {
        return "draw-indirect sgpr offset out of range";
    }
    if (key.stages.empty() || key.stages.size() > ListStages) {
        return "bad stage count";
    }
    u32 sw_stages = 0;
    for (const auto& stage : key.stages) {
        if (stage.sw_stage >= ListStages || stage.hw_stage >= ListHwStages) {
            return "stage out of range";
        }
        if (sw_stages & (1u << stage.sw_stage)) {
            return "stage twice";
        }
        sw_stages |= 1u << stage.sw_stage;
        for (const auto& [sgpr, target] : stage.roots) {
            if (sgpr >= ListUserDataWords - 1) { // the replay rewrites two words (no wrap)
                return "user-data pointer out of range";
            }
        }
    }
    std::set<std::pair<u64, u32>> recorded;
    for (const auto& word : key.words) {
        if (word.offset > MaxWordOffset) {
            return "guest word offset out of range";
        }
        recorded.emplace(word.base, word.offset);
    }
    // ShadowLayout::Fill rewrites both words of a pointer in place.
    for (const auto& pointer : key.pointers) {
        if (pointer.offset > MaxWordOffset || !recorded.contains({pointer.base, pointer.offset}) ||
            !recorded.contains({pointer.base, pointer.offset + 4})) {
            return "pointer without its words";
        }
    }
    return nullptr;
}

/// Decodes a key body (after its kind); false when it ends early or has bytes left over.
inline bool Decode(Reader& in, RecordedKey& key) {
    key.key_hash = in.Get<u64>();
    key.vertex_sgpr_offset = in.Get<u32>();
    key.instance_sgpr_offset = in.Get<u32>();
    const u32 num_regs = in.Get<u32>();
    if (!in.Fits(num_regs, 8)) {
        return false;
    }
    key.regs.resize(num_regs);
    for (auto& [index, value] : key.regs) {
        index = in.Get<u32>();
        value = in.Get<u32>();
    }
    const u32 num_stages = in.Get<u32>();
    if (num_stages > ListStages) {
        return false;
    }
    key.stages.resize(num_stages);
    for (auto& stage : key.stages) {
        stage.sw_stage = in.Get<u32>();
        stage.hw_stage = in.Get<u32>();
        stage.hash = in.Get<u64>();
        stage.spec_digest = in.Get<u64>();
        for (auto& word : stage.user_data) {
            word = in.Get<u32>();
        }
        const u32 num_roots = in.Get<u32>();
        if (num_roots > ListUserDataWords || !in.Fits(num_roots, 12)) {
            return false;
        }
        stage.roots.resize(num_roots);
        for (auto& [sgpr, target] : stage.roots) {
            sgpr = in.Get<u32>();
            target = in.Get<u64>();
        }
    }
    const u32 num_words = in.Get<u32>();
    if (!in.Fits(num_words, 16)) {
        return false;
    }
    key.words.resize(num_words);
    for (auto& word : key.words) {
        word.base = in.Get<u64>();
        word.offset = in.Get<u32>();
        word.value = in.Get<u32>();
    }
    const u32 num_pointers = in.Get<u32>();
    if (!in.Fits(num_pointers, 20)) {
        return false;
    }
    key.pointers.resize(num_pointers);
    for (auto& pointer : key.pointers) {
        pointer.base = in.Get<u64>();
        pointer.offset = in.Get<u32>();
        pointer.target = in.Get<u64>();
    }
    return in.Ok() && in.AtEnd();
}

/// Keeps a key entry; why it was dropped, or null.
inline const char* AddKey(Reader& in, std::vector<RecordedKey>& keys, u32 max_keys) {
    RecordedKey key{};
    const char* why = !Decode(in, key) ? "malformed" : Check(key);
    if (!why && keys.size() >= max_keys) {
        why = "past the entry cap";
    }
    if (!why) {
        keys.push_back(std::move(key));
    }
    return why;
}

/// Keeps a program's code, once per hash; why it was dropped, or null.
inline const char* AddCode(Reader& in, std::map<u64, std::vector<u32>>& code) {
    u64 hash;
    std::vector<u32> words;
    if (!DecodeCode(in, hash, words)) {
        return "malformed";
    }
    if (const char* why = CheckCode(hash, words)) {
        return why;
    }
    const auto [it, added] = code.try_emplace(hash, std::move(words));
    return added || it->second == words ? nullptr : "code differs from an earlier record";
}

} // namespace ListDetail

[[nodiscard]] inline std::vector<u8> EncodeListHeader() {
    std::vector<u8> out(ListMagic.begin(), ListMagic.end());
    ListDetail::Put(out, ListVersion);
    ListDetail::Put(out, ListRegsWords);
    ListDetail::Put(out, ListUserDataWords);
    ListDetail::Put(out, ListStages);
    return out;
}

/// One entry, framed: body size, checksum, body.
[[nodiscard]] inline std::vector<u8> EncodeListEntry(const RecordedKey& key) {
    using ListDetail::Put;
    std::vector<u8> body;
    Put(body, ListRecord::Key);
    Put(body, key.key_hash);
    Put(body, key.vertex_sgpr_offset);
    Put(body, key.instance_sgpr_offset);
    Put(body, u32(key.regs.size()));
    for (const auto& [index, value] : key.regs) {
        Put(body, index);
        Put(body, value);
    }
    Put(body, u32(key.stages.size()));
    for (const auto& stage : key.stages) {
        Put(body, stage.sw_stage);
        Put(body, stage.hw_stage);
        Put(body, stage.hash);
        Put(body, stage.spec_digest);
        for (const u32 word : stage.user_data) {
            Put(body, word);
        }
        Put(body, u32(stage.roots.size()));
        for (const auto& [sgpr, target] : stage.roots) {
            Put(body, sgpr);
            Put(body, target);
        }
    }
    Put(body, u32(key.words.size()));
    for (const auto& word : key.words) {
        Put(body, word.base);
        Put(body, word.offset);
        Put(body, word.value);
    }
    Put(body, u32(key.pointers.size()));
    for (const auto& pointer : key.pointers) {
        Put(body, pointer.base);
        Put(body, pointer.offset);
        Put(body, pointer.target);
    }
    return ListDetail::Frame(body);
}

/// A program's code, framed as an entry.
[[nodiscard]] inline std::vector<u8> EncodeListCode(u64 hash, std::span<const u32> code) {
    using ListDetail::Put;
    std::vector<u8> body;
    Put(body, ListRecord::Code);
    Put(body, hash);
    Put(body, u32(code.size()));
    for (const u32 word : code) {
        Put(body, word);
    }
    return ListDetail::Frame(body);
}

struct ParsedList {
    enum class Status { Ok, NotAList, OtherVersion, OtherLayout };
    Status status = Status::NotAList;
    u32 version = 0;
    std::vector<RecordedKey> keys;
    std::map<u64, std::vector<u32>> code; ///< program hash -> code words, checked (CheckCode)
    u32 rejected = 0;                 ///< entries dropped: corrupt, invalid, cut off or past the cap
    std::vector<std::string> reasons; ///< why, for the first few
    /// The header and every entry before the first that is cut off or fails its checksum (a
    /// bad size field mid-file, a zero-filled tail after a power loss): what a recording keeps.
    size_t framed_bytes = 0;
};

/// Reads a list; keeps at most `max_keys` entries.
[[nodiscard]] inline ParsedList ParseList(std::span<const u8> file, u32 max_keys,
                                          u32 max_reasons = 8) {
    ParsedList list;
    ListDetail::Reader in{file};
    const auto magic = in.Take(ListMagic.size());
    if (!in.Ok() || !std::equal(magic.begin(), magic.end(), ListMagic.begin())) {
        return list;
    }
    list.version = in.Get<u32>();
    if (!in.Ok()) {
        return list;
    }
    if (list.version != ListVersion) {
        list.status = ParsedList::Status::OtherVersion;
        return list;
    }
    const u32 regs_words = in.Get<u32>();
    const u32 user_data_words = in.Get<u32>();
    const u32 stages = in.Get<u32>();
    if (!in.Ok() || regs_words != ListRegsWords || user_data_words != ListUserDataWords ||
        stages != ListStages) {
        list.status = ParsedList::Status::OtherLayout;
        return list;
    }
    list.status = ParsedList::Status::Ok;
    list.framed_bytes = in.Position();
    const auto reject = [&](std::string why) {
        if (list.reasons.size() < max_reasons) {
            list.reasons.push_back(std::move(why));
        }
        ++list.rejected;
    };
    bool intact = true;
    for (size_t index = 0; !in.AtEnd(); ++index) {
        const u32 size = in.Get<u32>();
        const u64 checksum = in.Get<u64>();
        const auto body = in.Take(size);
        if (!in.Ok() || size > MaxListEntryBytes) {
            reject("entry " + std::to_string(index) + ": cut off");
            break; // the rest cannot be framed
        }
        const bool checked = Fnv1a(body) == checksum;
        intact = intact && checked;
        if (intact) {
            list.framed_bytes = in.Position();
        }
        const char* why = checked ? nullptr : "checksum mismatch";
        ListDetail::Reader entry{body};
        const auto kind = entry.Get<ListRecord>();
        if (!why && kind == ListRecord::Code) {
            why = ListDetail::AddCode(entry, list.code);
        } else if (!why && kind == ListRecord::Key) {
            why = ListDetail::AddKey(entry, list.keys, max_keys);
        } else if (!why) {
            why = "malformed";
        }
        if (why) {
            reject("entry " + std::to_string(index) + ": " + why);
        }
    }
    return list;
}

/// What the rebuild makes of an entry: per stage its program and spec. Entries alike in this
/// translate to the same permutations, whatever their guest addresses and registers (which
/// differ between launches).
[[nodiscard]] inline u64 StagesDigest(std::span<const RecordedStage> stages) {
    std::vector<u8> bytes;
    for (const auto& stage : stages) {
        ListDetail::Put(bytes, stage.sw_stage);
        ListDetail::Put(bytes, stage.hash);
        ListDetail::Put(bytes, stage.spec_digest);
    }
    return Fnv1a(bytes);
}

/// What a recording keeps one entry of: the stages and the pipeline key the draw selected.
/// Entries alike in their stages can still be different pipelines (another blend mode, another
/// render target's samples or depth format), and the loader builds each entry's pipeline, so
/// each is kept. The key hash holds permutation numbers, which follow the order of first use:
/// it was the same in every launch looked at (the loader selects the recorded hash again), and
/// a launch that numbers them otherwise records the pipeline a second time, which the loader
/// finds built already.
[[nodiscard]] inline u64 EntryDigest(const RecordedKey& key) {
    std::vector<u8> bytes;
    ListDetail::Put(bytes, StagesDigest(key.stages));
    ListDetail::Put(bytes, key.key_hash);
    return Fnv1a(bytes);
}

/// Whether an entry of `entry_bytes` still fits a list whose entries take `list_bytes`
/// (LoadListFile reads at most the header plus MaxListBytes).
[[nodiscard]] inline bool ListHasRoom(size_t list_bytes, size_t entry_bytes) {
    return entry_bytes <= MaxListEntryBytes && list_bytes + entry_bytes <= MaxListBytes;
}

/// What a recording keeps of the list file it was given (GOW3_PRECOMPILE_RECORD): a list of
/// this version grows over launches; anything else is written anew.
struct ListAppend {
    bool keep = false;          ///< append to the file; otherwise it is written anew
    const char* why = nullptr;  ///< why an existing file is written anew
    size_t bytes = 0;           ///< kept: the header and every entry that could be framed
    std::set<u64> keys;         ///< kept keys, by EntryDigest
    size_t entries = 0;         ///< kept key entries: what the entry cap counts
    std::set<u64> code;         ///< programs whose code is kept
};

/// `file` is the existing file's bytes, empty when there is none.
[[nodiscard]] inline ListAppend PlanListAppend(std::span<const u8> file) {
    ListAppend plan;
    if (file.empty()) {
        return plan;
    }
    if (file.size() > MaxListBytes + EncodeListHeader().size()) {
        plan.why = "is larger than a list can be";
        return plan;
    }
    auto list = ParseList(file, ~0u, 0);
    switch (list.status) {
    case ParsedList::Status::NotAList:
        plan.why = "is not a precompile list";
        return plan;
    case ParsedList::Status::OtherVersion:
        plan.why = "has another list version";
        return plan;
    case ParsedList::Status::OtherLayout:
        plan.why = "has another register layout";
        return plan;
    case ParsedList::Status::Ok:
        break;
    }
    plan.keep = true;
    plan.bytes = list.framed_bytes;
    if (plan.bytes < file.size()) { // only what is kept counts
        list = ParseList(file.first(plan.bytes), ~0u, 0);
    }
    plan.entries = list.keys.size();
    for (const auto& key : list.keys) {
        plan.keys.insert(EntryDigest(key));
    }
    for (const auto& [hash, words] : list.code) {
        plan.code.insert(hash);
    }
    return plan;
}

/// Whether GOW3_PRECOMPILE_BOOT=1 asks for the start-up rebuild from GOW3_PRECOMPILE_LIST.
[[nodiscard]] inline bool BootRequested() {
    const char* boot = std::getenv("GOW3_PRECOMPILE_BOOT");
    return boot && boot[0] == '1';
}

/// Whether GOW3_PRECOMPILE_LOAD=1 asks for the loader, which builds GOW3_PRECOMPILE_LIST while
/// loading (vk_precompile_load.cpp); it replaces the start-up rebuild.
[[nodiscard]] inline bool LoadRequested() {
    const char* load = std::getenv("GOW3_PRECOMPILE_LOAD");
    return load && load[0] == '1';
}

/// Reads the list at `path` (vk_precompile_record.cpp); none when it cannot be used (logged).
std::optional<ParsedList> LoadListFile(const char* path, u32 max_keys);

} // namespace Vulkan::Precompile
