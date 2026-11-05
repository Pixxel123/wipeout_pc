// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include "common/types.h"

namespace Vulkan {

/// gow3: a missing pipeline or shader is built on the GPU thread (the draw waits) unless a
/// pipeline was compiled for real in the last second, a binary cache miss; then the workers
/// build it and the draw is skipped until it is published. A restore from the cache takes well
/// under a millisecond, so with a warm cache nothing is ever skipped (WipEout lap-1 flashes).
struct CompilePolicy {
    static constexpr u64 window_ns = 1'000'000'000;

    /// gow3: a frame with at least this many draws shows a scene; a loading screen draws about 50.
    static constexpr u64 scene_draws = 500;
    /// This long without a scene frame: a loading screen or a menu.
    static constexpr u64 no_scene_ns = 2'000'000'000;
    /// The longer grace wait lasts this long into the first scene after one. With a fresh cache
    /// a race intro builds its pipelines in two bursts, from its first frame and about 5.5 s in;
    /// the racing starts about 20 s in.
    static constexpr u64 after_load_ns = 7'000'000'000;

    std::atomic<u64> background_until_ns{0};
    std::atomic<u64> last_scene_ns{0};
    std::atomic<u64> long_grace_until_ns{0};
    std::atomic<u64> flip_draws{0};

    static u64 Now() {
        return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
    }

    /// A pipeline was compiled rather than restored (any thread).
    void Compiled(u64 now_ns) {
        background_until_ns.store(now_ns + window_ns, std::memory_order_relaxed);
    }

    /// `enabled`: the in-game "compile new shaders in the background" setting.
    bool Background(u64 now_ns, bool enabled = true) const {
        return enabled && now_ns < background_until_ns.load(std::memory_order_relaxed);
    }

    /// Each flip; `draws` counts every draw so far. True when the frame is the first scene after
    /// a loading screen or a menu (the longer grace wait starts now).
    bool Flipped(u64 now_ns, u64 draws) {
        const u64 frame_draws = draws - flip_draws.exchange(draws, std::memory_order_relaxed);
        if (frame_draws < scene_draws) {
            return false;
        }
        const bool first = NoSceneFor(now_ns);
        if (first) {
            long_grace_until_ns.store(now_ns + after_load_ns, std::memory_order_relaxed);
        }
        last_scene_ns.store(now_ns, std::memory_order_relaxed);
        return first;
    }

    /// A draw may wait longer for its pipeline (GraceWait): no gameplay is on screen. Covers the
    /// first scene frame too, which is drawn before its flip and needs the most new pipelines.
    bool LongGrace(u64 now_ns) const {
        return NoSceneFor(now_ns) || now_ns < long_grace_until_ns.load(std::memory_order_relaxed);
    }

private:
    /// No scene yet, or none for no_scene_ns. Another thread may have stored a scene flip
    /// slightly after `now_ns` was read.
    bool NoSceneFor(u64 now_ns) const {
        const u64 last = last_scene_ns.load(std::memory_order_relaxed);
        return last == 0 || (now_ns > last && now_ns - last >= no_scene_ns);
    }
};

inline CompilePolicy g_compile_policy;

} // namespace Vulkan
