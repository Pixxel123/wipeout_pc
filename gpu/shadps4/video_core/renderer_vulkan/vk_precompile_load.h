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
#include <atomic>
#include <map>
#include <memory>
#include <optional>
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
    Skip,  ///< none of its programs is needed: nothing to build (counted once; not when checked)
    Wait,  ///< one is, or it is checked, but the scan has not found every stage's code yet
    Build, ///< one is, or it is checked, and the code of every stage was found
};

/// Whether none of the entry's `stages` has a needed program (`needed(hash)`, as PlanEntry's).
template <typename Stages, typename Needed>
[[nodiscard]] bool NoNeededProgram(const Stages& stages, Needed&& needed) {
    return std::ranges::none_of(stages, [&](const auto& stage) { return needed(stage.hash); });
}

/// `stages`: the entry's, by RecordedStage::hash. `needed(hash)`: the program is neither in use
/// (program_cache) nor saved (the store's index); `found(hash)`: the scan found its code. A
/// stage whose program is not needed is still selected (the next stage's spec depends on its
/// bindings), so it needs its code too. `check_existing` (the loader adds variants): an entry
/// with no needed program is built all the same, for the variants its pipeline selection adds to
/// programs that exist, so it is never skipped.
template <typename Stages, typename Needed, typename Found>
[[nodiscard]] EntryPlan PlanEntry(bool check_existing, const Stages& stages, Needed&& needed,
                                  Found&& found) {
    if (!check_existing && NoNeededProgram(stages, needed)) {
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
/// Of that, a pipeline miss's hold builds checked entries (no needed program) at most this long
/// per loading state together; the pump builds the rest.
constexpr u64 CheckedHoldNs = 1'000'000'000;

/// How long a pipeline miss's hold may still build checked entries: the smaller of what this
/// loading state's holds (`held_ns`) left of HoldBudgetNs and what their checked builds
/// (`checked_held_ns`) left of CheckedHoldNs, nothing when either is spent.
[[nodiscard]] constexpr u64 CheckedHoldLeftNs(u64 held_ns, u64 checked_held_ns) {
    if (held_ns >= HoldBudgetNs || checked_held_ns >= CheckedHoldNs) {
        return 0;
    }
    return std::min(HoldBudgetNs - held_ns, CheckedHoldNs - checked_held_ns);
}

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
/// some are not handed to the compile workers yet or still building, or checked entries (no
/// needed program) wait to be built and CheckedHoldLeftNs is not spent (`pipelines_pending`).
/// A first use alone does not cover them: the loader's own selections load programs into
/// program_cache, so later draws of those programs have no first use, and a checked entry has
/// no program a first use waits for.
[[nodiscard]] constexpr bool PipelineHoldActive(bool loading, u64 held_ns,
                                                bool pipelines_pending) {
    return loading && held_ns < HoldBudgetNs && pipelines_pending;
}

/// Which queue the pump builds from next: entries with a needed program first, the checked ones
/// after, those left over from an earlier loading state (`stale`) last.
enum class PumpQueue : u8 { None, Ready, Checking, Stale };
[[nodiscard]] constexpr PumpQueue NextPumpQueue(bool ready, bool checking, bool stale) {
    return ready      ? PumpQueue::Ready
           : checking ? PumpQueue::Checking
           : stale    ? PumpQueue::Stale
                      : PumpQueue::None;
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
    Full,    ///< the workers' queue holds LoadQueueDepth tasks, or time is up: again later
    Dropped, ///< not selected: a program or permutation is missing, or a read faulted
};

/// What a loader selection found of a stage's variant (permutation) it missed in its program.
enum class LoadVariant : u8 {
    Missing, ///< the program is in use or saved, without it
    Pending, ///< the program is in use and the game is translating a variant of it
};

/// Whether the loader translates a missing variant into the program and selects again: not
/// while the game translates one (that takes the next index), nor with
/// GOW3_PRECOMPILE_LOAD_VARIANTS=0 (`enabled` false).
[[nodiscard]] constexpr bool AddsVariant(bool enabled, LoadVariant found) {
    return enabled && found == LoadVariant::Missing;
}

/// One loading state's variants of programs in use or saved: added by the loader, or left to
/// the game (switched off, the game translating, the translation failed its checks or failed
/// before in this loading state or the game's, or four were added already).
struct VariantCounts {
    /// `present`: the program had an equal spec already; `present` and `left` count entries
    /// (selections), as each ends its selection.
    u32 added = 0, present = 0, left = 0;
};

/// The start-up rebuild's progress on the loading screen: one count for both halves.
struct BootProgress {
    u32 done, total;
};

/// Of a list of `entries`: the rebuild's entries passed (`translated`) and the pipeline pass's
/// (`pipelines_done`), each half `entries` long.
[[nodiscard]] constexpr BootProgress BootProgressOf(u32 entries, u32 translated,
                                                    u32 pipelines_done) {
    return {translated + pipelines_done, 2 * entries};
}

/// Permutations a program takes: IndexDiskShaders reads indices below it.
constexpr size_t MaxPermutations = 64;

/// GOW3_PRECOMPILE_LOAD_TRACE: what a loader selection found besides its PipelineStep.
struct PipelineTrace {
    std::optional<u64> key_hash;  ///< std::hash<GraphicsPipelineKey> of the key it selected
    std::vector<u8> key;          ///< that key's bytes, for the draw line's field comparison
    const char* dropped = nullptr; ///< why, for PipelineStep::Dropped (a static text)
    std::optional<u32> stage;      ///< the SwStage `dropped` is about, when it is one stage's
};

/// GOW3_PRECOMPILE_LOAD_TRACE=1 with the loader on; set once as the loader starts.
extern std::atomic<bool> g_load_trace;
/// With the trace: draws skipped (g_gow3_pending_draws) since the last flip, which takes them.
extern std::atomic<u32> g_load_trace_skipped;
/// Each flip, after CompilePolicy::Flipped, with the same draw count (video-out thread): with
/// the trace, a line for a frame that had skipped draws. `draws` (Gow3Stats::draws) counts
/// Rasterizer::Draw only; a skipped DrawIndirect counts among the skips all the same.
void LoadTraceFlipped(u64 flip_ns, u64 draws);

} // namespace Precompile

/// The start-up rebuild's entry build (vk_precompile_boot.cpp) for the loader: the code is the
/// loader's checked copies of what the scan found in guest memory, not the list's. A stage
/// whose program is not needed is only bound. GPU command thread only.
class LoadBuilder {
public:
    /// `code`: program hash -> code words, the loader's copies; kept by reference. `variants`:
    /// a selection that misses a variant of a program in use or saved adds it (Pipeline).
    LoadBuilder(PipelineCache& cache, const Precompile::ParsedList& list,
                const std::map<u64, std::vector<u32>>& code, bool variants);
    ~LoadBuilder();

    /// Builds entry `index` of the list, which `code` has every stage's code for: null when it
    /// was built, else why not (a static text). `no_info` is set when the reason is a program
    /// in use whose first translation is pending, which is no fault.
    const char* Build(u32 index, const Precompile::RecordedKey& key, bool& no_info);
    /// The permutations written since the last call: program hash, permutation index.
    [[nodiscard]] std::vector<std::pair<u64, u32>> TakeWritten();
    /// Entry `key`, built and its permutations in the store's index: its pipeline, selected
    /// and handed to the compile workers. Room in their queue (LoadQueueDepth) is checked first.
    /// A variant missing from a program in use or saved is translated into it and saved, and
    /// the pipeline selected again; past `until_ns` (CompilePolicy::Now) no variant after the
    /// first is added: Full, and the entry is selected again later. `trace`, when set, gets the
    /// key selected and why it was dropped.
    Precompile::PipelineStep Pipeline(const Precompile::RecordedKey& key, u64 until_ns,
                                      Precompile::PipelineTrace* trace = nullptr);
    /// The variants Pipeline added or left to the game since the last ResetVariants.
    [[nodiscard]] const Precompile::VariantCounts& Variants() const;
    /// When a loading state begins.
    void ResetVariants();
    /// When a loading state begins: the variants Pipeline could not add may be tried again.
    void ForgetFailedVariants();

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
