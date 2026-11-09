// SPDX-License-Identifier: GPL-2.0-or-later
// Guest-relative scene sizes: standalone checks, no game, Vulkan device or window required.
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/scene_resolution.h"

using namespace Vulkan::SceneResolution;

int main() {
    // Guest-relative sizes: the render resolution patches set scene targets to the guest size
    // and half-resolution targets to exactly half of it.
    constexpr Size native{1920, 1080}, wide{2560, 1080}, uhd{3840, 2160};
    static_assert(SizeDivisor({1920, 1080}, native, {1280, 720}) == 1);
    static_assert(SizeDivisor({960, 540}, native, {1280, 720}) == 2);
    static_assert(SizeDivisor({1280, 720}, native, {1280, 720}) == 0);
    static_assert(SizeDivisor({2560, 1080}, wide, {1706, 720}) == 1);
    static_assert(SizeDivisor({1280, 540}, wide, {1706, 720}) == 2);
    static_assert(SizeDivisor({1920, 1080}, wide, {1706, 720}) == 0); // patch not applied
    static_assert(SizeDivisor({1280, 540}, wide, {1707, 720}) == 0);  // odd scene
    static_assert(SizeDivisor({1920, 1080}, uhd, {2560, 1440}) == 2);
    static_assert(SizeDivisor({3840, 2160}, uhd, {2560, 1440}) == 1);
    static_assert(SizeDivisor({960, 540}, native, {1280, 720}, false) == 0); // toggle off
    // Presets keep even sides at any aspect (Balanced is 1/1.7).
    assert((ForPreset(2, {3440, 1440}) == Size{2024, 848}));
    assert((ForPreset(3, {2560, 1080}) == Size{1280, 540}));
    static_assert(NativeRender({1920, 1080}) && !NativeRender({2560, 1080}) &&
                  !NativeRender({1920, 1200}));
    // The session's guest: below the game's own size the game keeps its whole buffer, above the
    // largest generated size nothing is patched, and half-resolution targets need even sides.
    constexpr Size floor{1920, 1080};
    static_assert(ClampGuest({1280, 720}, floor) == Size{1920, 1080});
    static_assert(ClampGuest({1600, 900}, floor) == Size{1920, 1080});
    static_assert(ClampGuest({1280, 1440}, floor) == Size{1920, 1080}); // one side below: inert
    static_assert(ClampGuest({2560, 1080}, floor) == Size{2560, 1080});
    static_assert(ClampGuest({1280, 720}, Size{0, 0}) == Size{1280, 720}); // no floor for this title
    static_assert(ClampGuest({9000, 5000}, floor) == Size{7680, 4320});
    static_assert(ClampGuest({9000, 5000}, Size{0, 0}) == Size{7680, 4320});
    static_assert(ClampGuest({2561, 1081}, Size{0, 0}) == Size{2560, 1080});
    static_assert(ClampGuest({3200, 1350}, floor) == Size{3200, 1350}); // odd half size is the game's business
    static_assert(Unpack(Pack({7680, 4320})) == Size{7680, 4320});
    // A game that shows another size than the session expects (patch refused) gets no preset.
    static_assert(EffectivePreset(3, true, false) == 3);
    static_assert(EffectivePreset(3, true, true) == 0);
    static_assert(EffectivePreset(3, false, false) == 0);
    std::puts("Guest size: PASS (scene and half-resolution divisors)");
}
