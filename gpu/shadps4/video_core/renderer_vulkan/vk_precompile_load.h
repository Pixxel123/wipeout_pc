// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_BOOT): the decisions and counts of the start-up rebuild's pipeline
// pass (vk_precompile_boot.cpp), which selects the pipeline of each entry the rebuild built and
// hands it to the compile workers.

#pragma once

#include <optional>
#include "common/types.h"

namespace Vulkan {

namespace Precompile {

/// The pass hands a pipeline to the compile workers only while fewer tasks than this wait in
/// their queue (64, shared with the shader translations): the rest stays for the draws.
constexpr size_t LoadQueueDepth = 48;

/// What became of a built entry's pipeline.
enum class PipelineStep {
    Have,    ///< built or being built already
    Queued,  ///< handed to the compile workers
    Full,    ///< the workers' queue holds LoadQueueDepth tasks, or time is up: again later
    Dropped, ///< not selected: a program or permutation is missing, or a read faulted
};

/// What a pass selection found of a stage's variant (permutation) it missed in its program.
enum class LoadVariant : u8 {
    Missing, ///< the program is in use or saved, without it
    Pending, ///< the program is in use and the game is translating a variant of it
};

/// Whether the pass translates a missing variant into the program and selects again: not
/// while the game translates one (that takes the next index).
[[nodiscard]] constexpr bool AddsVariant(LoadVariant found) {
    return found == LoadVariant::Missing;
}

/// The pass's variants of programs in use or saved, added to them.
struct VariantCounts {
    u32 added = 0;
};

/// The start-up rebuild's progress on the loading screen: one count for all its phases.
struct BootProgress {
    u32 done, total;
};

/// Of a list of `entries` whose code is read from `files` game files first (0 when the list
/// holds it): the files read (`files_read`), then the rebuild's entries passed (`translated`)
/// and the pipeline pass's (`pipelines_done`), each half `entries` long.
[[nodiscard]] constexpr BootProgress BootProgressOf(u32 files, u32 entries, u32 files_read,
                                                    u32 translated, u32 pipelines_done) {
    return {files_read + translated + pipelines_done, files + 2 * entries};
}

/// Permutations a program takes: IndexDiskShaders reads indices below it.
constexpr size_t MaxPermutations = 64;

/// For the pipeline pass's log: what a pass selection found besides its PipelineStep.
struct PipelineTrace {
    const char* dropped = nullptr; ///< why, for PipelineStep::Dropped (a static text)
    std::optional<u32> stage;      ///< the SwStage `dropped` is about, when it is one stage's
};

} // namespace Precompile

} // namespace Vulkan
