// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_LOAD=1 with GOW3_PRECOMPILE_LIST=<file>
// (vk_precompile_load.cpp): while a track loads, the list's entries are built from the code the
// shader scan (vk_precompile_scan.h) finds in the game's memory, so the list needs no code and
// the race reads the translated shaders from the store as on a second visit. The decisions are
// here; the rest is in the .cpp.

#pragma once

#include <algorithm>
#include <memory>
#include <unordered_map>
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

} // namespace Precompile

/// The start-up rebuild's entry build (vk_precompile_boot.cpp) with the loader's code source:
/// each program's code where the scan found it in guest memory. A stage whose program is not
/// needed is only bound. GPU command thread only.
class LoadBuilder {
public:
    /// `code_at`: program hash -> guest address of its code, the loader's; kept by reference.
    LoadBuilder(PipelineCache& cache, const Precompile::ParsedList& list,
                const std::unordered_map<u64, u64>& code_at);
    ~LoadBuilder();

    /// Builds entry `index` of the list, which `code_at` has every stage's code for: null when
    /// it was built, else why not (a static text).
    const char* Build(u32 index, const Precompile::RecordedKey& key);
    /// The permutations written since the last call: program hash, permutation index.
    [[nodiscard]] std::vector<std::pair<u64, u32>> TakeWritten();

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
