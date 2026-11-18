// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_LOAD=1 with GOW3_PRECOMPILE_LIST=<file> (vk_precompile_load.h):
// while loading (CompilePolicy::NoSceneFor) a shader scan of the loader's own finds each
// program's code in the game's memory. An entry with a needed program whose stages were all
// found is built by the start-up rebuild's entry build (LoadBuilder), which writes the needed
// programs' permutations to the store as the rebuild does; at most 4 ms of entries per
// submission. When the queue runs empty after a build, the store is flushed and the new
// permutations are added to its index, so a program's first use in this launch reads them
// (LoadDiskProgram) instead of translating it.
//
// Threads: the pump runs in Rasterizer::OnSubmit, which only the GPU command thread calls
// (Liverpool::Process, liverpool.cpp:172); so do GetProgram's lookups of program_cache and
// disk_permutations. The loading state is read from CompilePolicy's atomics here, so nothing
// of the loader is shared with the present thread.
//
// Linux only (the scan); elsewhere the switch logs that it is not supported.

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_compile_policy.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_precompile_guard.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"
#include "video_core/renderer_vulkan/vk_precompile_load.h"
#include "video_core/renderer_vulkan/vk_precompile_scan.h"

namespace Vulkan {

using namespace Precompile;

namespace {

constexpr u64 PumpNs = 4'000'000; ///< ready entries one pump builds, at most

} // namespace

struct PrecompileLoad {
    PrecompileLoad(PipelineCache& cache_, ParsedList list_)
        : cache{cache_}, list{std::move(list_)}, builder{cache, list, code_at} {
        waiting.resize(list.keys.size());
        std::iota(waiting.begin(), waiting.end(), 0u);
        scan.Start();
    }

    void Pump();

private:
    [[nodiscard]] EntryPlan Plan(u32 index) const;
    bool TakeFound();
    void Replan();
    void BuildReady(u64 until_ns);
    void Merge();

    PipelineCache& cache;
    const ParsedList list;
    std::unordered_map<u64, u64> code_at; ///< every program the scan found: its code's address
    LoadBuilder builder;
    ShaderScan scan;
    std::vector<u32> waiting; ///< entries the scan has not found every program of
    std::deque<u32> ready;    ///< entries to build
    bool loading = false;
    bool store_off_logged = false;
};

EntryPlan PrecompileLoad::Plan(u32 index) const {
    return PlanEntry(
        list.keys[index].stages, [this](u64 hash) { return cache.ProgramNeeded(hash); },
        [this](u64 hash) { return code_at.contains(hash); });
}

/// The programs the scan found since the last call (one address each, the first seen); true
/// when there were any.
bool PrecompileLoad::TakeFound() {
    const auto found = scan.TakeNew();
    for (const auto& code : found) {
        code_at.emplace(code.hash, code.address);
    }
    return !found.empty();
}

/// Queues the waiting entries the scan has found every program of and drops those with no
/// needed program.
void PrecompileLoad::Replan() {
    std::erase_if(waiting, [this](u32 index) {
        const auto plan = Plan(index);
        if (plan == EntryPlan::Build) {
            ready.push_back(index);
        }
        return plan != EntryPlan::Wait;
    });
}

/// Builds queued entries until `until_ns`; merges when the queue ran empty after a build.
void PrecompileLoad::BuildReady(u64 until_ns) {
    bool tried = false;
    while (!ready.empty() && CompilePolicy::Now() < until_ns) {
        const u32 index = ready.front();
        ready.pop_front();
        // The game may have used its programs since it was queued (found code stays found).
        if (Plan(index) != EntryPlan::Build) {
            continue;
        }
        builder.Build(index, list.keys[index]);
        tried = true;
    }
    if (tried && ready.empty()) {
        Merge();
    }
}

/// Flushes the store and adds the permutations written since the last merge to its index.
void PrecompileLoad::Merge() {
    const auto written = builder.TakeWritten();
    if (written.empty()) {
        return;
    }
    Storage::DataBase::Instance().Flush();
    cache.AddSavedPermutations(written);
}

void PrecompileLoad::Pump() {
    if (!cache.DiskShadersActive()) {
        if (!store_off_logged) {
            store_off_logged = true;
            std::printf("Precompile load: nothing is built while the shader store is not read on "
                        "demand (disk shaders)\n");
        }
        scan.SetLoading(false);
        return;
    }
    const bool now_loading = g_compile_policy.NoSceneFor(CompilePolicy::Now());
    scan.SetLoading(now_loading);
    if (now_loading != loading) {
        loading = now_loading;
        if (loading) {
            Replan(); // the game has used programs since the last load
        } else {
            Merge(); // the load ended before the queue ran empty
        }
    }
    if (!loading) {
        return;
    }
    if (TakeFound()) {
        Replan();
    }
    BuildReady(CompilePolicy::Now() + PumpNs);
}

void PrecompileLoadDeleter::operator()(PrecompileLoad* load) const {
    delete load;
}

PrecompileLoadPtr MakePrecompileLoad(PipelineCache& cache) {
    if (!LoadRequested()) {
        return nullptr;
    }
    if (BootRequested()) {
        std::printf("Precompile load: GOW3_PRECOMPILE_BOOT=1 is ignored with "
                    "GOW3_PRECOMPILE_LOAD=1, the list is built while loading\n");
    }
    const char* path = std::getenv("GOW3_PRECOMPILE_LIST");
    if (!path || !path[0]) {
        std::printf("Precompile load: GOW3_PRECOMPILE_LIST=<file> is not set, off\n");
        return nullptr;
    }
#ifndef __linux__
    (void)cache;
    std::printf("Precompile load: not supported on this platform\n");
    return nullptr;
#else
    auto parsed = LoadListFile(path, MaxListEntries);
    if (!parsed || !SetUpFaultGuard()) {
        return nullptr;
    }
    std::printf("Precompile load: %zu entries of %s, built while loading\n", parsed->keys.size(),
                path);
    return PrecompileLoadPtr{new PrecompileLoad(cache, std::move(*parsed))};
#endif
}

void PipelineCache::PumpPrecompileLoad() {
    if (precompile_load) {
        precompile_load->Pump();
    }
}

} // namespace Vulkan
