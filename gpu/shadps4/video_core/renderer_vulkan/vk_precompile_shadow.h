// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_*, vk_precompile_boot.cpp): the guest words a new pipeline's
// selection read, and where they go in the private "shadow" copy it is replayed from. Only
// layout and address arithmetic here; the replay maps the pages.

#pragma once

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "common/types.h"

namespace Vulkan::Precompile {

/// Guest pointers keep 48 bits: the SRT walker and Info::ReadUdReg mask the rest.
constexpr u64 GuestAddressMask = 0xFFFFFFFFFFFFULL;

/// `original` led to `address` instead. Its top 16 bits stay: readers mask them off, and a
/// pointer may be the first half of a buffer descriptor (V#), whose stride lives there.
[[nodiscard]] constexpr u64 Repoint(u64 original, u64 address) {
    return (original & ~GuestAddressMask) | (address & GuestAddressMask);
}

/// One guest dword a reader took through `base`, a guest pointer from user data or a table.
struct GuestWord {
    u64 base;
    u32 offset; ///< bytes from base
    u32 value;
};

/// A pointer a reader followed: the 8 bytes at base + offset hold `target`.
struct GuestPointer {
    u64 base;
    u32 offset;
    u64 target;
};

/// The words of one pipeline laid out in pages: one region per base pointer with the words at
/// their offsets from it, the last word right before a guard page, so a read past it faults.
/// Gaps between recorded words of a region read as zero.
class ShadowLayout {
public:
    struct Region {
        u32 lo, hi;   ///< recorded bytes [lo, hi) from the base
        size_t start; ///< position of `lo` in the layout
    };

    ShadowLayout(std::span<const GuestWord> words, size_t page_size_) : page_size{page_size_} {
        for (const auto& word : words) {
            auto& region = regions.try_emplace(word.base, Region{word.offset, word.offset, 0})
                               .first->second;
            region.lo = std::min(region.lo, word.offset);
            region.hi = std::max<u32>(region.hi, word.offset + 4);
        }
        for (auto& [base, region] : regions) {
            const size_t bytes = region.hi - region.lo;
            const size_t pages = (bytes + page_size - 1) / page_size;
            region.start = size + pages * page_size - bytes;
            guards.push_back(size + pages * page_size);
            size += (pages + 1) * page_size;
        }
    }

    /// Bytes, guard pages included.
    [[nodiscard]] size_t Size() const {
        return size;
    }

    /// Positions of the guard pages in the layout.
    [[nodiscard]] const std::vector<size_t>& GuardPages() const {
        return guards;
    }

    /// Shadow address of `base` for a layout placed at `address`; none when nothing was read
    /// through it.
    [[nodiscard]] std::optional<u64> Translate(u64 base, u64 address) const {
        const auto it = regions.find(base);
        if (it == regions.end()) {
            return std::nullopt;
        }
        return address + it->second.start - it->second.lo;
    }

    /// Writes the words into `memory` (placed at `address`, zeroed), then repoints each
    /// pointer to the shadow address of its target, or to `dead` for a target nothing was read
    /// through. A pointer's two words must be among the words.
    void Fill(u8* memory, u64 address, std::span<const GuestWord> words,
              std::span<const GuestPointer> pointers, u64 dead) const {
        for (const auto& word : words) {
            std::memcpy(memory + Position(word.base, word.offset), &word.value, sizeof(u32));
        }
        for (const auto& pointer : pointers) {
            u8* const at = memory + Position(pointer.base, pointer.offset);
            u64 value;
            std::memcpy(&value, at, sizeof(value));
            value = Repoint(value, Translate(pointer.target, address).value_or(dead));
            std::memcpy(at, &value, sizeof(value));
        }
    }

private:
    [[nodiscard]] size_t Position(u64 base, u32 offset) const {
        const auto& region = regions.at(base);
        return region.start + (offset - region.lo);
    }

    size_t page_size;
    size_t size = 0;
    std::map<u64, Region> regions;
    std::vector<size_t> guards;
};

/// Fake pointers handed to the SRT walker while its reads are recorded: each slot is a range
/// of reserved, inaccessible addresses, so every read through it faults and is served by the
/// recorder. Slot 0 stands for the stage's user data, the others for the guest pointers the
/// walker loaded; the last one is a dead end for a recording that went wrong.
class RecordSlots {
public:
    RecordSlots(u64 address_, u64 slot_size_, u32 count_)
        : address{address_}, slot_size{slot_size_}, count{count_} {
        bases.reserve(count);
        Reset();
    }

    void Reset() {
        bases.assign(1, 0);
    }

    /// Fake pointer of a slot.
    [[nodiscard]] u64 Address(u32 slot) const {
        return address + slot * slot_size;
    }

    [[nodiscard]] u64 Dead() const {
        return Address(count - 1);
    }

    /// The slot whose fake pointer is `pointer` (a register value, not any address in it).
    [[nodiscard]] std::optional<u32> SlotOf(u64 pointer) const {
        if (pointer < address || (pointer - address) % slot_size != 0) {
            return std::nullopt;
        }
        const u64 slot = (pointer - address) / slot_size;
        return slot < bases.size() ? std::optional<u32>{u32(slot)} : std::nullopt;
    }

    /// Guest pointer a slot stands for (0 for the user data).
    [[nodiscard]] u64 Base(u32 slot) const {
        return bases[slot];
    }

    /// The slot for a guest pointer, added on first use; none when every slot is taken.
    /// Never allocates (the recorder runs in a signal handler).
    [[nodiscard]] std::optional<u32> Find(u64 guest) {
        for (u32 slot = 1; slot < bases.size(); ++slot) {
            if (bases[slot] == guest) {
                return slot;
            }
        }
        if (bases.size() + 1 >= count) {
            return std::nullopt;
        }
        bases.push_back(guest);
        return u32(bases.size() - 1);
    }

private:
    u64 address, slot_size;
    u32 count;
    std::vector<u64> bases; ///< reserved for `count`: push_back never allocates
};

/// Serves and records the reads of an SRT walker that runs on RecordSlots' fake pointers. The
/// fault handler calls it, so it never allocates: the caller reserves the vectors.
struct WalkRecorder {
    /// A guest read that fails instead of faulting.
    using ReadFn = bool (*)(u64 address, void* out, size_t size);

    RecordSlots& slots;
    std::span<const u32> user_data; ///< what slot 0 stands for
    ReadFn read;
    std::vector<GuestWord>& words;
    std::vector<GuestPointer>& pointers;
    std::vector<std::pair<u32, u64>>& roots; ///< user-data pointers: first sgpr, target
    const char* error = nullptr;

    /// The value of a walker read at `address` through the fake pointer `pointer`, 8 bytes for
    /// a pointer (which is replaced by the fake pointer of the table it leads to), else 4.
    u64 Load(u64 pointer, u64 address, bool is_pointer) {
        if (error) {
            return Failed(is_pointer);
        }
        const auto slot = slots.SlotOf(pointer);
        const u64 offset = address - pointer;
        const u32 size = is_pointer ? 8 : 4;
        if (!slot || offset > (1u << 30)) {
            return Fail("read through an unknown pointer", is_pointer);
        }
        const u64 base = slots.Base(*slot);
        u64 value = 0;
        if (*slot == 0) {
            if (offset + size > user_data.size_bytes()) {
                return Fail("read past the user data", is_pointer);
            }
            std::memcpy(&value, reinterpret_cast<const u8*>(user_data.data()) + offset, size);
        } else {
            if (words.size() + 2 > words.capacity()) {
                return Fail("too many guest words", is_pointer);
            }
            if (!read(base + offset, &value, size)) {
                return Fail("guest word unreadable", is_pointer);
            }
            words.push_back({base, u32(offset), u32(value)});
            if (is_pointer) {
                words.push_back({base, u32(offset + 4), u32(value >> 32)});
            }
        }
        if (!is_pointer) {
            return value;
        }
        const u64 target = value & GuestAddressMask;
        const auto next = slots.Find(target);
        if (!next) {
            return Fail("too many tables", true);
        }
        if (*slot == 0) {
            if (roots.size() == roots.capacity()) {
                return Fail("too many user-data pointers", true);
            }
            roots.emplace_back(u32(offset / 4), target);
        } else {
            if (pointers.size() == pointers.capacity()) {
                return Fail("too many table pointers", true);
            }
            pointers.push_back({base, u32(offset), target});
        }
        return slots.Address(*next);
    }

private:
    /// After a failure the walk still has to finish: zeros, and pointers to the dead slot.
    u64 Failed(bool is_pointer) const {
        return is_pointer ? slots.Dead() : 0;
    }
    u64 Fail(const char* why, bool is_pointer) {
        error = error ? error : why;
        return Failed(is_pointer);
    }
};

} // namespace Vulkan::Precompile
