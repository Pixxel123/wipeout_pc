// SPDX-License-Identifier: GPL-2.0-or-later
// The Output resolution rows apply at the game's own render; other render sizes output at their own.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "gpu/shadps4/video_core/renderer_vulkan/output_resolution.h"

using namespace Vulkan::OutputResolution;
using Size = std::pair<uint32_t, uint32_t>;

int main() {
    assert(FixedOrGuest(1280, 720, 1920, 1080) == Size(1280, 720));
    assert(FixedOrGuest(3840, 2160, 1920, 1080) == Size(3840, 2160));
    assert(FixedOrGuest(1920, 1080, 2560, 1440) == Size(2560, 1440));
    assert(FixedOrGuest(1280, 720, 1920, 1200) == Size(1920, 1200));
    assert(FixedOrGuest(1920, 1080, 3840, 2160) == Size(3840, 2160));
    std::puts("Output resolution: PASS (fixed rows at the native render)");
}
