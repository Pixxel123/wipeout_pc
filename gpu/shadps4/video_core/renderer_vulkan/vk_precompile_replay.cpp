// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_REPLAY: can the selection of a new pipeline be rebuilt from
// recorded guest inputs alone, the way a loading-screen precompile would have to?
//
// Capture: when a draw selects a graphics pipeline key that is new in this launch, copy the
// registers, the draw-indirect parameters and the key, and per stage the program hash, code
// address and user data, plus every guest word the stage's SRT walker and fetch-shader parse
// read, with the pointer each was read through. The walker is JIT code: it runs once more with
// fake pointers into reserved, inaccessible memory, so each of its reads faults into the fault
// handler (vk_precompile_guard.cpp), which reads the guest word, records it and hands back the
// value (or the fake pointer for the next table). The fetch shader and vertex buffer
// descriptors are recorded from the parse the draw just did. Program code is not recorded for
// the same-launch replay: it is still in memory.
//
// Replay, a few submissions later, on the GPU command thread between submissions: the words go
// into a private shadow copy (vk_precompile_shadow.h) with a guard page after each table, the
// recorded pointers in it and in the copied user data lead into the copy, and the selection
// runs lookup-only on a private PipelineSelection (the draw-preparation worker path), so it
// reads the shadow instead of guest memory. A read outside the recorded words faults and is
// counted. The check: the same GraphicsPipelineKey (which holds each stage's permutation).
// Nothing shared is written: program_cache is only read, under programs_mutex.
//
// GOW3_PRECOMPILE_RECORD=<file> writes each capture to a list (vk_precompile_list.h) as it is
// made, for a later launch to replay, after the code of each program it is the first to use.
// With only that set, nothing is replayed in this launch, so the replay queue's cap drops
// nothing. GOW3_PRECOMPILE_LIST=<file> loads such a list in a
// later launch, possibly with another build. When a draw selects a key new in this launch, the
// entries recorded for the same stage programs are replayed the same way, with this launch's
// code addresses and walkers. Permutation numbers differ across launches, so the check is
// whether the replayed key is one the race selects in this launch, before or after the replay.
// An entry whose permutation is not built yet is tried again at the next new key of the same
// programs: when the race selects that key, the permutation exists.
//
// Linux only (the fault handler edits the walker's registers in the signal context). While it
// is on, the page manager's read-fault count also includes the faults of recorded walks.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <cerrno>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <fmt/format.h>

#include "common/hash.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_precompile_guard.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"
#include "video_core/renderer_vulkan/vk_precompile_replay.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan {

namespace {

using namespace Precompile;
using Shader::HwStage;
using Shader::SwStage;

constexpr u32 MaxCaptures = 2000;
constexpr size_t MaxWaiting = 256; ///< captures not replayed yet
constexpr u32 MaxDetails = 8; ///< detail lines per kind
constexpr u64 ReplayDelay = 3; ///< submissions between capture and replay
constexpr auto ReplayBudget = std::chrono::milliseconds(2);
constexpr auto ReportInterval = std::chrono::seconds(5);
constexpr size_t MaxRecordedWords = 4096, MaxRecordedPointers = 256;

static_assert(ListRegsWords == AmdGpu::Regs::NumRegs);
static_assert(ListUserDataWords == AmdGpu::NUM_USER_DATA);
static_assert(ListStages == MaxShaderStages);

struct StageCapture {
    SwStage sw_stage;
    HwStage hw_stage;
    u64 hash;
    VAddr code;
    u64 walker, walker_size; ///< the SRT walker's code
    AmdGpu::UserData user_data;
    std::vector<std::pair<u32, u64>> roots; ///< user-data pointers followed: first sgpr, target
};

/// One new pipeline key: everything its replay may read.
struct KeyCapture {
    std::unique_ptr<AmdGpu::Regs> regs;
    DrawIndirectParams draw_indirect_params;
    GraphicsPipelineKey key;
    u64 key_hash;
    u64 submission;
    std::vector<StageCapture> stages;
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
    std::vector<std::pair<VAddr, u64>> programs; ///< code the selection looks up: address, hash
};

/// Whether a program is still at its address. AmdGpu::GetParams stops the game (UNREACHABLE)
/// on code without its header, which the guest may have replaced since the capture: the same
/// search, without stopping. The caller guards against unmapped code.
bool CodeStillThere(VAddr address, u64 hash) {
    const auto* code = reinterpret_cast<const u32*>(address);
    const auto at = [&](size_t word) {
        return std::bit_cast<const AmdGpu::BinaryInfo*>(code + word);
    };
    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    const AmdGpu::BinaryInfo* found = nullptr;
    if (code[0] == token_mov_vcchi && at((code[1] + 1) * 2)->Valid()) {
        found = at((code[1] + 1) * 2);
    }
    for (size_t word = 0; !found && word < 0x4000; ++word) {
        found = at(word)->Valid() ? at(word) : nullptr;
    }
    return found && found->shader_hash == hash;
}

/// The permutation index behind a key's stage hash (HashCombine(program, index)), or -1.
int PermutationOf(u64 program, u64 stage_hash) {
    for (int perm = 0; perm < 64; ++perm) {
        if (HashCombine(program, u64(perm)) == stage_hash) {
            return perm;
        }
    }
    return -1;
}

/// Notes this thread's faults; reads of the stages' walkers are served (vk_precompile_guard.h).
void WatchStages(std::span<const StageCapture> stages) {
    std::array<ThreadState::Range, MaxShaderStages> walkers{};
    for (size_t i = 0; i < stages.size() && i < walkers.size(); ++i) {
        walkers[i] = {stages[i].walker, stages[i].walker + stages[i].walker_size};
    }
    FaultState().Watch(walkers);
}

void AddRoot(StageCapture& stage, u32 sgpr, u64 target) {
    for (const auto& root : stage.roots) {
        if (root.first == sgpr) {
            return;
        }
    }
    stage.roots.emplace_back(sgpr, target);
}

struct Tally {
    u64 captured, same, different, faulted, not_found, skipped, capture_failed, code_gone;
    u64 walker_changed, words;
    bool operator==(const Tally&) const = default;
};

/// Captures written to the list file, and those left out; programs whose code was written.
struct RecordTally {
    u64 written, bytes, not_written, code_written, code_missing;
    bool operator==(const RecordTally&) const = default;
};

/// A program's code from its start token through its OrbShdr header (vk_precompile_list.h),
/// read without faulting; empty when the header is not where AmdGpu::GetParams looks first.
std::vector<u32> ReadProgramCode(VAddr address) {
    std::array<u32, 2> start;
    if (!ReadGuest(address, start.data(), sizeof(start)) || start[0] != CodeStartToken) {
        return {};
    }
    const u64 header = CodeHeaderAt(start);
    std::array<u32, CodeHeaderWords> info;
    if ((header + CodeHeaderWords) * 4 > MaxCodeBytes ||
        !ReadGuest(address + header * 4, info.data(), sizeof(info))) {
        return {};
    }
    u32 length;
    std::memcpy(&length, reinterpret_cast<const u8*>(info.data()) + CodeHeaderLengthAt,
                sizeof(length));
    const u64 words = std::max<u64>(header + CodeHeaderWords, ((length >> 8) + 3) / 4);
    std::vector<u32> code(words);
    if (words * 4 > MaxCodeBytes || !ReadGuest(address, code.data(), words * 4)) {
        return {};
    }
    return code;
}

enum class Detail { Different, Faulted, NotFound, CaptureFailed, Count };

/// Each stage's program hash (0 for none), by SwStage: what a list entry waits for.
using ProgramSet = std::array<u64, MaxShaderStages>;

/// What became of a list entry in this launch.
enum class ListState {
    NotNew,   ///< no key with its programs was new in this launch (yet)
    Queued,   ///< waiting for its replay
    NotBuilt, ///< a program or permutation the lookup needs is not built yet
    Gone,     ///< the code moved before the replay
    Skipped,  ///< no room for the shadow copy
    Matched,  ///< replayed to a key the race selected
    Unused,   ///< replayed to a key the race has not selected (yet)
    Faulted,  ///< read outside the recorded words
};

/// Tried again at the next new key of the same programs.
bool Retryable(ListState state) {
    return state == ListState::NotNew || state == ListState::NotBuilt ||
           state == ListState::Gone || state == ListState::Skipped;
}

/// A key recorded by an earlier launch and what became of it in this one.
struct ListEntry {
    RecordedKey key;
    ProgramSet programs;
    ListState state = ListState::NotNew;
    u64 replayed = 0; ///< hash of the key the replay selected
};

/// An entry to replay with the code addresses of the draw that asked for it.
struct ListItem {
    u32 entry;
    u64 submission;
    std::array<VAddr, MaxShaderStages> code; ///< by SwStage
};

struct ListTally {
    u64 new_keys; ///< keys the race selected in this launch
    u64 covered;  ///< of those, keys a replayed entry landed on
    u64 matched, unused, not_built, faulted, gone, skipped, waiting;
    u64 never_seen; ///< NotNew, a program never seen in this launch
    u64 not_new;    ///< NotNew, every program seen
    bool operator==(const ListTally&) const = default;
};

/// Detail lines of the list replay. Unused and NotBuilt are printed at exit: most entries pass
/// through them on the way to a match.
enum class ListNote { Rejected, Faulted, Unused, NotBuilt, Count };

AmdGpu::ShaderProgram& ProgramAt(AmdGpu::Regs& regs, u32 hw_stage) {
    return const_cast<AmdGpu::ShaderProgram&>(*regs.ProgramForStage(hw_stage));
}

/// What a replayed selection came to.
enum class Outcome { Selected, NotFound, Faulted, CodeGone, WalkerChanged, NoRoom };

struct ReplayResult {
    Outcome outcome;
    GraphicsPipelineKey key; ///< the selected key
    u32 faults;
    u64 fault;           ///< the first fault's address
    size_t shadow_bytes; ///< of the shadow copy
};

} // namespace

struct PrecompileReplay {
    PrecompileReplay(bool replay_same_launch_, std::FILE* record_file_)
        : replay_same_launch{replay_same_launch_}, record_file{record_file_},
          slots{g_reserved.record, RecordSlotSize, NumRecordSlots} {}
    ~PrecompileReplay() {
        if (record_file) {
            std::fclose(record_file);
        }
    }

    void Capture(PipelineCache& cache);
    void Replay(PipelineCache& cache);
    void Report(bool final);
    void UseList(ParsedList&& parsed);
    void NoteNewKey(PipelineCache& cache);

    [[nodiscard]] bool Capturing() const {
        return replay_same_launch || record_file;
    }
    [[nodiscard]] bool Listing() const {
        return list_on;
    }

    u64 submissions = 0;
    Tally tally{}, reported{};

private:
    void Record(const KeyCapture& c);
    void RecordCode(const StageCapture& stage);
    bool WriteRecord(const std::vector<u8>& bytes);
    const char* RecordStage(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordWalker(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordFetch(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    void ReplayOne(PipelineCache& cache, KeyCapture& c);
    ReplayResult Select(PipelineCache& cache, KeyCapture& c);
    std::optional<Outcome> Stale(PipelineCache& cache, const KeyCapture& c);
    std::string Difference(const KeyCapture& c, const GraphicsPipelineKey& key) const;
    void Log(Detail kind, const std::string& text);
    void Note(ListNote kind, const std::string& text);
    void ReplayListed(PipelineCache& cache, const ListItem& item);
    KeyCapture Expand(const RecordedKey& key, const std::array<VAddr, MaxShaderStages>& code);
    void AdoptWalkers(PipelineCache& cache, KeyCapture& c);
    ListTally CountList();
    void ReportOpenEntries();

    const bool replay_same_launch; ///< GOW3_PRECOMPILE_REPLAY=1
    std::FILE* record_file;        ///< GOW3_PRECOMPILE_RECORD, null when off or failed
    RecordTally record{}, record_reported{};
    bool record_capped_logged = false;
    std::unordered_set<u64> code_seen; ///< programs whose code was written or not found
    RecordSlots slots;
    ShadowArena arena;
    std::unordered_set<u64> seen;          ///< key hashes already captured or skipped
    std::deque<KeyCapture> waiting;
    std::array<u32, size_t(Detail::Count)> details{};
    std::chrono::steady_clock::time_point last_report{};
    bool cap_logged = false, arena_logged = false, read_logged = false;

    // GOW3_PRECOMPILE_LIST: a list recorded by an earlier launch.
    bool list_on = false;
    std::vector<ListEntry> list;
    std::map<ProgramSet, std::vector<u32>> list_by_programs; ///< entry indices
    u32 list_rejected = 0;
    std::deque<ListItem> list_waiting;
    std::unordered_set<GraphicsPipelineKey> live_keys; ///< every key the race selected
    /// Keys replayed to but not selected yet, with their entries.
    std::unordered_map<GraphicsPipelineKey, std::vector<u32>> unused_keys;
    /// Keys the race selected that at least one replayed entry landed on: what a build from
    /// the list would have had ready (several entries can land on one key).
    std::unordered_set<GraphicsPipelineKey> covered;
    std::unordered_set<u64> live_programs;
    std::array<u32, size_t(ListNote::Count)> list_notes{};
    ListTally list_reported{};
    std::mutex list_mutex; ///< the states and live sets: the exit report runs on another thread
};

namespace {
PrecompileReplay* g_active; ///< reported at exit

void ReportAtExit() {
    if (g_active) {
        g_active->Report(true);
    }
}
} // namespace

void PrecompileReplay::Log(Detail kind, const std::string& text) {
    if (details[size_t(kind)]++ < MaxDetails) {
        std::printf("Precompile replay: %s\n", text.c_str());
    }
}

void PrecompileReplay::Note(ListNote kind, const std::string& text) {
    if (list_notes[size_t(kind)]++ < MaxDetails) {
        std::printf("Precompile list: %s\n", text.c_str());
    }
}

void PrecompileReplay::Report(bool final) {
    const auto now = std::chrono::steady_clock::now();
    if (!final && now - last_report < ReportInterval) {
        return;
    }
    last_report = now;
    if (record_file && (final || !(record == record_reported))) {
        record_reported = record;
        std::printf("Precompile record: %llu written (%llu KiB), %llu not written "
                    "(geometry/size/write), code of %llu programs, %llu without code%s\n",
                    static_cast<unsigned long long>(record.written),
                    static_cast<unsigned long long>(record.bytes >> 10),
                    static_cast<unsigned long long>(record.not_written),
                    static_cast<unsigned long long>(record.code_written),
                    static_cast<unsigned long long>(record.code_missing), final ? " (exit)" : "");
    }
    if (list_on) {
        const auto t = CountList();
        if (final || !(t == list_reported)) {
            list_reported = t;
            std::printf("%s\n",
                        fmt::format("Precompile list: {} entries, {} keys new in this launch ({} reached by a "
                                    "replay), "
                                    "{} matched a key the race used, {} replayed to a key not "
                                    "used{}, {} permutation not built, {} faulted (outside "
                                    "shadow), {} program never seen, {} programs never new "
                                    "together, {} code gone, {} skipped (no room), {} waiting, "
                                    "{} rejected{}",
                                    list.size(), t.new_keys, t.covered, t.matched, t.unused,
                                    final ? "" : " yet", t.not_built, t.faulted, t.never_seen,
                                    t.not_new, t.gone, t.skipped, t.waiting, list_rejected,
                                    final ? " (exit)" : "")
                            .c_str());
        }
        if (final) {
            ReportOpenEntries();
        }
    }
    if (!Capturing() || (!final && tally == reported)) {
        return;
    }
    reported = tally;
    const auto& t = tally;
    std::printf("%s\n",
                fmt::format("Precompile replay: {} captured, {} same key, {} different key, {} "
                            "faulted (outside shadow), {} not found, {} skipped "
                            "(cap/queue/hs/motion), {} capture failed, {} code gone, {} walker "
                            "changed, {} waiting, {} guest words{}",
                            t.captured, t.same, t.different, t.faulted, t.not_found, t.skipped,
                            t.capture_failed, t.code_gone, t.walker_changed, waiting.size(),
                            t.words, final ? " (exit)" : "")
                    .c_str());
}

void PrecompileReplay::Capture(PipelineCache& cache) {
    const auto& sel = cache.sel;
    const u64 key_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    if (!seen.insert(key_hash).second) {
        return;
    }
    // Tessellation reads constants at selection time (not prepared either); the motion choice
    // reads a global that may have moved on by the replay.
    if (sel.regs->stage_enable.hs_en || sel.graphics_key.motion_vectors ||
        (replay_same_launch && waiting.size() >= MaxWaiting)) {
        ++tally.skipped;
        return;
    }
    if (tally.captured + tally.capture_failed >= MaxCaptures) {
        ++tally.skipped;
        if (!cap_logged) {
            cap_logged = true;
            std::printf("Precompile replay: %u keys captured, later ones skipped\n", MaxCaptures);
        }
        return;
    }
    KeyCapture c{};
    c.regs = std::make_unique<AmdGpu::Regs>(*sel.regs);
    c.draw_indirect_params = sel.draw_indirect_params;
    c.key = sel.graphics_key;
    c.key_hash = key_hash;
    c.submission = submissions;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        const auto* info = sel.infos[i];
        if (!info) {
            continue;
        }
        auto& stage = c.stages.emplace_back();
        stage.sw_stage = SwStage(i);
        stage.hw_stage = info->hw_stage;
        stage.hash = info->pgm_hash;
        stage.code = sel.regs->ProgramForStage(u32(info->hw_stage))->Address<VAddr>();
        stage.walker = u64(info->srt_info.walker_func);
        stage.walker_size = info->srt_info.walker_func_size;
        if (const char* error = RecordStage(*info, stage, c)) {
            ++tally.capture_failed;
            Log(Detail::CaptureFailed, fmt::format("capture of key {:016x} failed, stage {} "
                                                   "{:016x}: {}",
                                                   key_hash, i, stage.hash, error));
            if (FirstReadError() && !read_logged) {
                read_logged = true;
                std::printf("Precompile replay: first failed guest read: %s\n",
                            std::strerror(FirstReadError()));
            }
            return;
        }
    }
    for (const auto& stage : c.stages) {
        c.programs.emplace_back(stage.code, stage.hash);
        if (stage.hw_stage == HwStage::Geometry) { // BuildRuntimeInfo reads the copy shader
            const auto& copy = sel.regs->vs_program;
            c.programs.emplace_back(copy.Address<VAddr>(), AmdGpu::GetParams(copy).hash);
        }
    }
    ++tally.captured;
    tally.words += c.words.size();
    if (record_file) {
        Record(c);
    }
    if (replay_same_launch) {
        waiting.push_back(std::move(c));
    }
}

namespace {
/// Index of the stage-enable word in Regs::reg_array.
u32 StageEnableWord() {
    const auto regs = std::make_unique<AmdGpu::Regs>();
    return u32((reinterpret_cast<const u8*>(&regs->stage_enable) -
                reinterpret_cast<const u8*>(regs->reg_array.data())) /
               sizeof(u32));
}
} // namespace

/// Keeps the entries the replay can use, indexed by their programs.
void PrecompileReplay::UseList(ParsedList&& parsed) {
    list_on = true;
    list_rejected = parsed.rejected;
    for (const auto& reason : parsed.reasons) {
        Note(ListNote::Rejected, "rejected " + reason);
    }
    const u32 stage_enable = StageEnableWord();
    const auto vs_or_ps = [](const RecordedStage& stage) {
        return (stage.sw_stage == u32(SwStage::Fragment) &&
                stage.hw_stage == u32(HwStage::Fragment)) ||
               (stage.sw_stage == u32(SwStage::Vertex) && stage.hw_stage == u32(HwStage::Vertex));
    };
    for (auto& key : parsed.keys) {
        // Only what RECORD writes: vertex + fragment keys (no geometry or tessellation).
        const bool vs_only = std::ranges::none_of(key.regs, [&](const auto& reg) {
            return reg.first == stage_enable && reg.second != AmdGpu::ShaderStageEnable::Vs;
        });
        if (!vs_only || !std::ranges::all_of(key.stages, vs_or_ps)) {
            ++list_rejected;
            Note(ListNote::Rejected,
                 fmt::format("rejected key {:016x}: not a vertex + fragment key", key.key_hash));
            continue;
        }
        ListEntry entry{std::move(key), {}};
        for (const auto& stage : entry.key.stages) {
            entry.programs[stage.sw_stage] = stage.hash;
        }
        list_by_programs[entry.programs].push_back(u32(list.size()));
        list.push_back(std::move(entry));
    }
}

/// Appends a capture to the list file, after the code of programs it uses first.
void PrecompileReplay::Record(const KeyCapture& c) {
    // A later launch replays vertex + fragment keys only: a geometry key's selection also reads
    // the copy shader through vs_program, which is not among the recorded stages.
    if (c.regs->stage_enable.raw != AmdGpu::ShaderStageEnable::Vs) {
        ++record.not_written;
        return;
    }
    RecordedKey key{};
    key.key_hash = c.key_hash;
    key.vertex_sgpr_offset = c.draw_indirect_params.vertex_sgpr_offset;
    key.instance_sgpr_offset = c.draw_indirect_params.instance_sgpr_offset;
    for (u32 i = 0; i < AmdGpu::Regs::NumRegs; ++i) {
        if (const u32 value = c.regs->reg_array[i]) {
            key.regs.emplace_back(i, value);
        }
    }
    for (const auto& stage : c.stages) {
        key.stages.push_back({u32(stage.sw_stage), u32(stage.hw_stage), stage.hash,
                              stage.user_data, stage.roots});
    }
    key.words = c.words;
    key.pointers = c.pointers;
    for (const auto& stage : c.stages) {
        RecordCode(stage);
    }
    if (record_file && WriteRecord(EncodeListEntry(key))) {
        ++record.written;
    } else {
        ++record.not_written;
    }
}

/// Writes a program's code before the first entry that uses it, once per program.
void PrecompileReplay::RecordCode(const StageCapture& stage) {
    if (!record_file || !code_seen.insert(stage.hash).second) {
        return;
    }
    const auto code = ReadProgramCode(stage.code);
    if (code.empty() || CheckCode(stage.hash, code) ||
        !WriteRecord(EncodeListCode(stage.hash, code))) {
        ++record.code_missing;
        return;
    }
    ++record.code_written;
}

/// Appends one entry to the list file, flushed at once: the game may end without exiting.
bool PrecompileReplay::WriteRecord(const std::vector<u8>& bytes) {
    if (bytes.size() > MaxListEntryBytes || record.bytes + bytes.size() > MaxListBytes) {
        if (!record_capped_logged) {
            record_capped_logged = true;
            std::printf("Precompile record: list full (%zu MiB), later captures not written\n",
                        MaxListBytes >> 20);
        }
        return false;
    }
    if (std::fwrite(bytes.data(), 1, bytes.size(), record_file) != bytes.size() ||
        std::fflush(record_file) != 0) {
        std::printf("Precompile record: write failed (%s), recording stopped\n",
                    std::strerror(errno));
        std::fclose(record_file);
        record_file = nullptr;
        return false;
    }
    record.bytes += bytes.size();
    return true;
}

const char* PrecompileReplay::RecordStage(const Shader::Info& info, StageCapture& stage,
                                          KeyCapture& c) {
    if (info.user_data.size() != stage.user_data.size()) {
        return "unexpected user data size";
    }
    std::copy(info.user_data.begin(), info.user_data.end(), stage.user_data.begin());
    stage.roots.reserve(stage.user_data.size());
    const char* error = nullptr;
    const bool completed = Guarded([&] {
        error = RecordWalker(info, stage, c);
        if (!error) {
            error = RecordFetch(info, stage, c);
        }
    });
    FaultState().Stop();
    return completed ? error : "fault while recording";
}

const char* PrecompileReplay::RecordWalker(const Shader::Info& info, StageCapture& stage,
                                           KeyCapture& c) {
    if (!info.srt_info.walker_func) {
        return nullptr;
    }
    std::vector<u32> flat(info.flattened_ud_buf.size());
    if (flat.size() < stage.user_data.size()) {
        return "flattened user data smaller than the user data";
    }
    std::copy(stage.user_data.begin(), stage.user_data.end(), flat.begin());
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
    words.reserve(MaxRecordedWords);
    pointers.reserve(MaxRecordedPointers);
    slots.Reset();
    WalkRecorder recorder{slots, stage.user_data, ReadGuest, words, pointers, stage.roots};
    auto& state = FaultState();
    WatchStages({&stage, 1});
    state.recorder = &recorder;
    info.srt_info.walker_func(reinterpret_cast<const u32*>(slots.Address(0)), flat.data());
    const u32 faults = state.faults;
    state.Stop();
    if (recorder.error) {
        return recorder.error;
    }
    if (faults != 0) {
        return "a read outside the record slots faulted";
    }
    // The recorded walk must give exactly what the draw's own walk gave.
    if (flat != info.flattened_ud_buf) {
        return "recorded walk differs from the draw's";
    }
    c.words.insert(c.words.end(), words.begin(), words.end());
    c.pointers.insert(c.pointers.end(), pointers.begin(), pointers.end());
    return nullptr;
}

const char* PrecompileReplay::RecordFetch(const Shader::Info& info, StageCapture& stage,
                                          KeyCapture& c) {
    if (!info.has_fetch_shader) {
        return nullptr;
    }
    // The draw just parsed it on this thread: the parse cache answers.
    const auto fetch = Shader::Gcn::ParseFetchShader(info);
    if (!fetch) {
        return nullptr;
    }
    const auto pointer_at = [&](u32 sgpr) -> std::optional<u64> {
        if (sgpr + 1 >= stage.user_data.size()) {
            return std::nullopt;
        }
        u64 pointer;
        std::memcpy(&pointer, &stage.user_data[sgpr], sizeof(pointer));
        return pointer & GuestAddressMask;
    };
    const auto code = pointer_at(info.fetch_shader_sgpr_base);
    if (!code) {
        return "fetch shader pointer out of range";
    }
    AddRoot(stage, info.fetch_shader_sgpr_base, *code);
    const auto* code_words = reinterpret_cast<const u32*>(*code);
    for (u32 i = 0; i < fetch->size / sizeof(u32); ++i) {
        c.words.push_back({*code, i * u32(sizeof(u32)), code_words[i]});
    }
    for (const auto& attrib : fetch->attributes) {
        const auto table = pointer_at(attrib.sgpr_base);
        if (!table) {
            return "vertex buffer table pointer out of range";
        }
        AddRoot(stage, attrib.sgpr_base, *table);
        const auto* sharp = reinterpret_cast<const u32*>(*table) + attrib.dword_offset;
        for (u32 w = 0; w < sizeof(AmdGpu::Buffer) / sizeof(u32); ++w) {
            c.words.push_back({*table, (attrib.dword_offset + w) * u32(sizeof(u32)), sharp[w]});
        }
    }
    return nullptr;
}

void PrecompileReplay::Replay(PipelineCache& cache) {
    ++submissions;
    const auto deadline = std::chrono::steady_clock::now() + ReplayBudget;
    while (!waiting.empty() && waiting.front().submission + ReplayDelay <= submissions &&
           std::chrono::steady_clock::now() < deadline) {
        KeyCapture c = std::move(waiting.front());
        waiting.pop_front();
        ReplayOne(cache, c);
    }
    while (!list_waiting.empty() && list_waiting.front().submission + ReplayDelay <= submissions &&
           std::chrono::steady_clock::now() < deadline) {
        const ListItem item = list_waiting.front();
        list_waiting.pop_front();
        ReplayListed(cache, item);
    }
    Report(false);
}

/// A draw selected a key new in this launch: it may match an entry replayed earlier, and the
/// entries recorded for the same programs are queued with this draw's code addresses.
void PrecompileReplay::NoteNewKey(PipelineCache& cache) {
    const auto& sel = cache.sel;
    std::scoped_lock lock{list_mutex};
    if (!live_keys.insert(sel.graphics_key).second) {
        return; // asked for again while it builds
    }
    if (const auto found = unused_keys.find(sel.graphics_key); found != unused_keys.end()) {
        for (const u32 index : found->second) {
            list[index].state = ListState::Matched;
        }
        covered.insert(sel.graphics_key);
        unused_keys.erase(found);
    }
    ProgramSet programs{};
    std::array<VAddr, MaxShaderStages> code{};
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (const auto* info = sel.infos[i]) {
            programs[i] = info->pgm_hash;
            code[i] = sel.regs->ProgramForStage(u32(info->hw_stage))->Address<VAddr>();
            live_programs.insert(info->pgm_hash);
        }
    }
    const auto found = list_by_programs.find(programs);
    if (found == list_by_programs.end() ||
        sel.regs->stage_enable.raw != AmdGpu::ShaderStageEnable::Vs) {
        return;
    }
    for (const u32 index : found->second) {
        auto& entry = list[index];
        // No queue cap: an entry is queued once at a time, and the items are small.
        if (!Retryable(entry.state)) {
            continue;
        }
        entry.state = ListState::Queued;
        list_waiting.push_back({index, submissions, code});
    }
}

/// The recorded registers with only this launch's code addresses of the recorded stages: the
/// earlier launch's addresses would reach AmdGpu::GetParams, which stops the game on code
/// without its header.
KeyCapture PrecompileReplay::Expand(const RecordedKey& key,
                                    const std::array<VAddr, MaxShaderStages>& code) {
    KeyCapture c{};
    c.regs = std::make_unique<AmdGpu::Regs>();
    c.regs->reg_array.fill(0);
    for (const auto& [index, value] : key.regs) {
        c.regs->reg_array[index] = value;
    }
    for (u32 hw_stage = 0; hw_stage < ListHwStages; ++hw_stage) {
        ProgramAt(*c.regs, hw_stage).address = 0;
    }
    c.draw_indirect_params = {u16(key.vertex_sgpr_offset), key.instance_sgpr_offset};
    c.key_hash = key.key_hash;
    c.submission = submissions;
    for (const auto& recorded : key.stages) {
        auto& stage = c.stages.emplace_back();
        stage.sw_stage = SwStage(recorded.sw_stage);
        stage.hw_stage = HwStage(recorded.hw_stage);
        stage.hash = recorded.hash;
        stage.code = code[recorded.sw_stage];
        stage.user_data = recorded.user_data;
        stage.roots = recorded.roots;
        ProgramAt(*c.regs, recorded.hw_stage).address = stage.code >> 8;
        c.programs.emplace_back(stage.code, stage.hash);
    }
    c.words = key.words;
    c.pointers = key.pointers;
    return c;
}

/// The walkers are this launch's JIT code: the replay watches the ones the lookup will run.
void PrecompileReplay::AdoptWalkers(PipelineCache& cache, KeyCapture& c) {
    std::shared_lock lock{cache.programs_mutex};
    for (auto& stage : c.stages) {
        const auto found = cache.program_cache.find(stage.hash);
        const Shader::Info* info =
            found != cache.program_cache.end() ? found->second->info_template.get() : nullptr;
        stage.walker = info ? u64(info->srt_info.walker_func) : 0;
        stage.walker_size = info ? info->srt_info.walker_func_size : 0;
    }
}

void PrecompileReplay::ReplayListed(PipelineCache& cache, const ListItem& item) {
    auto& entry = list[item.entry];
    KeyCapture c = Expand(entry.key, item.code);
    AdoptWalkers(cache, c);
    const auto result = Select(cache, c);
    std::scoped_lock lock{list_mutex};
    switch (result.outcome) {
    case Outcome::NoRoom:
        entry.state = ListState::Skipped;
        if (!arena_logged) {
            arena_logged = true;
            std::printf("Precompile list: no room for the shadow copy of recorded key %016llx "
                        "(%zu bytes), skipped\n",
                        static_cast<unsigned long long>(c.key_hash), result.shadow_bytes);
        }
        break;
    case Outcome::CodeGone:
    case Outcome::WalkerChanged:
        entry.state = ListState::Gone;
        break;
    case Outcome::Faulted:
        entry.state = ListState::Faulted;
        Note(ListNote::Faulted, fmt::format("recorded key {:016x}: read outside the recorded words "
                                            "at {:#x} ({}), {} fault(s)",
                                            c.key_hash, result.fault, FaultPlace(result.fault),
                                            result.faults));
        break;
    case Outcome::NotFound:
        entry.state = ListState::NotBuilt;
        break;
    case Outcome::Selected:
        entry.replayed = std::hash<GraphicsPipelineKey>{}(result.key);
        if (live_keys.contains(result.key)) {
            entry.state = ListState::Matched;
            covered.insert(result.key);
        } else {
            entry.state = ListState::Unused;
            unused_keys[result.key].push_back(item.entry);
        }
        break;
    }
}

/// At exit: the entries left replayed to a key the race never selected, or never built.
void PrecompileReplay::ReportOpenEntries() {
    std::scoped_lock lock{list_mutex};
    for (const auto& entry : list) {
        std::string programs;
        for (const auto& stage : entry.key.stages) {
            programs += fmt::format(" stage {} {:016x}", stage.sw_stage, stage.hash);
        }
        if (entry.state == ListState::Unused) {
            Note(ListNote::Unused, fmt::format("recorded key {:016x} replayed as {:016x}, never "
                                               "selected:{}",
                                               entry.key.key_hash, entry.replayed, programs));
        } else if (entry.state == ListState::NotBuilt) {
            Note(ListNote::NotBuilt, fmt::format("recorded key {:016x}: program or permutation "
                                                 "never built:{}",
                                                 entry.key.key_hash, programs));
        }
    }
}

ListTally PrecompileReplay::CountList() {
    std::scoped_lock lock{list_mutex};
    ListTally t{};
    t.new_keys = live_keys.size();
    t.covered = covered.size();
    for (const auto& entry : list) {
        switch (entry.state) {
        case ListState::NotNew:
            if (std::ranges::all_of(entry.programs, [&](u64 hash) {
                    return !hash || live_programs.contains(hash);
                })) {
                ++t.not_new;
            } else {
                ++t.never_seen;
            }
            break;
        case ListState::Queued:
            ++t.waiting;
            break;
        case ListState::NotBuilt:
            ++t.not_built;
            break;
        case ListState::Gone:
            ++t.gone;
            break;
        case ListState::Skipped:
            ++t.skipped;
            break;
        case ListState::Matched:
            ++t.matched;
            break;
        case ListState::Unused:
            ++t.unused;
            break;
        case ListState::Faulted:
            ++t.faulted;
            break;
        }
    }
    return t;
}

/// Why the programs are no longer at their addresses with the walkers the capture recorded,
/// none when they are. The lookup copies the program's info_template: a walker the replay does
/// not watch could fault into the SRT walker patch, which would rewrite it for the game.
std::optional<Outcome> PrecompileReplay::Stale(PipelineCache& cache, const KeyCapture& c) {
    if (!std::ranges::all_of(c.programs, [](const auto& program) {
            return CodeStillThere(program.first, program.second);
        })) {
        return Outcome::CodeGone;
    }
    std::shared_lock lock{cache.programs_mutex};
    for (const auto& stage : c.stages) {
        const auto found = cache.program_cache.find(stage.hash);
        if (found != cache.program_cache.end() && found->second->info_template &&
            u64(found->second->info_template->srt_info.walker_func) != stage.walker) {
            return Outcome::WalkerChanged;
        }
    }
    return std::nullopt;
}

void PrecompileReplay::ReplayOne(PipelineCache& cache, KeyCapture& c) {
    const auto result = Select(cache, c);
    switch (result.outcome) {
    case Outcome::NoRoom:
        ++tally.skipped;
        if (!arena_logged) {
            arena_logged = true;
            std::printf("Precompile replay: no room for the shadow copy of key %016llx (%zu "
                        "bytes), skipped\n",
                        static_cast<unsigned long long>(c.key_hash), result.shadow_bytes);
        }
        break;
    case Outcome::CodeGone:
        ++tally.code_gone;
        break;
    case Outcome::WalkerChanged:
        ++tally.walker_changed;
        break;
    case Outcome::Faulted:
        ++tally.faulted;
        Log(Detail::Faulted, fmt::format("key {:016x}: read outside the recorded words at {:#x} "
                                         "({}), {} fault(s)",
                                         c.key_hash, result.fault, FaultPlace(result.fault),
                                         result.faults));
        break;
    case Outcome::NotFound:
        ++tally.not_found;
        Log(Detail::NotFound, fmt::format("key {:016x}: program or permutation not found",
                                          c.key_hash));
        break;
    case Outcome::Selected:
        if (result.key == c.key) {
            ++tally.same;
        } else {
            ++tally.different;
            Log(Detail::Different,
                fmt::format("key {:016x} replayed as {:016x}: {}", c.key_hash,
                            std::hash<GraphicsPipelineKey>{}(result.key),
                            Difference(c, result.key)));
        }
        break;
    }
}

/// Runs the selection lookup-only from the capture alone, through a shadow copy of its words.
ReplayResult PrecompileReplay::Select(PipelineCache& cache, KeyCapture& c) {
    ReplayResult result{};
    const ShadowLayout layout{c.words, PageSize};
    result.shadow_bytes = layout.Size();
    const auto address = arena.Map(layout);
    if (!address) {
        result.outcome = Outcome::NoRoom;
        return result;
    }
    const u64 dead = g_reserved.shadow;
    layout.Fill(reinterpret_cast<u8*>(*address), *address, c.words, c.pointers, dead);
    // The copied registers get the recorded user data, its pointers led into the shadow.
    for (const auto& stage : c.stages) {
        auto& program = const_cast<AmdGpu::ShaderProgram&>(
            *c.regs->ProgramForStage(u32(stage.hw_stage)));
        program.user_data = stage.user_data;
        for (const auto& [sgpr, target] : stage.roots) {
            u64 pointer;
            std::memcpy(&pointer, &program.user_data[sgpr], sizeof(pointer));
            pointer = Repoint(pointer, layout.Translate(target, *address).value_or(dead));
            std::memcpy(&program.user_data[sgpr], &pointer, sizeof(pointer));
        }
    }

    // The check: a lookup-only selection on a private PipelineSelection.
    PrepWorker worker{};
    PipelineSelection selection{};
    selection.regs = c.regs.get();
    selection.worker = &worker;
    selection.draw_indirect_params = c.draw_indirect_params;
    bool checked = false, selected = false;
    std::optional<Outcome> stale;
    WatchStages(c.stages);
    const bool completed = Guarded([&] {
        // Checked right before the lookup, so the code cannot change in between.
        stale = Stale(cache, c);
        checked = true;
        if (!stale) {
            selected = cache.RefreshGraphicsKey(selection);
        }
    });
    auto& state = FaultState();
    result.faults = state.faults;
    result.fault = state.fault;
    state.Stop();
    arena.Unmap(*address, layout);

    if (!checked) {
        result.outcome = Outcome::CodeGone; // a fault in the check: the code is unmapped
    } else if (stale) {
        result.outcome = *stale;
    } else if (!completed || result.faults != 0) {
        result.outcome = Outcome::Faulted;
    } else if (!selected) {
        result.outcome = Outcome::NotFound;
    } else {
        result.outcome = Outcome::Selected;
        result.key = selection.graphics_key;
    }
    return result;
}

std::string PrecompileReplay::Difference(const KeyCapture& c,
                                         const GraphicsPipelineKey& key) const {
    for (const auto& stage : c.stages) {
        const u32 i = u32(stage.sw_stage);
        if (key.stage_hashes[i] != c.key.stage_hashes[i]) {
            return fmt::format("stage {} ({:016x}) permutation {} -> {}", i, stage.hash,
                               PermutationOf(stage.hash, c.key.stage_hashes[i]),
                               PermutationOf(stage.hash, key.stage_hashes[i]));
        }
    }
    const auto* a = reinterpret_cast<const u8*>(&c.key);
    const auto* b = reinterpret_cast<const u8*>(&key);
    size_t byte = 0;
    while (byte < sizeof(key) && a[byte] == b[byte]) {
        ++byte;
    }
    return fmt::format("same stages, key byte {} differs", byte);
}

void PrecompileReplayDeleter::operator()(PrecompileReplay* replay) const {
    if (g_active == replay) {
        g_active = nullptr;
    }
    delete replay;
}

namespace Precompile {

std::optional<ParsedList> LoadListFile(const char* path, u32 max_keys) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::printf("Precompile list: cannot read %s (%s), off\n", path, std::strerror(errno));
        return std::nullopt;
    }
    std::vector<u8> bytes;
    u8 chunk[1 << 16];
    for (size_t read; (read = std::fread(chunk, 1, sizeof(chunk), file)) != 0;) {
        if (bytes.size() + read > MaxListBytes + EncodeListHeader().size()) {
            std::fclose(file);
            std::printf("Precompile list: %s is larger than %zu MiB, rejected\n", path,
                        MaxListBytes >> 20);
            return std::nullopt;
        }
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    std::fclose(file);
    auto parsed = ParseList(bytes, max_keys);
    switch (parsed.status) {
    case ParsedList::Status::Ok:
        std::printf("Precompile list: %s: %zu entries, %u rejected\n", path, parsed.keys.size(),
                    parsed.rejected);
        return parsed;
    case ParsedList::Status::NotAList:
        std::printf("Precompile list: %s is not a precompile list, rejected\n", path);
        break;
    case ParsedList::Status::OtherVersion:
        std::printf("Precompile list: %s has version %u, this build reads %u, rejected\n", path,
                    parsed.version, ListVersion);
        break;
    case ParsedList::Status::OtherLayout:
        std::printf("Precompile list: %s has another register layout, rejected\n", path);
        break;
    }
    return std::nullopt;
}

} // namespace Precompile

namespace {
/// The value of an environment switch that names a file; null when unset or empty.
const char* PathSetting(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : nullptr;
}

/// A new list file with its header, or null (logged).
std::FILE* OpenRecord(const char* path) {
    std::FILE* file = std::fopen(path, "wb");
    const auto header = EncodeListHeader();
    if (!file || std::fwrite(header.data(), 1, header.size(), file) != header.size() ||
        std::fflush(file) != 0) {
        std::printf("Precompile record: cannot write %s (%s), off\n", path, std::strerror(errno));
        if (file) {
            std::fclose(file);
        }
        return nullptr;
    }
    std::printf("Precompile record: writing %s\n", path);
    return file;
}
} // namespace

PrecompileReplayPtr MakePrecompileReplay() {
    const char* env = std::getenv("GOW3_PRECOMPILE_REPLAY");
    const bool replay = env && env[0] == '1';
    const char* record_path = PathSetting("GOW3_PRECOMPILE_RECORD");
    const char* list_path = PathSetting("GOW3_PRECOMPILE_LIST");
    if (!replay && !record_path && !list_path) {
        return nullptr;
    }
#ifndef __linux__
    std::printf("Precompile replay: not supported on this platform\n");
    return nullptr;
#else
    // Loaded before the record file is opened, which may be the same file.
    auto parsed = list_path ? LoadListFile(list_path, MaxCaptures) : std::nullopt;
    std::FILE* record_file = record_path ? OpenRecord(record_path) : nullptr;
    if (!replay && !record_file && !parsed) {
        return nullptr;
    }
    if (!SetUpFaultGuard()) {
        if (record_file) {
            std::fclose(record_file);
        }
        return nullptr;
    }
    static const bool report_at_exit = std::atexit(ReportAtExit) == 0;
    (void)report_at_exit;
    auto* precompile = new PrecompileReplay(replay, record_file);
    if (parsed) {
        precompile->UseList(std::move(*parsed));
    }
    g_active = precompile;
    if (replay) {
        std::printf("Precompile replay: on\n");
    }
    return PrecompileReplayPtr{precompile};
#endif
}

void PipelineCache::CapturePrecompile() {
    if (precompile->Capturing()) {
        precompile->Capture(*this);
    }
    if (precompile->Listing()) {
        precompile->NoteNewKey(*this);
    }
}

void PipelineCache::ReplayPrecompile() {
    if (precompile) {
        precompile->Replay(*this);
    }
}

} // namespace Vulkan
