// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_LOAD=1 with GOW3_PRECOMPILE_LIST=<file> (vk_precompile_load.h):
// while loading (CompilePolicy::NoSceneFor) a shader scan of the loader's own finds each
// program's code in the game's memory. The loader copies each list program's code out of it
// and keeps the copy only when its header and its body (the header's crc32) check out: the game
// may be writing or reusing that memory, and the copy is what gets translated, so nothing the
// game does later reaches the translation. An entry with a needed program whose stages all have
// a copy is built by the start-up rebuild's entry build (LoadBuilder) from the copies, which
// writes the needed programs' permutations to the store as the rebuild does; at most 4 ms of
// entries per submission. When the queue runs empty after a build, the store is flushed and the new
// permutations are added to its index, so a program's first use in this launch reads them
// (LoadDiskProgram) instead of translating it.
//
// The first draw of the race asks for programs before the pump has built them: a draw's first
// use of a program while the loader is active holds the draw (Hold) until what the scan finds
// is built, at most 5 s per loading state, and never outside one.
//
// Threads: the pump and the hold never run at once, both are on the GPU command thread. The
// pump runs in Rasterizer::OnSubmit, which only Liverpool::Process calls (liverpool.cpp:172).
// The hold runs in GetProgram for PipelineCache::sel only: draw-preparation workers and the
// loader's own builds return before it (sel.worker, sel.boot), and sel is selected only by
// GetGraphicsPipeline and GetComputePipeline, which Rasterizer's Draw, DrawIndirect and
// Dispatch calls use on the thread that decodes the command lists, Liverpool::Process again
// (liverpool.cpp:155); the draw recording thread never selects. So disk_permutations, which
// has no lock, and the loader's state need none. The loading state is read from CompilePolicy's
// atomics on this thread, so nothing of the loader is shared with the present thread.
//
// Linux only (the scan); elsewhere the switch logs that it is not supported.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_set>
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

constexpr u64 PumpNs = 4'000'000;               ///< ready entries one pump builds, at most
constexpr u64 ReportIntervalNs = 5'000'000'000; ///< the line of programs used
constexpr auto HoldPoll = std::chrono::milliseconds(1);

} // namespace

struct PrecompileLoad {
    PrecompileLoad(PipelineCache& cache_, ParsedList list_)
        : cache{cache_}, list{std::move(list_)}, builder{cache, list, code} {
        waiting.resize(list.keys.size());
        std::iota(waiting.begin(), waiting.end(), 0u);
        for (const auto& key : list.keys) {
            for (const auto& stage : key.stages) {
                list_programs.insert(stage.hash);
            }
        }
        scan.Start();
    }

    void Pump();
    void FirstUse(u64 hash);

private:
    /// One loading state's counts, for its line.
    struct Tally {
        u32 built = 0, skipped = 0, rejected = 0;
        u32 unverified = 0; ///< list programs whose copy failed its checks
        u64 permutations = 0;
        u64 held_ns = 0;
        u64 passes_before = 0; ///< scan passes when the loading state began
        std::vector<std::pair<const char*, u32>> reasons;
    };

    [[nodiscard]] EntryPlan Plan(u32 index) const;
    bool Loading();
    bool TakeFound();
    void Replan();
    void BuildReady(u64 until_ns);
    void Hold();
    void Merge();
    void ReportLoad() const;
    void ReportUsed() const;

    PipelineCache& cache;
    const ParsedList list;
    std::map<u64, std::vector<u32>> code; ///< list programs' checked copies of their code
    LoadBuilder builder;
    ShaderScan scan;
    std::vector<u32> waiting; ///< entries the scan has not found every program of
    std::deque<u32> ready;    ///< entries to build
    bool loading = false;
    bool store_off_logged = false;
    u64 found_in = 0; ///< scan.Started() when code an entry lacked was last taken
    Tally tally;
    std::unordered_set<u64> list_programs; ///< every stage's program of the list
    std::unordered_set<u64> unverified;    ///< list programs whose copy failed its checks
    std::unordered_set<u64> used;          ///< list programs first used since the load began
    std::unordered_set<u64> built;         ///< programs the loader wrote permutations of
    u64 next_report_ns = 0;
};

EntryPlan PrecompileLoad::Plan(u32 index) const {
    return PlanEntry(
        list.keys[index].stages, [this](u64 hash) { return cache.ProgramNeeded(hash); },
        [this](u64 hash) { return code.contains(hash); });
}

/// Whether the loading state holds now. A load that begins prints the last line of programs
/// used and starts its counts; one that ends (its first scene frame) merges and prints its
/// line.
bool PrecompileLoad::Loading() {
    const bool now_loading = g_compile_policy.NoSceneFor(CompilePolicy::Now());
    scan.SetLoading(now_loading);
    if (now_loading == loading) {
        return loading;
    }
    loading = now_loading;
    if (loading) {
        ReportUsed();
        used.clear();
        tally = {};
        tally.passes_before = scan.Passes();
        Replan(); // the game has used programs since the last load
    } else {
        Merge(); // the load ended before the queue ran empty
        ReportLoad();
    }
    return loading;
}

/// Copies the list programs' code the scan found since the last call (one address each, the
/// first seen) and plans the waiting entries again; true when an entry with a needed program
/// lacked some of it. A copy that cannot be read or fails CheckCode or CheckCodeBody is not
/// kept: its entries are not built, and the game translates the program at its first use.
bool PrecompileLoad::TakeFound() {
    const auto found = scan.TakeNew();
    if (found.empty()) {
        return false;
    }
    std::unordered_set<u64> fresh;
    for (const auto& [hash, address] : found) {
        if (!list_programs.contains(hash) || code.contains(hash) || unverified.contains(hash)) {
            continue;
        }
        auto words = ReadProgramCode(address, ReadGuest);
        if (words.empty() || CheckCode(hash, words) || CheckCodeBody(words)) {
            unverified.insert(hash);
            ++tally.unverified;
            continue;
        }
        code.emplace(hash, std::move(words));
        fresh.insert(hash);
    }
    const bool lacked = std::ranges::any_of(waiting, [this, &fresh](u32 index) {
        return EntryLacked(
            list.keys[index].stages, [this](u64 hash) { return cache.ProgramNeeded(hash); },
            [&fresh](u64 hash) { return fresh.contains(hash); });
    });
    if (lacked) {
        found_in = scan.Started();
    }
    Replan();
    return lacked;
}

/// Queues the waiting entries the scan has found every program of and drops those with no
/// needed program.
void PrecompileLoad::Replan() {
    std::erase_if(waiting, [this](u32 index) {
        const auto plan = Plan(index);
        if (plan == EntryPlan::Build) {
            ready.push_back(index);
        }
        tally.skipped += plan == EntryPlan::Skip;
        return plan != EntryPlan::Wait;
    });
}

/// Builds queued entries until `until_ns`; merges once the queue is empty (also when an earlier
/// call's builds are still unmerged).
void PrecompileLoad::BuildReady(u64 until_ns) {
    while (!ready.empty() && CompilePolicy::Now() < until_ns) {
        const u32 index = ready.front();
        ready.pop_front();
        // The game may have used its programs since it was queued (found code stays found).
        if (Plan(index) != EntryPlan::Build) {
            ++tally.skipped;
            continue;
        }
        const char* why = builder.Build(index, list.keys[index]);
        if (!why) {
            ++tally.built;
            continue;
        }
        ++tally.rejected;
        const auto reason =
            std::ranges::find(tally.reasons, why, &std::pair<const char*, u32>::first);
        if (reason != tally.reasons.end()) {
            ++reason->second;
        } else {
            tally.reasons.emplace_back(why, 1);
        }
    }
    if (ready.empty()) {
        Merge(); // nothing when nothing was written
    }
}

/// Builds every queued entry, then has the scan run a pass that starts after that and builds
/// what it found, until such a pass finds no code an entry with a needed program lacked or the
/// loading state's holds took HoldBudgetNs; then merges, so the GetProgram this holds finds
/// what was built in the store. Not a pending draw, and no grace wait.
void PrecompileLoad::Hold() {
    const u64 began = CompilePolicy::Now();
    const auto held = [&] { return tally.held_ns + (CompilePolicy::Now() - began); };
    for (;;) {
        BuildReady(began + (HoldBudgetNs - tally.held_ns));
        if (held() >= HoldBudgetNs) {
            break;
        }
        const u64 started = scan.Started();
        scan.Wake();
        while (!WaitedPassDone(started, scan.Passes()) && held() < HoldBudgetNs) {
            std::this_thread::sleep_for(HoldPoll);
        }
        if (HoldEnds(TakeFound(), held())) {
            break;
        }
    }
    Merge();
    tally.held_ns = held();
}

/// Flushes the store and adds the permutations written since the last merge to its index.
void PrecompileLoad::Merge() {
    const auto written = builder.TakeWritten();
    if (written.empty()) {
        return;
    }
    Storage::DataBase::Instance().Flush();
    cache.AddSavedPermutations(written);
    tally.permutations += written.size();
    for (const auto& [hash, perm] : written) {
        built.insert(hash);
    }
}

void PrecompileLoad::ReportLoad() const {
    std::string reasons;
    for (const auto& [why, count] : tally.reasons) {
        reasons += (reasons.empty() ? " (" : ", ") + std::to_string(count) + " " + why;
    }
    if (!reasons.empty()) {
        reasons += ")";
    }
    std::printf("Precompile load: %u entries built (%llu permutations), %u skipped, %u "
                "rejected%s, %u programs' code not verified, hold %llu ms, %llu scan passes "
                "(last %llu ms)\n",
                tally.built, static_cast<unsigned long long>(tally.permutations), tally.skipped,
                tally.rejected, reasons.c_str(), tally.unverified,
                static_cast<unsigned long long>(tally.held_ns / 1'000'000),
                static_cast<unsigned long long>(scan.Passes() - tally.passes_before),
                static_cast<unsigned long long>(scan.LastPassMs()));
}

/// Every 5 s outside a load, and once more as the next load begins, before it clears the
/// count. "Built at the load": by the loader in this launch.
void PrecompileLoad::ReportUsed() const {
    size_t of_them = 0;
    for (const u64 hash : used) {
        of_them += built.contains(hash);
    }
    std::printf("Precompile load: %zu list programs used since the load, %zu of them built at "
                "the load\n",
                used.size(), of_them);
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
    if (!Loading()) {
        if (const u64 now = CompilePolicy::Now(); now >= next_report_ns) {
            next_report_ns = now + ReportIntervalNs;
            ReportUsed();
        }
        return;
    }
    TakeFound();
    BuildReady(CompilePolicy::Now() + PumpNs);
}

/// GetProgram, a program not in program_cache: before the store is asked for it.
void PrecompileLoad::FirstUse(u64 hash) {
    const bool now_loading = cache.DiskShadersActive() && Loading(); // may begin a load first
    if (list_programs.contains(hash)) {
        used.insert(hash);
    }
    if (!now_loading) {
        return;
    }
    TakeFound();
    if (HoldActive(loading, tally.held_ns, !ready.empty(),
                   LastPassFound(found_in, scan.Passes()))) {
        Hold();
    }
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

void PipelineCache::PrecompileLoadFirstUse(u64 hash) {
    if (precompile_load) {
        precompile_load->FirstUse(hash);
    }
}

} // namespace Vulkan
