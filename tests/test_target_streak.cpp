// Render targets drawn every frame (vk_target_streak.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_target_streak.h"

using namespace Vulkan;

static void every_frame_after_three_flips() {
    Vulkan::TargetStreaks s;
    s.Drawn(0x1000, 10); s.Drawn(0x1000, 11);
    assert(!s.EveryFrame(0x1000, 11));
    s.Drawn(0x1000, 12);
    assert(s.EveryFrame(0x1000, 12));
    assert(s.EveryFrame(0x1000, 13)); // asked before this flip's first draw to it
    assert(!s.EveryFrame(0x1000, 14));
}
static void one_time_target_is_not_every_frame() {
    Vulkan::TargetStreaks s;
    s.Drawn(0x2000, 5);
    assert(!s.EveryFrame(0x2000, 5));
}
static void target_skipping_a_flip_restarts() {
    Vulkan::TargetStreaks s;
    for (u64 f = 1; f <= 5; ++f) s.Drawn(0x3000, f);
    s.Drawn(0x3000, 7); // flip 6 had no draw: a budgeted cube face
    assert(!s.EveryFrame(0x3000, 7));
}
static void many_draws_in_one_flip_count_once() {
    Vulkan::TargetStreaks s;
    for (int i = 0; i < 100; ++i) s.Drawn(0x4000, 3);
    assert(!s.EveryFrame(0x4000, 3));
}
static void slices_of_one_address_are_separate_targets() {
    using S = Vulkan::TargetStreaks;
    assert(S::Key(0, 3) == 0);
    assert(S::Key(0x5000, 0) != S::Key(0x5000, 1));
    S s;
    for (u64 f = 1; f <= 5; ++f) s.Drawn(S::Key(0x5000, 0), f); // face 0 every flip
    s.Drawn(S::Key(0x5000, 1), 5);                               // face 1 once
    assert(s.EveryFrame(S::Key(0x5000, 0), 5));
    assert(!s.EveryFrame(S::Key(0x5000, 1), 5));
}
static void null_target_and_table_bound() {
    Vulkan::TargetStreaks s;
    for (u64 f = 1; f <= 5; ++f) s.Drawn(0, f);
    assert(!s.EveryFrame(0, 5));
    for (u64 t = 1; t <= 1000; ++t) s.Drawn(t * 0x100, 9);
    assert(s.Size() <= 256);
}

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    every_frame_after_three_flips();
    one_time_target_is_not_every_frame();
    target_skipping_a_flip_restarts();
    many_draws_in_one_flip_count_once();
    slices_of_one_address_are_separate_targets();
    null_target_and_table_bound();
    std::puts("target streak: ok");
    return EXIT_SUCCESS;
}
