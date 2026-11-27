// Shared decisions of the start-up rebuild's pipeline pass (vk_precompile_pass.h): when a
// selection adds a missing variant, and the loading screen's count.
#include <cassert>
#include "video_core/renderer_vulkan/vk_compile_policy.h"
#include "video_core/renderer_vulkan/vk_precompile_pass.h"

using namespace Vulkan::Precompile;

namespace {

void TestVariantStepsAsideWhileTheGameTranslates() {
    assert(AddsVariant(PassVariant::Missing));
    // The game's translation takes the next index.
    assert(!AddsVariant(PassVariant::Pending));
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

void TestTheBackgroundPassLeavesTheScreenTheTranslatingHalf() {
    const auto same = [](BootProgress a, BootProgress b) {
        return a.done == b.done && a.total == b.total;
    };
    assert(same(BootTranslationProgressOf(0, 3530, 0, 0), {0, 3530}));
    assert(same(BootTranslationProgressOf(0, 3530, 0, 3530), {3530, 3530}));
    assert(same(BootTranslationProgressOf(426, 3530, 426, 0), {426, 3956}));
    assert(same(BootTranslationProgressOf(426, 3530, 426, 3530), {3956, 3956}));
}

void TestOnlyZeroSwitchesTheBackgroundPassOff() {
    assert(BackgroundPassOn(nullptr));
    assert(BackgroundPassOn(""));
    assert(BackgroundPassOn("1"));
    assert(BackgroundPassOn("00"));
    assert(!BackgroundPassOn("0"));
}

void TestThePassIsLoggedInTenths() {
    assert(PassTenths(0, 3525) == 0);
    assert(PassTenths(352, 3525) == 0);
    assert(PassTenths(353, 3525) == 1);
    assert(PassTenths(3524, 3525) == 9);
    assert(PassTenths(3525, 3525) == 10);
    assert(PassTenths(0, 0) == 10);
    // No overflow at the most entries a list holds.
    assert(PassTenths(1u << 31, 1u << 31) == 10);
    static_assert(PassSliceNs == 4'000'000);
}

void TestARaceLoadingScreenTakesLongerSlices() {
    // Logos and menus: 4 ms. From a race's loading screen until the pass ends: 12 ms.
    assert(PassSliceNsFor(false) == 4'000'000);
    assert(PassSliceNsFor(true) == 12'000'000);
    static_assert(PassSliceNsFor(false) == PassSliceNs);
}

void TestTheFirstSceneFrameIsSeenAtItsFiveHundredthDraw() {
    using Vulkan::CompilePolicy;
    CompilePolicy policy;
    // Logos and menus: frames of up to 184 draws, each ended where its flip is decoded.
    assert(!policy.SceneFrameReached(184));
    policy.FlipDecoded(184);
    assert(!policy.SceneFrameReached(184 + 499));
    assert(policy.SceneFrameReached(184 + 500));
    // The present thread taking a flip late changes nothing: only the decoded flip counts.
    assert(!policy.Flipped(1'000'000'000, 184 + 300, 0));
    assert(policy.SceneFrameReached(184 + 500));
    // A flip decoded at this draw: the next frame counts from it.
    policy.FlipDecoded(184 + 500);
    assert(!policy.SceneFrameReached(184 + 500 + 499));
    assert(policy.SceneFrameReached(184 + 500 + 500));
    static_assert(CompilePolicy::scene_draws == 500);
}

} // namespace

int main() {
    TestVariantStepsAsideWhileTheGameTranslates();
    TestTheStartUpBuildCountsBothHalvesInOneBar();
    TestTheBackgroundPassLeavesTheScreenTheTranslatingHalf();
    TestOnlyZeroSwitchesTheBackgroundPassOff();
    TestThePassIsLoggedInTenths();
    TestARaceLoadingScreenTakesLongerSlices();
    TestTheFirstSceneFrameIsSeenAtItsFiveHundredthDraw();
    return 0;
}
