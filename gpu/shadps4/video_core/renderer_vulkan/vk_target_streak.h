// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <unordered_map>
#include "common/types.h"

namespace Vulkan {

/// gow3: a draw left out for a frame while its pipeline compiles heals itself only when its
/// render target is redrawn every frame: the next frame's draw then shows the target as it
/// should be. A cube face updated on a budget, or a target rendered once, does not heal, so
/// it may not be skipped. This tracker says which targets were drawn in each of the last
/// flips. GPU thread only.
struct TargetStreaks {
    /// gow3: the number of consecutive flips a target must be drawn in.
    static constexpr u32 needed = 3;
    /// gow3: the table is small; the oldest target is dropped beyond this.
    static constexpr std::size_t max_targets = 256;

    /// gow3: a cube face is one address with a slice number in the high bits. Guest addresses
    /// use far fewer than 52 bits, so the slices of one address do not collide. Address 0 is
    /// the null target and is never tracked.
    static constexpr u64 Key(u64 address, u32 slice) {
        return address == 0 ? 0 : address ^ (u64(slice) << 52);
    }

    /// A draw to `target` in frame `flip`. Several draws in one flip count once.
    void Drawn(u64 target, u64 flip) {
        if (target == 0) {
            return;
        }
        const auto it = streaks.find(target);
        if (it != streaks.end()) {
            Streak& streak = it->second;
            if (streak.last_flip == flip) {
                return;
            }
            // gow3: a flip without a draw breaks the run; this draw starts it again.
            streak.flips = streak.last_flip == flip - 1 ? streak.flips + 1 : 1;
            streak.last_flip = flip;
            return;
        }
        if (streaks.size() >= max_targets) {
            // gow3: the table is bounded; drop the target that was drawn longest ago.
            auto oldest = streaks.begin();
            for (auto i = streaks.begin(); i != streaks.end(); ++i) {
                if (i->second.last_flip < oldest->second.last_flip) {
                    oldest = i;
                }
            }
            streaks.erase(oldest);
        }
        streaks.emplace(target, Streak{flip, 1});
    }

    /// True when `target` was drawn in each of the last `needed` flips, ending at `flip` or at
    /// `flip - 1`: a draw asked about before this flip's first draw to it still counts.
    bool EveryFrame(u64 target, u64 flip) const {
        if (target == 0) {
            return false;
        }
        const auto it = streaks.find(target);
        if (it == streaks.end()) {
            return false;
        }
        const Streak& streak = it->second;
        return streak.flips >= needed &&
               (streak.last_flip == flip || streak.last_flip == flip - 1);
    }

    std::size_t Size() const {
        return streaks.size();
    }

private:
    struct Streak {
        u64 last_flip;
        u32 flips;
    };

    std::unordered_map<u64, Streak> streaks;
};

} // namespace Vulkan
