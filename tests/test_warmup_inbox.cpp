// Background pipeline cache warm-up helpers (vk_warmup_inbox.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"

using namespace Vulkan;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // Explicit loading-screen mode must not be bypassed by supported pipeline binaries.
    assert(!UseOnDemandPipelines(true, true, false, false));
    // Background mode keeps on-demand binaries and the existing warm-up fallbacks.
    assert(UseOnDemandPipelines(true, true, false, true));
    assert(!UseOnDemandPipelines(false, true, false, true));
    assert(!UseOnDemandPipelines(true, false, false, true));
    assert(!UseOnDemandPipelines(true, true, true, true));
    // The start-up rebuild runs only when asked for, with a list, outside capture-only runs.
    static_assert(RunsPrecompileBoot(true, true, false));
    static_assert(!RunsPrecompileBoot(false, true, false));
    static_assert(!RunsPrecompileBoot(true, false, false));
    static_assert(!RunsPrecompileBoot(true, true, true));
    // The start-up rebuild runs unless the list needed code from the game files and fewer than
    // 9 in 10 of those programs were read (no game folder, an archive not read): then the start
    // goes on as without a list.
    static_assert(GameCodeSuffices(0, 0));
    static_assert(GameCodeSuffices(1283, 1283));
    static_assert(GameCodeSuffices(1283, 1282)); // the shipped list: one program not found
    static_assert(GameCodeSuffices(1283, 1155));
    static_assert(!GameCodeSuffices(1283, 1154));
    static_assert(!GameCodeSuffices(1283, 0));
    static_assert(!GameCodeSuffices(1, 0));
    static_assert(GameCodeSuffices(0xffffffffu, 0xffffffffu));
    static_assert(!GameCodeSuffices(0xffffffffu, 0xe6666665u));
    // A start with the store open: (marker equal, driver key recorded, recorded key equal to the
    // driver's, list ready). A marker that differs rebuilds with a ready list and renews the
    // store without one, whatever the key; an equal marker rebuilds only for a recorded key that
    // differs and a ready list; everything else indexes the store as it is.
    using enum DiskShaderStart;
    static_assert(DiskShaderStartOf(false, false, false, false) == RenewWithoutBuild);
    static_assert(DiskShaderStartOf(false, false, true, false) == RenewWithoutBuild);
    static_assert(DiskShaderStartOf(false, true, false, false) == RenewWithoutBuild);
    static_assert(DiskShaderStartOf(false, true, true, false) == RenewWithoutBuild);
    static_assert(DiskShaderStartOf(false, false, false, true) == Rebuild);
    static_assert(DiskShaderStartOf(false, false, true, true) == Rebuild);
    static_assert(DiskShaderStartOf(false, true, false, true) == Rebuild);
    static_assert(DiskShaderStartOf(false, true, true, true) == Rebuild);
    static_assert(DiskShaderStartOf(true, false, false, false) == IndexOnly);
    static_assert(DiskShaderStartOf(true, false, true, false) == IndexOnly);
    static_assert(DiskShaderStartOf(true, true, false, false) == IndexOnly); // as without a key
    static_assert(DiskShaderStartOf(true, true, true, false) == IndexOnly);
    static_assert(DiskShaderStartOf(true, false, false, true) == IndexOnly); // store before the key
    static_assert(DiskShaderStartOf(true, false, true, true) == IndexOnly);
    static_assert(DiskShaderStartOf(true, true, false, true) == Rebuild); // the driver changed
    static_assert(DiskShaderStartOf(true, true, true, true) == IndexOnly);
    // A pending translation keeps the warm-up out of its program, except a stage already at its
    // saved index when the session numbers as the store does.
    static_assert(!WarmStageWaitsForTranslation(false, false, false));
    static_assert(!WarmStageWaitsForTranslation(false, true, true));
    static_assert(WarmStageWaitsForTranslation(true, false, false));
    static_assert(WarmStageWaitsForTranslation(true, false, true));
    static_assert(WarmStageWaitsForTranslation(true, true, false));
    static_assert(!WarmStageWaitsForTranslation(true, true, true));
    assert(WarmupThreadCount(1) == 1 && WarmupThreadCount(2) == 1 && WarmupThreadCount(4) == 1);
    assert(WarmupThreadCount(6) == 4 && WarmupThreadCount(8) == 6 && WarmupThreadCount(32) == 6);

    // Batches: never more than asked, oldest first, nothing lost across threads.
    WarmupInbox<int> inbox;
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&inbox, t] {
            for (int i = 0; i < 1000; ++i) {
                inbox.Push(t * 1000 + i);
            }
        });
    }
    for (auto& p : producers) {
        p.join();
    }
    std::vector<int> seen(4000, 0);
    size_t batches = 0;
    while (!inbox.Empty()) {
        const auto batch = inbox.Take(64);
        assert(!batch.empty() && batch.size() <= 64);
        for (int v : batch) {
            ++seen[v];
        }
        ++batches;
    }
    for (int count : seen) {
        assert(count == 1);
    }
    assert(batches == (4000 + 63) / 64);
    inbox.Push(1);
    inbox.Push(2);
    assert((inbox.Take(1) == std::vector<int>{1}) && (inbox.Take(8) == std::vector<int>{2}));
    assert(inbox.Take(8).empty());

    // Conflict: the first pipeline for a key wins, the later one is left to the caller.
    std::map<int, std::unique_ptr<int>> pipelines;
    auto runtime = std::make_unique<int>(1);
    auto warm = std::make_unique<int>(2);
    assert(PublishFirst(pipelines, 7, std::move(runtime)));
    assert(!PublishFirst(pipelines, 7, std::move(warm)));
    assert(*pipelines[7] == 1 && warm && *warm == 2);
    assert(PublishFirst(pipelines, 8, std::make_unique<int>(3)) && *pipelines[8] == 3);

    // #149: the first out-of-memory refusal stops the bulk warm-up; other refusals do not.
    WarmupMemoryStop stop;
    assert(!stop.Stopped());
    stop.Report(-3); // VK_ERROR_INITIALIZATION_FAILED: a damaged entry (#72), keep going
    stop.Report(0);  // VK_SUCCESS
    assert(!stop.Stopped());
    stop.Report(-1); // VK_ERROR_OUT_OF_HOST_MEMORY
    assert(stop.Stopped());
    stop.Reset();
    assert(!stop.Stopped());
    stop.Report(-2); // VK_ERROR_OUT_OF_DEVICE_MEMORY
    assert(stop.Stopped());

    std::puts("PASS: warm-up inbox");
}
