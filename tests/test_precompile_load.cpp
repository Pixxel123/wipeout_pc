// Shared decisions of the start-up rebuild's pipeline pass (vk_precompile_load.h): when a
// selection adds a missing variant, and the loading screen's count.
#include <cassert>
#include "video_core/renderer_vulkan/vk_precompile_load.h"

using namespace Vulkan::Precompile;

namespace {

void TestVariantStepsAsideWhileTheGameTranslates() {
    assert(AddsVariant(LoadVariant::Missing));
    // The game's translation takes the next index.
    assert(!AddsVariant(LoadVariant::Pending));
}

void TestTheStartUpBuildCountsBothHalvesInOneBar() {
    const auto same = [](BootProgress a, BootProgress b) {
        return a.done == b.done && a.total == b.total;
    };
    // A list that holds its code: no files.
    assert(same(BootProgressOf(0, 3530, 0, 0, 0), {0, 7060}));
    assert(same(BootProgressOf(0, 3530, 0, 3530, 0), {3530, 7060}));
    assert(same(BootProgressOf(0, 3530, 0, 3530, 3530), {7060, 7060}));
    // Its code read from 426 game files first.
    assert(same(BootProgressOf(426, 3530, 0, 0, 0), {0, 7486}));
    assert(same(BootProgressOf(426, 3530, 426, 0, 0), {426, 7486}));
    assert(same(BootProgressOf(426, 3530, 426, 3530, 3530), {7486, 7486}));
}

} // namespace

int main() {
    TestVariantStepsAsideWhileTheGameTranslates();
    TestTheStartUpBuildCountsBothHalvesInOneBar();
    return 0;
}
