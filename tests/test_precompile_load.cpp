// Loader of the precompile diagnostics (vk_precompile_load.h): which list entries are built
// while loading, from fake sets of programs in use and found by the scan.
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

} // namespace

int main() {
    TestEntryWithoutNeededProgramIsSkippedEvenWithoutCode();
    TestNeededEntryWaitsUntilEveryStageWasFound();
    TestStageInUseStillNeedsItsCode();
    TestOneNeededProgramIsEnough();
    return 0;
}
