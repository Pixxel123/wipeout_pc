// SPDX-License-Identifier: GPL-2.0-or-later
// "Native (window)" output size: standalone checks, no game, Vulkan device or window required.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "gpu/shadps4/video_core/renderer_vulkan/output_resolution.h"

using namespace Vulkan::OutputResolution;
using Size = std::pair<uint32_t, uint32_t>;

int main() {
    // The presenter's frame: the window fitted to 16:9, even sides, uncapped.
    assert(Frame(2880, 1800) == Size(2880, 1620)); // bars top and bottom
    assert(Frame(3440, 1440) == Size(2560, 1440)); // bars left and right
    assert(Frame(1920, 1080) == Size(1920, 1080));
    assert(Frame(1366, 768) == Size(1364, 768));
    assert(Frame(1365, 768) == Size(1364, 766));
    assert(Frame(1000, 1000) == Size(1000, 562));
    assert(Frame(5120, 2880) == Size(5120, 2880));
    assert(Frame(0, 0) == Size(0, 0) && Frame(0, 1080) == Size(0, 0) && Frame(3, 3) == Size(0, 0));

    // The output is the frame, capped at FSR 4.1.1's 2160p tier; none for another game aspect.
    assert(Output(1280, 800, 1920, 1080) == Size(1280, 720));
    assert(Output(1366, 768, 1920, 1080) == Frame(1366, 768));
    assert(Output(5120, 2880, 1920, 1080) == Size(MaxWidth, MaxHeight));
    assert(Output(7680, 4320, 1920, 1080) == Size(MaxWidth, MaxHeight));
    assert(Output(1920, 1080, 2560, 1080) == Size(0, 0));
    assert(Output(1920, 1080, 0, 0) == Size(0, 0));

    assert(Is16x9(1920, 1080) && Is16x9(1364, 768) && Is16x9(1364, 766));
    assert(!Is16x9(1920, 1200) && !Is16x9(2560, 1080) && !Is16x9(1280, 960) && !Is16x9(1920, 0));

    // A size applies once it has held for kStableFrames calls; a drag never settles.
    Settled settled;
    for (const Size& size : {Size(1920, 1080), Size(2560, 1440), Size(1920, 1080)}) {
        for (uint32_t i = 0; i < Settled::kStableFrames; ++i) {
            assert(!settled.Update(size.first, size.second));
        }
        assert(settled.Update(size.first, size.second) && settled.Update(size.first, size.second));
    }
    Settled drag;
    for (uint32_t w = 1280; w < 2560; w += 16) {
        assert(!drag.Update(w, w * 9 / 16));
    }
    std::puts("Output resolution: PASS (frame fit, output cap, game aspect, settling)");
}
