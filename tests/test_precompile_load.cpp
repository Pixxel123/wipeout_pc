// Loader of the precompile diagnostics (vk_precompile_load.h): which list entries are built
// while loading, when a draw's first use of a program or a pipeline miss waits for them and when
// a wait for their pipelines ends, from fake sets of programs in use and found by the scan and
// fake scan pass counters.
#include <cassert>
#include <set>
#include <vector>
#include "video_core/renderer_vulkan/vk_precompile_load.h"

using namespace Vulkan::Precompile;

namespace {

struct Stage {
    u64 hash;
};

/// The plan for an entry of `stages` when `needed` are neither in use nor saved and the scan
/// found the code of `found`.
EntryPlan Plan(const std::vector<Stage>& stages, const std::set<u64>& needed,
               const std::set<u64>& found) {
    return PlanEntry(
        stages, [&](u64 hash) { return needed.contains(hash); },
        [&](u64 hash) { return found.contains(hash); });
}

void TestEntryWithoutNeededProgramIsSkippedEvenWithoutCode() {
    assert(Plan({{1}, {2}}, {}, {}) == EntryPlan::Skip);
    assert(Plan({{1}, {2}}, {3}, {1, 2}) == EntryPlan::Skip);
}

void TestNeededEntryWaitsUntilEveryStageWasFound() {
    assert(Plan({{1}, {2}}, {1, 2}, {}) == EntryPlan::Wait);
    assert(Plan({{1}, {2}}, {1, 2}, {1}) == EntryPlan::Wait);
}

void TestStageInUseStillNeedsItsCode() {
    // Program 2 is in use: walked for its bindings only, but its code is read all the same.
    assert(Plan({{1}, {2}}, {1}, {1}) == EntryPlan::Wait);
    assert(Plan({{1}, {2}}, {1}, {1, 2}) == EntryPlan::Build);
}

void TestOneNeededProgramIsEnough() {
    assert(Plan({{1}, {2}}, {2}, {1, 2}) == EntryPlan::Build);
    assert(Plan({{1}, {2}}, {1, 2}, {1, 2}) == EntryPlan::Build);
}

bool Lacked(const std::vector<Stage>& stages, const std::set<u64>& needed,
            const std::set<u64>& fresh) {
    return EntryLacked(
        stages, [&](u64 hash) { return needed.contains(hash); },
        [&](u64 hash) { return fresh.contains(hash); });
}

void TestFreshCodeCountsOnlyForAnEntryWithANeededProgram() {
    assert(Lacked({{1}, {2}}, {1}, {2}));
    assert(Lacked({{1}, {2}}, {1}, {1}));
    assert(!Lacked({{1}, {2}}, {}, {2}));     // nothing to build
    assert(!Lacked({{1}, {2}}, {1, 2}, {3})); // not one of its programs
}

void TestLastPassFoundWhileItRanOrUntilTheNextOneFinishes() {
    assert(!LastPassFound(0, 0)); // nothing taken yet
    assert(!LastPassFound(0, 3));
    // Taken while pass 4 ran (3 finished): pass 4, or 3 when it was taken late.
    assert(LastPassFound(4, 3));
    assert(LastPassFound(4, 4));
    assert(!LastPassFound(4, 5)); // pass 5 finished without anything lacked
}

void TestHoldOnlyWhileLoadingWithWorkAndTimeLeft() {
    constexpr u64 Second = 1'000'000'000;
    assert(HoldActive(true, 0, true, false));
    assert(HoldActive(true, 0, false, true));
    assert(!HoldActive(true, 0, false, false)); // nothing queued, the scan found nothing more
    assert(!HoldActive(false, 0, true, true));  // never outside the loading state
    assert(HoldActive(true, 5 * Second - 1, true, true));
    assert(!HoldActive(true, 5 * Second, true, true)); // the load's holds took 5 s
}

void TestPipelineHoldOnlyWhileLoadingWithPipelinesPendingAndTimeLeft() {
    constexpr u64 Second = 1'000'000'000;
    assert(PipelineHoldActive(true, 0, true));
    assert(!PipelineHoldActive(true, 0, false)); // every pipeline handed over and built
    assert(!PipelineHoldActive(false, 0, true)); // never outside the loading state
    assert(PipelineHoldActive(true, 5 * Second - 1, true));
    assert(!PipelineHoldActive(true, 5 * Second, true)); // the load's holds took 5 s
}

void TestHoldWaitsForAPassThatStartedAfterItWokeTheScan() {
    // Pass 7 ran when the hold woke the scan (7 started, 6 finished): pass 8 is the one.
    assert(!WaitedPassDone(7, 6));
    assert(!WaitedPassDone(7, 7));
    assert(WaitedPassDone(7, 8));
    // No pass ran (6 started, 6 finished): pass 7.
    assert(WaitedPassDone(6, 7));
}

void TestHoldEndsWhenAPassFindsNothingNeededOrTimeIsUp() {
    constexpr u64 Second = 1'000'000'000;
    assert(HoldEnds(false, 0));
    assert(!HoldEnds(true, 0));
    assert(!HoldEnds(true, 5 * Second - 1));
    assert(HoldEnds(true, 5 * Second));
}

void TestPipelineWaitEndsOnceAllAreHandedOverAndBuiltOrTimeIsUp() {
    constexpr u64 Second = 1'000'000'000;
    assert(PipelineWaitEnds(false, true, 0));
    assert(!PipelineWaitEnds(false, false, 0)); // a build still runs
    assert(!PipelineWaitEnds(true, true, 0));   // the workers' queue was full: more to hand over
    assert(!PipelineWaitEnds(true, false, 5 * Second - 1));
    assert(PipelineWaitEnds(true, false, 5 * Second)); // the load's holds took 5 s
}

void TestBuildWaitOnlyWhileLoadingFromWhatTheHoldsLeft() {
    constexpr u64 Second = 1'000'000'000;
    assert(HoldLeftNs(true, 0) == 5 * Second);
    assert(HoldLeftNs(true, 2 * Second) == 3 * Second);
    assert(HoldLeftNs(true, 5 * Second) == 0);
    assert(HoldLeftNs(true, 6 * Second) == 0); // a wait may end a little past the budget
    assert(HoldLeftNs(false, 0) == 0);         // never outside the loading state
}

} // namespace

int main() {
    TestEntryWithoutNeededProgramIsSkippedEvenWithoutCode();
    TestNeededEntryWaitsUntilEveryStageWasFound();
    TestStageInUseStillNeedsItsCode();
    TestOneNeededProgramIsEnough();
    TestFreshCodeCountsOnlyForAnEntryWithANeededProgram();
    TestLastPassFoundWhileItRanOrUntilTheNextOneFinishes();
    TestHoldOnlyWhileLoadingWithWorkAndTimeLeft();
    TestPipelineHoldOnlyWhileLoadingWithPipelinesPendingAndTimeLeft();
    TestHoldWaitsForAPassThatStartedAfterItWokeTheScan();
    TestHoldEndsWhenAPassFindsNothingNeededOrTimeIsUp();
    TestPipelineWaitEndsOnceAllAreHandedOverAndBuiltOrTimeIsUp();
    TestBuildWaitOnlyWhileLoadingFromWhatTheHoldsLeft();
    return 0;
}
