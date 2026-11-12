// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_GPU_PROFILE=1 — GPU time per render pass, dispatch and upscaler run. A timestamp is
// written where each of them starts (and at the frame end); the time to the next timestamp is
// charged to its label, barriers and copies recorded in between included. Results are read four
// frames later and printed every 5 s: GPU ms per frame by label.

#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class GpuProfiler {
public:
    /// The profiler when GOW3_GPU_PROFILE=1, else null.
    static GpuProfiler* Get() noexcept {
        return instance_ptr;
    }
    static void Init(const Instance& instance, Scheduler& scheduler);

    /// Starts a segment labelled `key`; `describe` names it the first time the key is seen.
    template <typename Describe>
    void Mark(u64 key, Describe&& describe) {
        if (!described.contains(key)) {
            described.emplace(key, describe());
        }
        WriteTimestamp(key);
    }

    /// The label of the open segment, for Resume after a nested segment (transfers).
    [[nodiscard]] u64 Current() const noexcept {
        return current;
    }
    /// Continues the segment of `key` (a label seen before) where a nested one ends.
    void Resume(u64 key) {
        WriteTimestamp(key);
    }

    /// Whether `other` is the scheduler it records into (the presenter has its own).
    [[nodiscard]] bool Records(const Scheduler* other) const noexcept {
        return other == &scheduler;
    }

    /// GOW3_GPU_PROFILE=2: splits a pass's segment in three. `kind` 1, just before the pass
    /// begins: what follows is its draws (" | draws"); before it, from the label, the barriers
    /// and transfers recorded ahead of the pass. `kind` 2, just after it ends: what follows, up
    /// to the next label, is recorded between passes (" | after").
    void Step(u32 kind);

    /// The idle gap: NoteIdleMark right after the "between submissions" label is written (the
    /// end of a submission's commands), NoteSubmitted once that submission is with the driver.
    /// With calibrated timestamps, each gap is then split into the GPU waiting for the next
    /// submission to be handed over and the time after that until its first timestamp.
    static constexpr u64 IdleKey = 0x5B317ull;
    void NoteIdleMark();
    void NoteSubmitted();

    /// At the display pass: closes the frame's last segment, reads an old frame's results.
    void BeginFrame();

private:
    GpuProfiler(const Instance& instance, Scheduler& scheduler);
    void WriteTimestamp(u64 key);
    void Collect(u32 slice);
    void Print();

    /// Frames of timestamps in flight: a slice is read and reused NumSlices frames later, and a
    /// frame whose slice the GPU has not finished by then goes unmeasured (`skipped`). With 4, a
    /// GPU-bound frame queue skipped many frames.
    static constexpr u32 NumSlices = 8;
    static constexpr u32 SliceQueries = 4096;
    static inline GpuProfiler* instance_ptr = nullptr;

    vk::Device device;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    double period_ns = 1.0;
    u32 slice = 0;
    std::array<std::vector<u64>, NumSlices> keys; ///< label of each timestamp but the last
    std::array<u32, NumSlices> used{};
    std::array<bool, NumSlices> pending{};
    std::array<u64, NumSlices> ticks{}; ///< scheduler tick of each pending slice's last timestamp
    bool paused = false; ///< the next slice is still in flight: no timestamps this frame
    std::unordered_map<u64, std::string> described;
    struct Total {
        double ms = 0;
        u64 segments = 0;
    };
    std::unordered_map<u64, Total> totals;
    u64 frames = 0;
    u64 skipped = 0; ///< frames in the window without timestamps (the GPU was behind)
    /// Wall time between frame starts in the window, to compare the GPU ms per frame against.
    double wall_ms = 0;
    u64 wall_frames = 0;
    std::chrono::steady_clock::time_point last_frame{};
    u64 current = 0;
    struct IdleEvent {
        u32 index;            ///< the idle label's timestamp in its slice
        u32 ordinal;          ///< which submission of the frame it ends (0 = first)
        s64 next_submit_ns{}; ///< CLOCK_MONOTONIC when the next submission reached the driver
    };
    std::array<std::vector<IdleEvent>, NumSlices> idle_events;
    struct IdleRef {
        u32 slice;
        u32 pos;
        bool valid = false;
    };
    IdleRef idle_prev, idle_cur;
    u32 submit_ordinal = 0;
    bool calibrated = false;
    struct IdleTotal {
        double gap_ms = 0, wait_ms = 0;
        u64 gaps = 0, queued = 0;
    };
    std::array<IdleTotal, 4> idle_totals{};
    u64 base = 0;       ///< the last label written by WriteTimestamp (Step derives from it)
    bool steps = false; ///< GOW3_GPU_PROFILE=2
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};

} // namespace Vulkan
