// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_*, vk_precompile_replay.cpp): the reserved memory the precompile
// diagnostics read recorded guest words through, and the fault handler that serves or counts
// the reads that land outside them. Linux only: elsewhere nothing can be reserved and
// SetUpFaultGuard fails.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <utility>
#include "common/types.h"
#include "gow3_toggles.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan::Precompile {

constexpr size_t PageSize = 4096;
constexpr u64 RecordSlotSize = 16ull << 20;
constexpr u32 NumRecordSlots = 64;
constexpr u64 ShadowArenaSize = 1ull << 30;
constexpr size_t MaxShadowBytes = 4u << 20; ///< one capture
/// Most guest words and table pointers one stage's walk may read (WalkRecorder's vectors).
constexpr size_t MaxRecordedWords = 4096, MaxRecordedPointers = 256;

/// Address ranges reserved once: the record slots and the shadow arena, whose first page
/// stays inaccessible (the "dead" pointer for anything not recorded).
struct Reserved {
    u64 record = 0, shadow = 0;
    [[nodiscard]] bool InRecord(u64 at) const {
        return at - record < RecordSlotSize * NumRecordSlots;
    }
    [[nodiscard]] bool InShadow(u64 at) const {
        return at - shadow < ShadowArenaSize;
    }
};
extern Reserved g_reserved; // set once, before the fault handler is registered

/// Reserves the ranges and registers the fault handler, once; false when it cannot (logged).
bool SetUpFaultGuard();

bool Protect(u64 address, size_t size, bool access);
void Discard(u64 address, size_t size);

/// A guest read that fails instead of faulting, so the fault handler can use it.
bool ReadGuest(u64 address, void* out, size_t size);
/// errno of the first failed ReadGuest (EPERM from a sandbox, EFAULT unreadable), or 0.
int FirstReadError();

/// Fault-handler state of a thread that records or replays.
struct ThreadState {
    static constexpr u32 MaxWatched = 6;
    using Range = std::pair<u64, u64>; ///< code [first, second)

    WalkRecorder* recorder;                    ///< set while a walker is recorded
    std::array<Range, MaxWatched> walkers;     ///< code allowed to read our ranges
    u32 num_walkers;
    bool watching; ///< a recording or replay runs: note every fault
    u32 faults;
    u64 fault; ///< the first one's address

    /// Starts noting faults; reads of the walkers in `ranges` are served (empty ones skipped).
    void Watch(std::span<const Range> ranges) {
        num_walkers = 0;
        for (const auto& range : ranges) {
            if (range.first && num_walkers < MaxWatched) {
                walkers[num_walkers++] = range;
            }
        }
        faults = 0;
        fault = 0;
        watching = true;
    }
    void Stop() {
        recorder = nullptr;
        num_walkers = 0;
        watching = false;
    }
    [[nodiscard]] bool InWalker(u64 rip) const {
        for (u32 i = 0; i < num_walkers; ++i) {
            if (rip >= walkers[i].first && rip < walkers[i].second) {
                return true;
            }
        }
        return false;
    }
    void Note(u64 address) {
        if (faults++ == 0) {
            fault = address;
        }
    }
};

/// This thread's fault-handler state.
ThreadState& FaultState();

/// Runs `body`; false when a fault jumped back out of it (runtime_fault_recover).
template <typename Body>
[[gnu::noinline]] bool Guarded(Body&& body) {
    sigjmp_buf* const outer = runtime_fault_recover;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = outer;
        return false;
    }
    runtime_fault_recover = &recover;
    body();
    runtime_fault_recover = outer;
    return true;
}

/// Pages for the shadow copies, handed out once each: an address is never reused, so the
/// fetch-shader parse cache (keyed by code address) cannot match an older copy.
class ShadowArena {
public:
    std::optional<u64> Map(const ShadowLayout& layout);
    void Unmap(u64 address, const ShadowLayout& layout);

private:
    u64 next = g_reserved.shadow + PageSize; ///< the first page is the dead page
};

/// Where a fault outside the recorded words was, for the log.
const char* FaultPlace(u64 fault);

} // namespace Vulkan::Precompile
