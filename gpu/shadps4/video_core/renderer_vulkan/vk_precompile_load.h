// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_LOAD=1 with GOW3_PRECOMPILE_LIST=<file>
// (vk_precompile_load.cpp): while a track loads, the list's entries are built from the code the
// shader scan (vk_precompile_scan.h) finds in the game's memory, so the list needs no code and
// the race reads the translated shaders from the store as on a second visit, and their
// pipelines are built on the compile workers before the race draws. The decisions are here; the
// rest is in the .cpp.

#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include "common/types.h"

namespace Vulkan {

class PipelineCache;
struct PrecompileBoot;

namespace Precompile {

struct ParsedList;
struct RecordedKey;

/// What the loader does with a list entry now.
enum class EntryPlan {
    Skip,  ///< none of its programs is needed: nothing to build (counted once)
    Wait,  ///< one is, but the scan has not found the code of every stage yet
    Build, ///< one is, and the code of every stage was found
};

/// `stages`: the entry's, by RecordedStage::hash. `needed(hash)`: the program is neither in use
/// (program_cache) nor saved (the store's index); `found(hash)`: the scan found its code. A
/// stage whose program is not needed is still selected (the next stage's spec depends on its
/// bindings), so it needs its code too.
template <typename Stages, typename Needed, typename Found>
[[nodiscard]] EntryPlan PlanEntry(const Stages& stages, Needed&& needed, Found&& found) {
    if (std::ranges::none_of(stages, [&](const auto& stage) { return needed(stage.hash); })) {
        return EntryPlan::Skip;
    }
    return std::ranges::all_of(stages, [&](const auto& stage) { return found(stage.hash); })
               ? EntryPlan::Build
               : EntryPlan::Wait;
}

/// Whether code the scan just found (`fresh(hash)`) holds a program of this entry that it
/// lacked, while it has a needed program: the scan is still finding what the loader waits for.
template <typename Stages, typename Needed, typename Fresh>
[[nodiscard]] bool EntryLacked(const Stages& stages, Needed&& needed, Fresh&& fresh) {
    return std::ranges::any_of(stages, [&](const auto& stage) { return needed(stage.hash); }) &&
           std::ranges::any_of(stages, [&](const auto& stage) { return fresh(stage.hash); });
}

/// The holds of one loading state take at most this long together.
constexpr u64 HoldBudgetNs = 5'000'000'000;

/// Whether the last finished scan pass found code an entry with a needed program lacked.
/// `found_in`: ShaderScan::Started() when the loader last took such code (0: never);
/// `finished`: ShaderScan::Passes(). Code taken while a pass runs may be the previous pass's
/// find: it then counts until the next pass finishes.
[[nodiscard]] constexpr bool LastPassFound(u64 found_in, u64 finished) {
    return found_in != 0 && found_in >= finished;
}

/// Whether a draw's first use of a program waits for the loader (the loader is active): only
/// while loading with hold time left (`held_ns` in this loading state), when entries are
/// queued or the last finished pass found code an entry lacked.
[[nodiscard]] constexpr bool HoldActive(bool loading, u64 held_ns, bool queued,
                                        bool last_pass_found) {
    return loading && held_ns < HoldBudgetNs && (queued || last_pass_found);
}

/// Whether a draw's pipeline miss waits for the pipelines of the entries built (the loader is
/// active): only while loading with hold time left (`held_ns` in this loading state), when
/// some are not handed to the compile workers yet or still building (`pipelines_pending`). A
/// first use alone does not cover them: the loader's own selections load programs into
/// program_cache, so later draws of those programs have no first use.
[[nodiscard]] constexpr bool PipelineHoldActive(bool loading, u64 held_ns,
                                                bool pipelines_pending) {
    return loading && held_ns < HoldBudgetNs && pipelines_pending;
}

/// Whether the pass a hold waits for has finished: one that started after the `started`
/// passes that had when the hold woke the scan (ShaderScan::Started(), Passes()).
[[nodiscard]] constexpr bool WaitedPassDone(u64 started, u64 finished) {
    return finished > started;
}

/// Whether a hold ends after its pass: the pass found no code an entry with a needed program
/// lacked, or this loading state's holds have taken HoldBudgetNs.
[[nodiscard]] constexpr bool HoldEnds(bool pass_found, u64 held_ns) {
    return !pass_found || held_ns >= HoldBudgetNs;
}

/// Whether a hold stops waiting for the pipelines of the entries built: every one was handed
/// to the compile workers (`left`: their queue held LoadQueueDepth tasks) and every build the
/// loader handed over is done, or this loading state's holds have taken HoldBudgetNs.
[[nodiscard]] constexpr bool PipelineWaitEnds(bool left, bool builds_done, u64 held_ns) {
    return (!left && builds_done) || held_ns >= HoldBudgetNs;
}

/// How long a draw may still wait for a pipeline the loader is building: what this loading
/// state's holds left of HoldBudgetNs, nothing outside a loading state.
[[nodiscard]] constexpr u64 HoldLeftNs(bool loading, u64 held_ns) {
    return loading && held_ns < HoldBudgetNs ? HoldBudgetNs - held_ns : 0;
}

/// The loader hands a pipeline to the compile workers only while fewer tasks than this wait in
/// their queue (64, shared with the shader translations): the rest stays for the draws.
constexpr size_t LoadQueueDepth = 48;

/// What became of a built entry's pipeline.
enum class PipelineStep {
    Have,    ///< built or being built already
    Queued,  ///< handed to the compile workers
    Full,    ///< the workers' queue holds LoadQueueDepth tasks: again later
    Dropped, ///< not selected: a program or permutation is missing, or a read faulted
};

} // namespace Precompile

/// The start-up rebuild's entry build (vk_precompile_boot.cpp) for the loader: the code is the
/// loader's checked copies of what the scan found in guest memory, not the list's. A stage
/// whose program is not needed is only bound. GPU command thread only.
class LoadBuilder {
public:
    /// `code`: program hash -> code words, the loader's copies; kept by reference.
    LoadBuilder(PipelineCache& cache, const Precompile::ParsedList& list,
                const std::map<u64, std::vector<u32>>& code);
    ~LoadBuilder();

    /// Builds entry `index` of the list, which `code` has every stage's code for: null when it
    /// was built, else why not (a static text). `no_info` is set when the reason is a program
    /// in use whose first translation is pending, which is no fault.
    const char* Build(u32 index, const Precompile::RecordedKey& key, bool& no_info);
    /// The permutations written since the last call: program hash, permutation index.
    [[nodiscard]] std::vector<std::pair<u64, u32>> TakeWritten();
    /// Entry `key`, built and its permutations in the store's index: its pipeline, selected
    /// and handed to the compile workers. Room in their queue (LoadQueueDepth) is checked first.
    Precompile::PipelineStep Pipeline(const Precompile::RecordedKey& key);

private:
    std::unique_ptr<PrecompileBoot> boot;
};

struct PrecompileLoad;
struct PrecompileLoadDeleter {
    void operator()(PrecompileLoad* load) const;
};
using PrecompileLoadPtr = std::unique_ptr<PrecompileLoad, PrecompileLoadDeleter>;

/// Null unless GOW3_PRECOMPILE_LOAD=1 and GOW3_PRECOMPILE_LIST=<file> are set (Linux only).
PrecompileLoadPtr MakePrecompileLoad(PipelineCache& cache);

} // namespace Vulkan
