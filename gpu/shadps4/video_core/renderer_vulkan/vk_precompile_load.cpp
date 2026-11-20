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
// (LoadDiskProgram) instead of translating it. An entry with no needed program is built too (it
// only binds its stages, writes nothing) once its code is found, so its pipeline selection can
// add variants to the programs that exist; with GOW3_PRECOMPILE_LOAD_VARIANTS=0 it is skipped.
// Such a checked entry waits in a queue of its own, which the pump builds only while no entry
// with a needed program is queued, and a first use never holds for it (see below). Checked
// entries not built when the next loading state begins move to a third queue, which only the
// pump builds, after this load's checked ones; no hold waits for them.
//
// Then each built entry's pipeline is selected again, lookup-only as a draw-preparation worker
// does but reading its programs from the store (LoadBuilder::Pipeline), and handed to the
// compile workers as a draw's background build is, while their queue has room; the draw path
// publishes it (PublishGraphics). A translated shader alone does not draw: a draw whose
// pipeline is still building is skipped after a short grace wait (GetGraphicsPipeline). A
// selection that misses a variant (permutation) of a program in use or saved, which the entry
// build only binds, translates it from the selection's shadow copy with the entry build's
// checks, adds it to the program at the index the game's own translation would take and saves
// it there, so the store stays the game's across launches, and selects again; while the game
// translates a variant of that program it is left to the game, as one that fails a check is.
// The load line counts both. A selection that misses a program or a variant not added, and a
// failed build, are dropped and counted; the race's draw then builds that pipeline itself.
//
// The first draw of the race asks for programs before the pump has built them: a draw's first
// use of a program while the loader is active holds the draw (Hold) until what the scan finds
// is built and its pipelines too. Once the pump has built every entry but not handed every
// pipeline over, a scene's draws have no first use left to hold at (the loader's own
// selections put their programs in program_cache), so a draw's pipeline miss while built
// entries' pipelines are not handed over or still building waits for them as the end of a hold
// does (HoldPipelines); without it the scene's first draws build those pipelines themselves
// and are skipped. Once those are waited for, that hold also builds checked entries, as their
// pipelines too are ones a draw would otherwise build itself, at most 1 s per loading state
// (CheckedHoldNs), and waits for their pipelines; the pump builds the rest. A first use's hold
// does not, as a checked entry has no program a first use waits for. A draw of a pipeline the
// loader is building waits for it (Wait). All three at most 5 s per loading state together, and
// never outside one.
//
// Diagnostic GOW3_PRECOMPILE_LOAD_NO_PUMP_PIPELINES=1: the pump still builds entries but hands
// none of their pipelines over, so only a hold does. With a count N > 1 instead it hands at most
// N over per loading state (every entry its selections take off the queue counts, also one
// dropped or built already) and then stops, as a short loading screen ends the pump part way;
// the holds are not limited. It shows what the pump's handover adds to the pipelines a scene
// starts with, against what the holds and the draws build themselves.
//
// GOW3_PRECOMPILE_LOAD_VARIANTS=0 (read once, logged as the loader starts) adds no variant: a
// selection that misses one is dropped as before, and counted among those left to the game.
//
// Diagnostic GOW3_PRECOMPILE_LOAD_TRACE=1: when a scene's draw still builds a pipeline itself,
// either the list has no entry for it (the recorder missed the draw) or the loader dropped its
// entry or built another key; and how visible the skips are depends on how many frames they
// fall in. So each entry keeps what became of it and the key its selection found; a dropped
// selection prints why (at most 32 per loading state); a draw that starts its own build prints
// its key and the entries with the same programs, their outcome and key (at most 200 per run);
// and each flip after a frame with skipped draws prints their count (at most 300 per run). Its
// index of the list's programs is built once, at the start; without it nothing is counted.
//
// Threads: the pump and the hold never run at once, both are on the GPU command thread. The
// pump runs in Rasterizer::OnSubmit, which only Liverpool::Process calls, after a submission.
// The hold runs in GetProgram for PipelineCache::sel only: draw-preparation workers and the
// loader's own builds return before it (sel.worker, sel.boot), and sel is selected only by
// GetGraphicsPipeline and GetComputePipeline, which Rasterizer's Draw, DrawIndirect and
// Dispatch calls use on the thread that decodes the command lists, Liverpool::Process again,
// which resumes them; the draw recording thread never selects. So disk_permutations, which
// has no lock, and the loader's state need none. The loading state is read from CompilePolicy's
// atomics on this thread, so nothing of the loader is shared with the present thread, but for
// the trace's frame line: it is printed on the video-out thread at each flip (LoadTraceFlipped)
// and reads only atomics, the skipped draws this thread counts and when the last load ended. The
// pipeline selections run on this thread too and add programs to program_cache and variants to
// its programs under its lock, as GetProgram does; a hold inside GetProgram finds its program
// again afterwards.
// HoldPipelines runs in GetGraphicsPipeline after sel's selection, on the same thread; the
// selections it makes never come back to it, as only Rasterizer calls GetGraphicsPipeline.
//
// Linux only (the scan); elsewhere the switch logs that it is not supported.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

constexpr u64 PumpNs = 4'000'000;               ///< queued entries one pump builds, at most
constexpr u64 ReportIntervalNs = 5'000'000'000; ///< the line of programs used
constexpr auto HoldPoll = std::chrono::milliseconds(1);
constexpr u32 MaxFailedCopies = 2; ///< of a program: then its code counts as not verified
constexpr u32 MaxTraceDropped = 32;  ///< trace lines of dropped selections per loading state
constexpr u32 MaxTraceDraws = 200;   ///< trace lines of draws building their pipeline, per run
constexpr u32 MaxTraceFrames = 300;  ///< trace lines of frames with skipped draws, per run
constexpr size_t MaxTraceMatches = 4; ///< list entries a draw's trace line names

/// How many pipelines the pump hands over per loading state at most: none for
/// GOW3_PRECOMPILE_LOAD_NO_PUMP_PIPELINES=1, N for a count N > 1; no limit when it is unset or
/// not a count.
[[nodiscard]] std::optional<u32> PumpPipelineLimit() {
    const char* value = std::getenv("GOW3_PRECOMPILE_LOAD_NO_PUMP_PIPELINES");
    if (!value) {
        return std::nullopt;
    }
    const std::string_view text{value};
    u32 count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc{} || end != text.data() + text.size() || count == 0) {
        return std::nullopt;
    }
    return count == 1 ? 0 : count;
}

/// Probe switches (off unless set to a count > 0): GOW3_PRECOMPILE_LOAD_PUMP_MS, the pump's time
/// per submission in place of PumpNs; GOW3_PRECOMPILE_LOAD_MERGE_EVERY, the pump and the holds
/// merge after that many built entries and the pump hands pipelines over before it builds, so
/// the compile workers start during a wave instead of after it.
[[nodiscard]] u32 EnvCount(const char* name) {
    const char* value = std::getenv(name);
    if (!value) {
        return 0;
    }
    const std::string_view text{value};
    u32 count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    return error == std::errc{} && end == text.data() + text.size() ? count : 0;
}

/// Off only for GOW3_PRECOMPILE_LOAD_VARIANTS=0.
[[nodiscard]] bool VariantsRequested() {
    const char* value = std::getenv("GOW3_PRECOMPILE_LOAD_VARIANTS");
    return !value || std::string_view{value} != "0";
}

[[nodiscard]] bool TraceRequested() {
    const char* value = std::getenv("GOW3_PRECOMPILE_LOAD_TRACE");
    return value && std::string_view{value} == "1";
}

/// With the trace: when the last loading state ended (CompilePolicy::Now()), 0 while one holds
/// or before the first ended. Written on the GPU command thread, read at each flip too.
std::atomic<u64> g_load_trace_ended_ns{0};
std::atomic<u64> g_load_trace_flip_draws{0}; ///< the draw count at the last flip
std::atomic<u32> g_load_trace_frames{0};     ///< frame lines printed

/// The stages in the order they run: vs, hs, ds, gs, ps.
[[nodiscard]] u32 StageOrder(u32 sw_stage) {
    switch (Shader::SwStage(sw_stage)) {
    case Shader::SwStage::Vertex:
        return 0;
    case Shader::SwStage::TessellationControl:
        return 1;
    case Shader::SwStage::TessellationEval:
        return 2;
    case Shader::SwStage::Geometry:
        return 3;
    case Shader::SwStage::Fragment:
        return 4;
    default:
        return 5;
    }
}

[[nodiscard]] const char* StageName(u32 sw_stage) {
    switch (Shader::SwStage(sw_stage)) {
    case Shader::SwStage::Vertex:
        return "vs";
    case Shader::SwStage::TessellationControl:
        return "hs";
    case Shader::SwStage::TessellationEval:
        return "ds";
    case Shader::SwStage::Geometry:
        return "gs";
    case Shader::SwStage::Fragment:
        return "ps";
    case Shader::SwStage::Compute:
        return "cs";
    default:
        return "stage?";
    }
}

[[nodiscard]] std::string Hex(u64 value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

/// "vs 0x…, ps 0x…" of SwStage index and program hash pairs.
[[nodiscard]] std::string StagesText(std::vector<std::pair<u32, u64>> stages) {
    std::ranges::sort(stages, {}, [](const auto& stage) { return StageOrder(stage.first); });
    std::string text;
    for (const auto& [sw_stage, hash] : stages) {
        text += (text.empty() ? "" : ", ") + std::string{StageName(sw_stage)} + " " + Hex(hash);
    }
    return text;
}

/// "while loading", or how long after the loading state that ended at `ended_ns` (0: none).
[[nodiscard]] std::string WhenText(u64 now_ns, u64 ended_ns) {
    if (ended_ns == 0) {
        return "while loading";
    }
    const u64 ms = now_ns > ended_ns ? (now_ns - ended_ns) / 1'000'000 : 0;
    return std::to_string(ms) + " ms after the load ended";
}

/// GOW3_PRECOMPILE_LOAD_TRACE: what became of each list entry, and an index of the list's
/// programs for a draw's line.
struct LoadTrace {
    enum class Outcome : u8 {
        Waiting,   ///< the scan has not found every stage's code
        Ready,     ///< every stage's code found, not built yet
        Skipped,   ///< no needed program: not built, so its pipeline is never selected
        Rejected,  ///< its build failed (`why`)
        NoInfo,    ///< a program in use whose translation was pending: not built
        Built,     ///< built, its pipeline not selected yet
        Cleared,   ///< built, its pipeline not handed over before the next load began
        Queued,    ///< its pipeline handed to the compile workers
        Have,      ///< its pipeline built or building already
        Dropped,   ///< its pipeline selection dropped (`why`, `stage`)
    };
    struct Entry {
        Outcome outcome = Outcome::Waiting;
        const char* why = nullptr;
        std::optional<u32> stage;
        std::optional<u64> key_hash;
    };

    explicit LoadTrace(const ParsedList& list) : entries(list.keys.size()) {
        for (u32 index = 0; index < list.keys.size(); ++index) {
            auto programs = Programs(list.keys[index]);
            for (const u64 hash : programs) {
                ++entries_with[hash];
            }
            by_programs[std::move(programs)].push_back(index);
        }
    }

    /// The entry's stage programs, sorted, each once.
    [[nodiscard]] static std::vector<u64> Programs(const RecordedKey& key) {
        std::vector<u64> programs;
        for (const auto& stage : key.stages) {
            programs.push_back(stage.hash);
        }
        std::ranges::sort(programs);
        programs.erase(std::ranges::unique(programs).begin(), programs.end());
        return programs;
    }

    [[nodiscard]] static std::vector<std::pair<u32, u64>> Stages(const RecordedKey& key) {
        std::vector<std::pair<u32, u64>> stages;
        for (const auto& stage : key.stages) {
            stages.emplace_back(stage.sw_stage, stage.hash);
        }
        return stages;
    }

    [[nodiscard]] static std::string Why(const Entry& entry) {
        std::string why = entry.stage ? std::string{StageName(*entry.stage)} + " " : "";
        return why + (entry.why ? entry.why : "unknown");
    }

    [[nodiscard]] static std::string OutcomeText(const Entry& entry) {
        switch (entry.outcome) {
        case Outcome::Waiting:
            return "waiting for code";
        case Outcome::Ready:
            return "ready, not built";
        case Outcome::Skipped:
            return "skipped (no needed program)";
        case Outcome::Rejected:
            return "rejected (" + Why(entry) + ")";
        case Outcome::NoInfo:
            return "not built (a program's translation pending)";
        case Outcome::Built:
            return "built, not selected";
        case Outcome::Cleared:
            return "built, not handed over before the next load";
        case Outcome::Queued:
            return "queued";
        case Outcome::Have:
            return "had (built or building already)";
        case Outcome::Dropped:
            return "dropped (" + Why(entry) + ")";
        }
        return "?";
    }

    void Set(u32 index, Outcome outcome, const char* why = nullptr) {
        entries[index].outcome = outcome;
        entries[index].why = why;
        entries[index].stage.reset();
    }

    std::vector<Entry> entries;
    std::map<std::vector<u64>, std::vector<u32>> by_programs; ///< Programs() -> entries
    std::unordered_map<u64, u32> entries_with; ///< program hash -> entries holding it
    u32 dropped_lines = 0; ///< this loading state's
    u32 draw_lines = 0;
    u64 ended_ns = 0; ///< when the last loading state ended, 0 while one holds or none ended
};

} // namespace

std::atomic<bool> Precompile::g_load_trace{false};
std::atomic<u32> Precompile::g_load_trace_skipped{0};

struct PrecompileLoad {
    PrecompileLoad(PipelineCache& cache_, ParsedList list_)
        : cache{cache_}, list{std::move(list_)}, variants{VariantsRequested()},
          builder{cache, list, code, variants}, pump_limit{PumpPipelineLimit()},
          pump_ms{EnvCount("GOW3_PRECOMPILE_LOAD_PUMP_MS")},
          merge_every{EnvCount("GOW3_PRECOMPILE_LOAD_MERGE_EVERY")},
          trace{TraceRequested() ? std::make_unique<LoadTrace>(list) : nullptr} {
        if (!variants) {
            std::printf("Precompile load: GOW3_PRECOMPILE_LOAD_VARIANTS=0, a variant missing from "
                        "a program in use or saved is left to the game\n");
        }
        std::printf("Precompile load: probe switches: pump %llu ms a submission, merge every %u "
                    "built entries (0 = when the queue is empty)\n",
                    static_cast<unsigned long long>(PumpBudgetNs() / 1'000'000), merge_every);
        if (pump_limit == 0u) {
            std::printf("Precompile load: GOW3_PRECOMPILE_LOAD_NO_PUMP_PIPELINES=1, only holds "
                        "hand pipelines over\n");
        } else if (pump_limit) {
            std::printf("Precompile load: GOW3_PRECOMPILE_LOAD_NO_PUMP_PIPELINES=%u, the pump "
                        "hands at most %u pipelines over per load, then only holds do\n",
                        *pump_limit, *pump_limit);
        }
        if (trace) {
            std::printf("Precompile load: GOW3_PRECOMPILE_LOAD_TRACE=1, lines for dropped "
                        "selections, pipelines draws build themselves and frames with skipped "
                        "draws\n");
            g_load_trace.store(true, std::memory_order_relaxed);
        }
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
    bool HoldPipelines();
    bool Wait(const std::function<bool(u64 left_ns)>& wait);
    void TraceDraw(const PipelineSelection& selection);

private:
    /// One loading state's counts, for its line.
    struct Tally {
        u32 built = 0, skipped = 0, rejected = 0;
        u32 checked = 0;    ///< built entries with no needed program, selected for their variants
        u32 no_info = 0;    ///< entries with a program in use whose translation is pending
        u32 unverified = 0; ///< list programs whose copies failed their checks
        u64 permutations = 0;
        u64 held_ns = 0;
        u64 checked_held_ns = 0; ///< of `held_ns`, pipeline misses' holds building `checking`
        u64 passes_before = 0; ///< scan passes when the loading state began
        u32 dropped = 0;       ///< pipeline selections that found no pipeline
        u32 have = 0;          ///< pipeline selections whose pipeline was built or building
        u64 published_before = 0, failed_before = 0; ///< the cache's load builds then
        u64 began_ns = 0; ///< CompilePolicy::Now() when the loading state began
        u32 holds = 0;    ///< Hold() and HoldPipelines() calls that held
        u32 pumped = 0;   ///< entries the pump's pipeline selections took off `selecting`
        std::vector<std::pair<const char*, u32>> reasons;
        // Probe: where a loading state's time went (ms of the load are from began_ns).
        u32 pumps = 0, pump_built = 0;
        u64 pump_take_ns = 0, pump_build_ns = 0, pump_select_ns = 0, pump_max_ns = 0;
        u64 wave_ns = 0;            ///< when `ready` first held 100 entries or more
        u64 ready_empty_ns = 0;     ///< when `ready` was first empty after that
        u64 selecting_empty_ns = 0; ///< when `selecting` and `unmerged` were first empty after that
        u64 hold_build_ns = 0, hold_pass_ns = 0, hold_pipeline_ns = 0; ///< of the current hold
        u32 hold_passes = 0;
    };

    [[nodiscard]] u64 PumpBudgetNs() const {
        return pump_ms ? u64{pump_ms} * 1'000'000 : PumpNs;
    }
    void ProbeMarks();

    [[nodiscard]] EntryPlan Plan(u32 index) const;
    [[nodiscard]] bool Checked(u32 index) const;
    bool Loading();
    bool TakeFound();
    void Replan();
    void BuildReady(u64 until_ns, std::deque<u32>& queue);
    u32 SelectPipelines(u64 until_ns, u32 most = std::numeric_limits<u32>::max());
    void TraceSelection(u32 index, PipelineStep step, const PipelineTrace& found);
    void Hold();
    u32 WaitPipelines(u64 began);
    void EndHold(u64 began, u32 built_before, u32 handed, size_t not_handed, size_t building);
    void Merge();
    void ReportLoad() const;
    void ReportUsed() const;

    PipelineCache& cache;
    const ParsedList list;
    std::map<u64, std::vector<u32>> code; ///< list programs' checked copies of their code
    const bool variants; ///< GOW3_PRECOMPILE_LOAD_VARIANTS not 0: missing variants are added
    LoadBuilder builder;
    ShaderScan scan;
    std::vector<u32> waiting; ///< entries the scan has not found every program of
    std::deque<u32> ready;    ///< entries with a needed program to build
    std::deque<u32> checking; ///< checked entries to build (no needed program), after `ready`
    /// Checked entries still in `checking` when a loading state began: only the pump builds
    /// them, after `checking`; no hold waits for them.
    std::deque<u32> stale_checking;
    std::vector<u32> unmerged; ///< entries built since the last merge
    std::deque<u32> selecting; ///< entries built and merged whose pipeline is not handed over
    bool loading = false;
    bool store_off_logged = false;
    u64 found_in = 0; ///< scan.Started() when code an entry lacked was last taken
    Tally tally;
    std::unordered_set<u64> list_programs; ///< every stage's program of the list
    std::unordered_set<u64> unverified;    ///< list programs whose copies failed their checks
    std::unordered_map<u64, u32> failed_copies; ///< copies of a list program that failed
    std::unordered_set<u64> used;          ///< list programs first used since the load began
    std::unordered_set<u64> built;         ///< programs the loader wrote permutations of
    u64 next_report_ns = 0;
    /// GOW3_PRECOMPILE_LOAD_NO_PUMP_PIPELINES: pipelines the pump hands over per loading state
    const std::optional<u32> pump_limit;
    const u32 pump_ms;     ///< GOW3_PRECOMPILE_LOAD_PUMP_MS, 0 = PumpNs
    const u32 merge_every; ///< GOW3_PRECOMPILE_LOAD_MERGE_EVERY, 0 = merge when `ready` is empty
    const std::unique_ptr<LoadTrace> trace; ///< GOW3_PRECOMPILE_LOAD_TRACE, null without it
};

EntryPlan PrecompileLoad::Plan(u32 index) const {
    return PlanEntry(
        variants, list.keys[index].stages,
        [this](u64 hash) { return cache.ProgramNeeded(hash); },
        [this](u64 hash) { return code.contains(hash); });
}

/// Whether entry `index` has no needed program, PlanEntry's test for Skip: with variants on
/// (check_existing) it is built all the same, only for the variants its pipeline selection adds
/// to programs that exist, a checked entry.
bool PrecompileLoad::Checked(u32 index) const {
    return NoNeededProgram(list.keys[index].stages,
                           [this](u64 hash) { return cache.ProgramNeeded(hash); });
}

/// Whether the loading state holds now. A load that begins prints the last line of programs
/// used, starts its counts and drops the last load's pipelines not handed over; one that ends
/// (its first scene frame) merges and prints its line.
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
        builder.ResetVariants();
        builder.ForgetFailedVariants();
        tally.began_ns = CompilePolicy::Now();
        tally.passes_before = scan.Passes();
        tally.published_before = cache.load_builds_published;
        tally.failed_before = cache.load_builds_failed;
        if (trace) {
            trace->dropped_lines = 0;
            trace->ended_ns = 0;
            g_load_trace_ended_ns.store(0, std::memory_order_relaxed);
            for (const u32 index : selecting) {
                trace->Set(index, LoadTrace::Outcome::Cleared);
            }
        }
        // Pipelines the last load did not hand over: the race has built what it drew of them,
        // and the rest would take this load's budget.
        selecting.clear();
        // The last load's checked entries not built: the pump builds them after this load's,
        // and they neither start nor lengthen a hold.
        stale_checking.insert(stale_checking.end(), checking.begin(), checking.end());
        checking.clear();
        Replan(); // the game has used programs since the last load
    } else {
        Merge(); // the load ended before the queue ran empty
        ReportLoad();
        if (trace) {
            trace->ended_ns = CompilePolicy::Now();
            g_load_trace_ended_ns.store(trace->ended_ns, std::memory_order_relaxed);
        }
    }
    return loading;
}

/// Copies the list programs' code the scan found since the last call (one address each, the
/// first seen) and plans the waiting entries again; true when an entry with a needed program
/// lacked some of it. A copy that cannot be read or fails CheckCode or CheckCodeBody is not
/// kept, and the scan forgets the program so a later pass may find another copy; after
/// MaxFailedCopies its entries are not built, and the game translates it at its first use.
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
            if (++failed_copies[hash] < MaxFailedCopies) {
                scan.Forget(hash);
            } else {
                unverified.insert(hash);
                ++tally.unverified;
            }
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

/// Queues the waiting entries the scan has found every program of, those with a needed program
/// on `ready` and the checked ones on `checking`; drops those with no needed program while
/// variants are off (GOW3_PRECOMPILE_LOAD_VARIANTS=0).
void PrecompileLoad::Replan() {
    std::erase_if(waiting, [this](u32 index) {
        const auto plan = Plan(index);
        if (plan == EntryPlan::Build) {
            (Checked(index) ? checking : ready).push_back(index);
        }
        tally.skipped += plan == EntryPlan::Skip;
        if (trace && plan != EntryPlan::Wait) {
            trace->Set(index, plan == EntryPlan::Build ? LoadTrace::Outcome::Ready
                                                       : LoadTrace::Outcome::Skipped);
        }
        return plan != EntryPlan::Wait;
    });
}

/// Builds the entries of `queue` (`ready`, `checking` or `stale_checking`) until `until_ns`;
/// merges once `ready` is empty (also when an earlier call's builds are still unmerged).
void PrecompileLoad::BuildReady(u64 until_ns, std::deque<u32>& queue) {
    while (!queue.empty() && CompilePolicy::Now() < until_ns) {
        const u32 index = queue.front();
        queue.pop_front();
        // The game may have used its programs since it was queued (found code stays found):
        // with variants off the entry is skipped then; with them on it is checked instead, and
        // one taken from `ready` goes on to `checking`, which a first use's hold does not build.
        if (Plan(index) != EntryPlan::Build) {
            ++tally.skipped;
            if (trace) {
                trace->Set(index, LoadTrace::Outcome::Skipped);
            }
            continue;
        }
        const bool checked = Checked(index);
        if (checked && &queue == &ready) {
            checking.push_back(index);
            continue;
        }
        bool no_info = false;
        const char* why = builder.Build(index, list.keys[index], no_info);
        if (trace) {
            trace->Set(index,
                       !why      ? LoadTrace::Outcome::Built
                       : no_info ? LoadTrace::Outcome::NoInfo
                                 : LoadTrace::Outcome::Rejected,
                       why);
        }
        if (!why) {
            ++tally.built;
            tally.checked += checked;
            unmerged.push_back(index);
            if (merge_every && unmerged.size() >= merge_every) {
                Merge();
            }
            continue;
        }
        if (no_info) {
            ++tally.no_info; // not retried either
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

/// Hands built and merged entries' pipelines to the compile workers until `until_ns`, until
/// their queue holds LoadQueueDepth tasks or until it took `most` entries off `selecting`; a
/// selection adds no variant after its first past `until_ns` either (inside a hold that is the
/// hold's bound). Returns how many it took.
u32 PrecompileLoad::SelectPipelines(u64 until_ns, u32 most) {
    u32 taken = 0;
    while (!selecting.empty() && taken < most && CompilePolicy::Now() < until_ns) {
        const u32 index = selecting.front();
        PipelineTrace found;
        const auto step = builder.Pipeline(list.keys[index], until_ns, trace ? &found : nullptr);
        if (step == PipelineStep::Full) {
            break; // again on a later call (the queue was full or time is up)
        }
        selecting.pop_front();
        ++taken;
        tally.dropped += step == PipelineStep::Dropped;
        tally.have += step == PipelineStep::Have;
        if (trace) {
            TraceSelection(index, step, found);
        }
    }
    return taken;
}

/// The trace: what entry `index`'s pipeline selection found, and a line when it was dropped.
void PrecompileLoad::TraceSelection(u32 index, PipelineStep step, const PipelineTrace& found) {
    auto& entry = trace->entries[index];
    entry.outcome = step == PipelineStep::Queued ? LoadTrace::Outcome::Queued
                    : step == PipelineStep::Have ? LoadTrace::Outcome::Have
                                                 : LoadTrace::Outcome::Dropped;
    entry.why = found.dropped;
    entry.stage = found.stage;
    entry.key_hash = found.key_hash;
    if (step != PipelineStep::Dropped || trace->dropped_lines >= MaxTraceDropped) {
        return;
    }
    ++trace->dropped_lines;
    const auto& key = list.keys[index];
    std::printf("Precompile load: trace dropped entry %u: %s; stages %s (recorded key %s)\n",
                index, LoadTrace::Why(entry).c_str(), StagesText(LoadTrace::Stages(key)).c_str(),
                Hex(key.key_hash).c_str());
}

/// Builds every queued entry with a needed program (`ready`, not the checked ones), then has the
/// scan run a pass that starts after that and builds what it found, until such a pass finds no
/// code an entry with a needed program lacked or the loading state's holds took HoldBudgetNs;
/// then waits for the entries' pipelines in the same budget (WaitPipelines). Not a pending draw,
/// and no grace wait.
void PrecompileLoad::Hold() {
    const u64 began = CompilePolicy::Now();
    ++tally.holds;
    const u32 built_before = tally.built;
    const size_t not_handed = selecting.size() + unmerged.size();
    const size_t building = cache.LoadBuildsRunning();
    const u64 until = began + (HoldBudgetNs - tally.held_ns);
    const auto held = [&] { return tally.held_ns + (CompilePolicy::Now() - began); };
    for (;;) {
        const u64 t0 = CompilePolicy::Now();
        BuildReady(until, ready);
        const u64 t1 = CompilePolicy::Now();
        tally.hold_build_ns += t1 - t0;
        if (held() >= HoldBudgetNs) {
            break;
        }
        const u64 started = scan.Started();
        scan.Wake();
        while (!WaitedPassDone(started, scan.Passes()) && held() < HoldBudgetNs) {
            std::this_thread::sleep_for(HoldPoll);
        }
        tally.hold_pass_ns += CompilePolicy::Now() - t1;
        ++tally.hold_passes;
        if (HoldEnds(TakeFound(), held())) {
            break;
        }
    }
    const u64 t2 = CompilePolicy::Now();
    const u32 handed = WaitPipelines(began);
    tally.hold_pipeline_ns += CompilePolicy::Now() - t2;
    EndHold(began, built_before, handed, not_handed, building);
}

/// The second half of a hold that began at `began`: merges, so a GetProgram finds what was
/// built in the store, hands the built entries' pipelines to the compile workers and waits for
/// them until this loading state's holds took HoldBudgetNs, and publishes them, so the draw
/// finds them in graphics_pipelines. Returns how many it handed over.
u32 PrecompileLoad::WaitPipelines(u64 began) {
    u32 handed = 0;
    const u64 until = began + (HoldBudgetNs - tally.held_ns);
    const auto held = [&] { return tally.held_ns + (CompilePolicy::Now() - began); };
    Merge();
    for (;;) {
        handed += SelectPipelines(until);
        const bool left = !selecting.empty();
        const bool done = !left && cache.WaitLoadBuilds(until);
        if (PipelineWaitEnds(left, done, held())) {
            break;
        }
        if (left) {
            std::this_thread::sleep_for(HoldPoll); // the queue holds LoadQueueDepth tasks
        }
    }
    cache.PublishGraphics();
    return handed;
}

/// Counts the time of a hold that began at `began` as held and prints its line: when it began
/// in the load, what it built and handed over, and what was left of the pipelines then.
void PrecompileLoad::EndHold(u64 began, u32 built_before, u32 handed, size_t not_handed,
                             size_t building) {
    const u64 this_hold = CompilePolicy::Now() - began;
    tally.held_ns += this_hold;
    std::printf("Precompile load: hold %u at %llu ms of the load, %llu ms, %u entries built, %u "
                "pipelines handed over, %zu not handed over and %zu building at its start\n",
                tally.holds, static_cast<unsigned long long>((began - tally.began_ns) / 1'000'000),
                static_cast<unsigned long long>(this_hold / 1'000'000), tally.built - built_before,
                handed, not_handed, building);
    std::printf("Precompile load: probe hold %u: %llu ms building entries, %llu ms waiting for "
                "%u scan passes, %llu ms on pipelines\n",
                tally.holds, static_cast<unsigned long long>(tally.hold_build_ns / 1'000'000),
                static_cast<unsigned long long>(tally.hold_pass_ns / 1'000'000), tally.hold_passes,
                static_cast<unsigned long long>(tally.hold_pipeline_ns / 1'000'000));
    tally.hold_build_ns = tally.hold_pass_ns = tally.hold_pipeline_ns = 0;
    tally.hold_passes = 0;
    ProbeMarks();
}

/// Probe: notes when the big wave arrived and when its queues first ran empty.
void PrecompileLoad::ProbeMarks() {
    const u64 at = CompilePolicy::Now() - tally.began_ns;
    if (!tally.wave_ns) {
        if (ready.size() >= 100) {
            tally.wave_ns = at;
        }
        return;
    }
    if (!tally.ready_empty_ns && ready.empty()) {
        tally.ready_empty_ns = at;
    }
    if (!tally.selecting_empty_ns && tally.ready_empty_ns && selecting.empty() &&
        unmerged.empty()) {
        tally.selecting_empty_ns = at;
    }
}

/// Flushes the store and adds the permutations written since the last merge to its index; the
/// entries built since then go on to their pipelines.
void PrecompileLoad::Merge() {
    if (auto written = builder.TakeWritten(); !written.empty()) {
        Storage::DataBase::Instance().Flush();
        cache.AddSavedPermutations(written); // keeps what it indexed
        tally.permutations += written.size();
        for (const auto& [hash, perm] : written) {
            built.insert(hash);
        }
    }
    selecting.insert(selecting.end(), unmerged.begin(), unmerged.end());
    unmerged.clear();
}

/// The built entries' pipelines: built, dropped (a selection without one, a failed build) or
/// left (built or building already, not handed over, or still building), which add up to the
/// entries built; the left are split so a scene start that draws them itself shows why (taken by
/// a draw first: its own build came first).
void PrecompileLoad::ReportLoad() const {
    std::string reasons;
    for (const auto& [why, count] : tally.reasons) {
        reasons += (reasons.empty() ? " (" : ", ") + std::to_string(count) + " " + why;
    }
    if (!reasons.empty()) {
        reasons += ")";
    }
    std::printf("Precompile load: %u entries built (%u of them only checked for variants, %llu "
                "permutations), %u skipped, %u rejected%s, %u with a program in use but "
                "untranslated, %u programs' code not verified, %llu pipelines built, %llu "
                "dropped, %llu left (%u taken by a draw first, %zu not handed over, %zu still "
                "building), hold %llu ms, %llu scan passes (last %llu ms), %u holds, variants of "
                "existing programs: %u added, %u already there, %u entries left to the game\n",
                tally.built, tally.checked, static_cast<unsigned long long>(tally.permutations),
                tally.skipped, tally.rejected, reasons.c_str(), tally.no_info, tally.unverified,
                static_cast<unsigned long long>(cache.load_builds_published -
                                                tally.published_before),
                static_cast<unsigned long long>(tally.dropped + cache.load_builds_failed -
                                                tally.failed_before),
                static_cast<unsigned long long>(tally.have + selecting.size() + unmerged.size() +
                                                cache.LoadBuildsRunning()),
                tally.have, selecting.size() + unmerged.size(), cache.LoadBuildsRunning(),
                static_cast<unsigned long long>(tally.held_ns / 1'000'000),
                static_cast<unsigned long long>(scan.Passes() - tally.passes_before),
                static_cast<unsigned long long>(scan.LastPassMs()), tally.holds,
                builder.Variants().added, builder.Variants().present, builder.Variants().left);
    const auto ms = [](u64 ns) { return static_cast<unsigned long long>(ns / 1'000'000); };
    std::printf("Precompile load: probe: load %llu ms; %u pumps built %u entries, %llu ms taking "
                "found code, %llu ms building, %llu ms selecting, longest pump %llu ms; wave of "
                "100+ entries at %llu ms, queue empty at %llu ms, pipelines handed over at %llu "
                "ms (0 = not before the scene)\n",
                ms(CompilePolicy::Now() - tally.began_ns), tally.pumps, tally.pump_built,
                ms(tally.pump_take_ns), ms(tally.pump_build_ns), ms(tally.pump_select_ns),
                ms(tally.pump_max_ns), ms(tally.wave_ns), ms(tally.ready_empty_ns),
                ms(tally.selecting_empty_ns));
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
    const u64 p0 = CompilePolicy::Now();
    const u64 until = p0 + PumpBudgetNs();
    const u32 built_before = tally.built;
    TakeFound();
    ProbeMarks();
    const u64 p1 = CompilePolicy::Now();
    u64 early_ns = 0;
    if (merge_every) {
        // Pipelines of the entries merged so far first, for half the time at most: it stops
        // at once when the workers' queue is full.
        if (const u32 most = pump_limit.value_or(std::numeric_limits<u32>::max());
            tally.pumped < most) {
            tally.pumped += SelectPipelines(p1 + PumpBudgetNs() / 2, most - tally.pumped);
        }
        early_ns = CompilePolicy::Now() - p1;
    }
    BuildReady(until, ready);
    const u64 p2 = CompilePolicy::Now();
    // Pipelines are handed over before a checked entry is built: the checked queue can hold
    // hundreds of entries, and the pipelines of the entries with a needed program must not
    // wait behind all of them. The handover stops when the workers' queue is full, which
    // leaves the checked entries the rest of the time. A selection that adds variants starts
    // none after its first past `until`, but that one translation may overshoot the 4 ms.
    if (const u32 most = pump_limit.value_or(std::numeric_limits<u32>::max());
        tally.pumped < most) {
        tally.pumped += SelectPipelines(until, most - tally.pumped);
    }
    const u64 p3 = CompilePolicy::Now();
    ++tally.pumps;
    tally.pump_take_ns += p1 - p0;
    tally.pump_build_ns += p2 - p1 - early_ns;
    tally.pump_select_ns += p3 - p2 + early_ns;
    switch (NextPumpQueue(!ready.empty(), !checking.empty(), !stale_checking.empty())) {
    case PumpQueue::Checking:
        BuildReady(until, checking);
        break;
    case PumpQueue::Stale:
        BuildReady(until, stale_checking);
        break;
    case PumpQueue::None:
    case PumpQueue::Ready:
        break;
    }
    const u64 p4 = CompilePolicy::Now();
    tally.pump_build_ns += p4 - p3;
    tally.pump_built += tally.built - built_before;
    tally.pump_max_ns = std::max(tally.pump_max_ns, p4 - p0);
    ProbeMarks();
}

/// GetProgram, a program not in program_cache: before the store is asked for it. Only a
/// needed list program holds; without a hold what is built is merged now, so the store is
/// asked for it (else the game translates it as permutation 0 and overwrites the loader's).
void PrecompileLoad::FirstUse(u64 hash) {
    const bool now_loading = cache.DiskShadersActive() && Loading(); // may begin a load first
    const bool list_program = list_programs.contains(hash);
    if (list_program) {
        used.insert(hash);
    }
    if (!now_loading) {
        return;
    }
    TakeFound();
    const bool needed = list_program && cache.ProgramNeeded(hash);
    if (needed && HoldActive(loading, tally.held_ns, !ready.empty(),
                             LastPassFound(found_in, scan.Passes()))) {
        Hold();
    } else {
        Merge(); // nothing when nothing is unmerged
    }
}

/// GetGraphicsPipeline, a pipeline miss: while loading, before the draw builds the pipeline
/// itself, waits for the pipelines of the entries built that are not handed over or still
/// building, as the end of a hold does, which may include this one; then builds checked entries
/// (`checking`) for what CheckedHoldLeftNs leaves and waits for their pipelines too. What is
/// left in `checking` stays for the pump. In this loading state's HoldBudgetNs. True when it
/// held.
bool PrecompileLoad::HoldPipelines() {
    const bool now_loading = cache.DiskShadersActive() && Loading();
    const bool checked_left =
        !checking.empty() && CheckedHoldLeftNs(tally.held_ns, tally.checked_held_ns) > 0;
    if (!PipelineHoldActive(now_loading, tally.held_ns,
                            checked_left || !selecting.empty() || !unmerged.empty() ||
                                cache.LoadBuildsRunning() > 0)) {
        return false;
    }
    const u64 began = CompilePolicy::Now();
    ++tally.holds;
    const u32 built_before = tally.built;
    const size_t not_handed = selecting.size() + unmerged.size();
    const size_t building = cache.LoadBuildsRunning();
    // What is pending first: the checked queue can hold hundreds of entries.
    u32 handed = WaitPipelines(began);
    tally.hold_pipeline_ns += CompilePolicy::Now() - began;
    const u64 left =
        CheckedHoldLeftNs(tally.held_ns + (CompilePolicy::Now() - began), tally.checked_held_ns);
    if (!checking.empty() && left > 0) {
        const u64 checked_began = CompilePolicy::Now();
        const u32 checked_before = tally.built;
        BuildReady(checked_began + left, checking);
        tally.checked_held_ns += CompilePolicy::Now() - checked_began;
        tally.hold_build_ns += CompilePolicy::Now() - checked_began;
        if (tally.built != checked_before) {
            const u64 t = CompilePolicy::Now();
            handed += WaitPipelines(began);
            tally.hold_pipeline_ns += CompilePolicy::Now() - t;
        }
    }
    EndHold(began, built_before, handed, not_handed, building);
    return true;
}

/// GetGraphicsPipeline, a pipeline the loader is building: `wait` for it while loading, from
/// what this loading state's holds left. Not a pending draw, and no grace wait.
bool PrecompileLoad::Wait(const std::function<bool(u64 left_ns)>& wait) {
    const bool now_loading = cache.DiskShadersActive() && Loading();
    const u64 left = HoldLeftNs(now_loading, tally.held_ns);
    if (left == 0) {
        return false;
    }
    const u64 began = CompilePolicy::Now();
    const bool done = wait(left);
    tally.held_ns += CompilePolicy::Now() - began;
    return done;
}

/// GetGraphicsPipeline, a draw that starts its own build of the pipeline `selection` selected:
/// with the trace, its key and the list entries with the same set of programs, what became of
/// them and their key; when there is none, how many entries hold each of its programs. Reads
/// the loading state as last seen: Loading() here would move the load's lines.
void PrecompileLoad::TraceDraw(const PipelineSelection& selection) {
    if (!trace || trace->draw_lines >= MaxTraceDraws) {
        return;
    }
    ++trace->draw_lines;
    const u64 key_hash = std::hash<GraphicsPipelineKey>{}(selection.graphics_key);
    std::vector<std::pair<u32, u64>> stages;
    std::vector<u64> programs;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (selection.infos[i]) {
            stages.emplace_back(i, selection.infos[i]->pgm_hash);
            programs.push_back(selection.infos[i]->pgm_hash);
        }
    }
    std::ranges::sort(programs);
    programs.erase(std::ranges::unique(programs).begin(), programs.end());
    std::ranges::sort(stages, {}, [](const auto& stage) { return StageOrder(stage.first); });
    std::string list_text;
    const auto matched = trace->by_programs.find(programs);
    if (matched != trace->by_programs.end()) {
        list_text = std::to_string(matched->second.size()) + " entries with these programs";
        // Those with this key first, then those with another: a key match must not be cut off.
        auto indices = matched->second;
        std::ranges::stable_sort(indices, {}, [&](u32 index) {
            const auto& found = trace->entries[index].key_hash;
            return found ? (*found == key_hash ? 0 : 1) : 2;
        });
        for (size_t i = 0; i < indices.size() && i < MaxTraceMatches; ++i) {
            const u32 index = indices[i];
            const auto& entry = trace->entries[index];
            list_text += (i == 0 ? ": entry " : ", entry ") + std::to_string(index) + " " +
                         LoadTrace::OutcomeText(entry);
            if (entry.key_hash) {
                list_text += " key " + Hex(*entry.key_hash) +
                             (*entry.key_hash == key_hash ? " same key" : " other key");
            }
        }
    } else {
        list_text = "0 entries with these programs";
        for (const auto& [sw_stage, hash] : stages) {
            const auto held = trace->entries_with.find(hash);
            list_text += std::string{"; "} + StageName(sw_stage) + " in " +
                         std::to_string(held != trace->entries_with.end() ? held->second : 0) +
                         " entries";
        }
    }
    const u64 ended = loading ? 0 : trace->ended_ns;
    std::printf("Precompile load: trace draw built %s, %s; stages %s; list: %s\n",
                Hex(key_hash).c_str(), WhenText(CompilePolicy::Now(), ended).c_str(),
                StagesText(stages).c_str(), list_text.c_str());
}

void Precompile::LoadTraceFlipped(u64 flip_ns, u64 draws) {
    if (!g_load_trace.load(std::memory_order_relaxed)) {
        return;
    }
    const u64 frame_draws =
        draws - g_load_trace_flip_draws.exchange(draws, std::memory_order_relaxed);
    const u32 skipped = g_load_trace_skipped.exchange(0, std::memory_order_relaxed);
    if (skipped == 0 ||
        g_load_trace_frames.fetch_add(1, std::memory_order_relaxed) >= MaxTraceFrames) {
        return;
    }
    std::printf("Precompile load: trace frame: %u of %llu draws skipped, %s\n", skipped,
                static_cast<unsigned long long>(frame_draws),
                WhenText(flip_ns, g_load_trace_ended_ns.load(std::memory_order_relaxed)).c_str());
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

bool PipelineCache::PrecompileLoadHoldPipelines() {
    return precompile_load && precompile_load->HoldPipelines();
}

bool PipelineCache::PrecompileLoadWait(const std::function<bool(u64 left_ns)>& wait) {
    return precompile_load && precompile_load->Wait(wait);
}

void PipelineCache::PrecompileLoadTraceDraw() {
    if (precompile_load) {
        precompile_load->TraceDraw(sel);
    }
}

} // namespace Vulkan
