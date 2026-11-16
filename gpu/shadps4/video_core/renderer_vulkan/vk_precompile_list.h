// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_RECORD / GOW3_PRECOMPILE_LIST, vk_precompile_replay.cpp): the file one
// launch records its new pipelines' guest inputs into and a later launch replays them from.
// Only the format and its checks here. An assert in the selection ends the game, so every
// count, index and offset the replay relies on is checked on load, and an entry that fails is
// dropped and counted.
//
// Layout (native byte order, x86-64 only):
//   header: magic, version, Regs words, user-data words, stages
//   entries: body size, FNV-1a 64 of the body, body
//   body: recorded key hash, draw-indirect sgpr offsets, the nonzero register words, then per
//         stage its program's hash, user data and user-data pointers, then the guest words and
//         the table pointers (vk_precompile_shadow.h).
// No program code and no translator output: the list outlives a shader store reset.

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan::Precompile {

constexpr std::array<char, 8> ListMagic = {'G', 'O', 'W', '3', 'P', 'C', 'L', '\n'};
constexpr u32 ListVersion = 1;

/// The build's layout, checked in the header (the replay static_asserts them).
constexpr u32 ListRegsWords = 0xD000;  ///< AmdGpu::Regs::NumRegs
constexpr u32 ListUserDataWords = 16;  ///< AmdGpu::NUM_USER_DATA
constexpr u32 ListStages = 6;          ///< MaxShaderStages
constexpr u32 ListHwStages = 6;        ///< Regs::ProgramForStage takes 0..5

constexpr size_t MaxListBytes = 128u << 20;
constexpr size_t MaxListEntryBytes = 4u << 20;
constexpr u32 MaxWordOffset = (1u << 30) + 4; ///< WalkRecorder stops at 1 << 30, plus a pointer

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

namespace ListDetail {

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
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

private:
    std::span<const u8> bytes;
    size_t at = 0;
    bool ok = true;
};

/// Why a decoded key cannot be replayed safely, or null.
inline const char* Check(const RecordedKey& key) {
    for (const auto& [index, value] : key.regs) {
        if (index >= ListRegsWords) {
            return "register index out of range";
        }
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
            if (sgpr + 1 >= ListUserDataWords) { // the replay rewrites two words
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

/// Decodes a body; false when it ends early or has bytes left over.
inline bool Decode(std::span<const u8> body, RecordedKey& key) {
    Reader in{body};
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
    std::vector<u8> out;
    Put(out, u32(body.size()));
    Put(out, Fnv1a(body));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

struct ParsedList {
    enum class Status { Ok, NotAList, OtherVersion, OtherLayout };
    Status status = Status::NotAList;
    u32 version = 0;
    std::vector<RecordedKey> keys;
    u32 rejected = 0;                 ///< entries dropped: corrupt, invalid, cut off or past the cap
    std::vector<std::string> reasons; ///< why, for the first few
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
    const auto reject = [&](std::string why) {
        if (list.reasons.size() < max_reasons) {
            list.reasons.push_back(std::move(why));
        }
        ++list.rejected;
    };
    while (!in.AtEnd()) {
        const size_t index = list.keys.size() + list.rejected;
        const u32 size = in.Get<u32>();
        const u64 checksum = in.Get<u64>();
        const auto body = in.Take(size);
        if (!in.Ok() || size > MaxListEntryBytes) {
            reject("entry " + std::to_string(index) + ": cut off");
            break; // the rest cannot be framed
        }
        RecordedKey key{};
        const char* why = Fnv1a(body) != checksum       ? "checksum mismatch"
                          : !ListDetail::Decode(body, key) ? "malformed"
                                                           : ListDetail::Check(key);
        if (!why && list.keys.size() >= max_keys) {
            why = "past the entry cap";
        }
        if (why) {
            reject("entry " + std::to_string(index) + ": " + why);
            continue;
        }
        list.keys.push_back(std::move(key));
    }
    return list;
}

} // namespace Vulkan::Precompile
