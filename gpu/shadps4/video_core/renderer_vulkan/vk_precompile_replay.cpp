// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_REPLAY: can the selection of a new pipeline be rebuilt from
// recorded guest inputs alone, the way a loading-screen precompile would have to?
//
// Capture: when a draw selects a graphics pipeline key that is new in this launch, copy the
// registers, the draw-indirect parameters and the key, and per stage the program hash, code
// address and user data, plus every guest word the stage's SRT walker and fetch-shader parse
// read, with the pointer each was read through. The walker is JIT code: it runs once more with
// fake pointers into reserved, inaccessible memory, so each of its reads faults into OnFault,
// which reads the guest word, records it and hands back the value (or the fake pointer for the
// next table). The fetch shader and vertex buffer descriptors are recorded from the parse the
// draw just did. Program code is not recorded: in the same launch it is still in memory.
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
// made, for a later launch to replay. With only that set, nothing is replayed in this launch,
// so the replay queue's cap drops nothing. GOW3_PRECOMPILE_LIST=<file> loads such a list in a
// later launch, possibly with another build.
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
#include <unordered_set>
#include <fmt/format.h>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "common/decoder.h"
#include "common/hash.h"
#include "core/signals.h"
#include "gow3_tls.h"
#include "gow3_toggles.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
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
constexpr size_t PageSize = 4096;
constexpr u64 RecordSlotSize = 16ull << 20;
constexpr u32 NumRecordSlots = 64;
constexpr u64 ShadowArenaSize = 1ull << 30;
constexpr size_t MaxShadowBytes = 4u << 20; ///< one capture
constexpr size_t MaxRecordedWords = 4096, MaxRecordedPointers = 256;

static_assert(ListRegsWords == AmdGpu::Regs::NumRegs);
static_assert(ListUserDataWords == AmdGpu::NUM_USER_DATA);
static_assert(ListStages == MaxShaderStages);

/// Address ranges this diagnostic reserved: the record slots and the shadow arena, whose first
/// page stays inaccessible (the "dead" pointer for anything not recorded).
struct Reserved {
    u64 record = 0, shadow = 0;
    [[nodiscard]] bool InRecord(u64 at) const {
        return at - record < RecordSlotSize * NumRecordSlots;
    }
    [[nodiscard]] bool InShadow(u64 at) const {
        return at - shadow < ShadowArenaSize;
    }
};
Reserved g_reserved; // set once, before the fault handler is registered

#ifdef __linux__
u64 Reserve(u64 size) {
    void* at = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return at == MAP_FAILED ? 0 : u64(at);
}
bool Protect(u64 address, size_t size, bool access) {
    return mprotect(reinterpret_cast<void*>(address), size,
                    access ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
}
void Discard(u64 address, size_t size) {
    madvise(reinterpret_cast<void*>(address), size, MADV_DONTNEED);
    Protect(address, size, false);
}
int g_read_errno; ///< of the first failed ReadGuest: EPERM from a sandbox, EFAULT unreadable

/// A guest read that fails instead of faulting, so the fault handler can use it.
bool ReadGuest(u64 address, void* out, size_t size) {
    const int saved = errno;
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    const bool read = process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
    if (!read && !g_read_errno) {
        g_read_errno = errno;
    }
    errno = saved;
    return read;
}
#else
u64 Reserve(u64) {
    return 0;
}
bool Protect(u64, size_t, bool) {
    return false;
}
void Discard(u64, size_t) {}
bool ReadGuest(u64, void*, size_t) {
    return false;
}
#endif

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

/// Fault-handler state of the GPU command thread, the only one that records or replays.
struct ThreadState {
    WalkRecorder* recorder;                                  ///< set while a walker is recorded
    std::array<std::pair<u64, u64>, MaxShaderStages> walkers; ///< code allowed to read our ranges
    u32 num_walkers;
    bool watching; ///< a recording or replay runs: note every fault
    u32 faults;
    u64 fault; ///< the first one's address

    void Watch(std::span<const StageCapture> stages) {
        num_walkers = 0;
        for (const auto& stage : stages) {
            if (stage.walker) {
                walkers[num_walkers++] = {stage.walker, stage.walker + stage.walker_size};
            }
        }
        faults = 0;
        fault = 0;
        watching = true;
    }
    void Stop() {
        recorder = nullptr;
        num_walkers = 0;
        watching = false;
    }
    [[nodiscard]] bool InWalker(u64 rip) const {
        for (u32 i = 0; i < num_walkers; ++i) {
            if (rip >= walkers[i].first && rip < walkers[i].second) {
                return true;
            }
        }
        return false;
    }
    void Note(u64 address) {
        if (faults++ == 0) {
            fault = address;
        }
    }
};
constinit thread_local ThreadState t_state GOW3_TLS_INITIAL_EXEC{};

/// Access-violation handler (priority 0, before the SRT walker patch). Reads of a watched
/// walker are served here: recorded guest words while recording, else zero (and counted), so
/// the SRT walker patch never rewrites a walker the game still uses. Other faults in our
/// ranges are noted and go on to the recovery point (runtime_fault_recover); the rest pass
/// through untouched (a guest page the page manager resolves is not a stray read).
bool OnFault(void* context, void* fault_address) {
#ifdef __linux__
    auto& t = t_state;
    const u64 address = u64(fault_address);
    if (!t.watching && !g_reserved.InRecord(address) && !g_reserved.InShadow(address)) {
        return false; // the game's own faults
    }
    auto& gregs = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(gregs[REG_RIP]);
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const bool walker_read =
        t.InWalker(rip) &&
        ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
            inst, operands, reinterpret_cast<void*>(rip))) &&
        inst.mnemonic == ZYDIS_MNEMONIC_MOV && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
        (operands[0].reg.value == ZYDIS_REGISTER_RDI ||
         operands[0].reg.value == ZYDIS_REGISTER_R10D);
    const bool ours = g_reserved.InRecord(address) || g_reserved.InShadow(address);
    if (!walker_read) {
        if (ours) {
            t.Note(address); // a descriptor or fetch-shader read: back to the recovery point
        }
        return false;
    }
    const bool is_pointer = operands[0].reg.value == ZYDIS_REGISTER_RDI;
    u64 value;
    if (t.recorder && g_reserved.InRecord(address)) {
        value = t.recorder->Load(u64(gregs[REG_RDI]), address, is_pointer);
    } else {
        // Outside the recorded words (or the record slots in a replay): a zero, or for a
        // pointer the dead page (not 0, whose next read would fault again outside the shadow).
        t.Note(address);
        value = is_pointer ? g_reserved.shadow : 0;
    }
    gregs[is_pointer ? REG_RDI : REG_R10] = greg_t(value);
    gregs[REG_RIP] += inst.length;
    return true;
#else
    return false;
#endif
}

/// Runs `body`; false when a fault jumped back out of it (runtime_fault_recover).
template <typename Body>
[[gnu::noinline]] bool Guarded(Body&& body) {
    sigjmp_buf* const outer = runtime_fault_recover;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        runtime_fault_recover = outer;
        return false;
    }
    runtime_fault_recover = &recover;
    body();
    runtime_fault_recover = outer;
    return true;
}

/// Pages for the shadow copies, handed out once each: an address is never reused, so the
/// fetch-shader parse cache (keyed by code address) cannot match an older copy.
class ShadowArena {
public:
    std::optional<u64> Map(const ShadowLayout& layout) {
        const size_t size = layout.Size();
        if (size > MaxShadowBytes || next + size > g_reserved.shadow + ShadowArenaSize ||
            !Protect(next, size, true)) {
            return std::nullopt;
        }
        const u64 address = next;
        next += size;
        for (const size_t guard : layout.GuardPages()) {
            if (!Protect(address + guard, PageSize, false)) {
                Discard(address, size); // without its guard a stray read would go unnoticed
                return std::nullopt;
            }
        }
        return address;
    }
    void Unmap(u64 address, const ShadowLayout& layout) {
        Discard(address, layout.Size());
    }

private:
    u64 next = g_reserved.shadow + PageSize; ///< the first page is the dead page
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

/// Captures written to the list file, and those left out.
struct RecordTally {
    u64 written, bytes, not_written;
    bool operator==(const RecordTally&) const = default;
};

enum class Detail { Different, Faulted, NotFound, CaptureFailed, Count };

/// Each stage's program hash (0 for none), by SwStage: what a list entry waits for.
using ProgramSet = std::array<u64, MaxShaderStages>;

/// A key recorded by an earlier launch and what became of it in this one.
struct ListEntry {
    RecordedKey key;
    ProgramSet programs;
};

/// Detail lines of the list replay.
enum class ListNote { Rejected, Count };

/// What a replayed selection came to.
enum class Outcome { Selected, NotFound, Faulted, CodeGone, WalkerChanged, NoRoom };

struct ReplayResult {
    Outcome outcome;
    GraphicsPipelineKey key; ///< the selected key
    u32 faults;
    u64 fault;           ///< the first fault's address
    size_t shadow_bytes; ///< of the shadow copy
};

const char* FaultPlace(u64 fault) {
    return g_reserved.InShadow(fault)   ? "shadow guard or dead page"
           : g_reserved.InRecord(fault) ? "record slots"
           : fault                      ? "guest or host memory"
                                        : "unknown address";
}

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

    [[nodiscard]] bool Capturing() const {
        return replay_same_launch || record_file;
    }

    u64 submissions = 0;
    Tally tally{}, reported{};

private:
    void Record(const KeyCapture& c);
    const char* RecordStage(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordWalker(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordFetch(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    void ReplayOne(PipelineCache& cache, KeyCapture& c);
    ReplayResult Select(PipelineCache& cache, KeyCapture& c);
    std::optional<Outcome> Stale(PipelineCache& cache, const KeyCapture& c);
    std::string Difference(const KeyCapture& c, const GraphicsPipelineKey& key) const;
    void Log(Detail kind, const std::string& text);
    void Note(ListNote kind, const std::string& text);

    const bool replay_same_launch; ///< GOW3_PRECOMPILE_REPLAY=1
    std::FILE* record_file;        ///< GOW3_PRECOMPILE_RECORD, null when off or failed
    RecordTally record{}, record_reported{};
    bool record_capped_logged = false;
    RecordSlots slots;
    ShadowArena arena;
    std::unordered_set<u64> seen;          ///< key hashes already captured or skipped
    std::deque<KeyCapture> waiting;
    std::array<u32, size_t(Detail::Count)> details{};
    std::chrono::steady_clock::time_point last_report{};
    bool cap_logged = false, arena_logged = false, read_logged = false;

    // GOW3_PRECOMPILE_LIST: a list recorded by an earlier launch.
    std::vector<ListEntry> list;
    std::map<ProgramSet, std::vector<u32>> list_by_programs; ///< entry indices
    u32 list_rejected = 0;
    std::array<u32, size_t(ListNote::Count)> list_notes{};
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
                    "(geometry/size/write)%s\n",
                    static_cast<unsigned long long>(record.written),
                    static_cast<unsigned long long>(record.bytes >> 10),
                    static_cast<unsigned long long>(record.not_written), final ? " (exit)" : "");
    }
    if (!final && tally == reported) {
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
            if (g_read_errno && !read_logged) {
                read_logged = true;
                std::printf("Precompile replay: first failed guest read: %s\n",
                            std::strerror(g_read_errno));
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
    list_rejected = parsed.rejected;
    for (const auto& reason : parsed.reasons) {
        Note(ListNote::Rejected, "rejected " + reason);
    }
    const u32 stage_enable = StageEnableWord();
    for (auto& key : parsed.keys) {
        // Only what RECORD writes: vertex + fragment keys (no geometry or tessellation).
        const bool vs_only = std::ranges::none_of(key.regs, [&](const auto& reg) {
            return reg.first == stage_enable && reg.second != AmdGpu::ShaderStageEnable::Vs;
        });
        if (!vs_only) {
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

/// Appends a capture to the list file, flushed at once: the game may end without exiting.
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
    const auto entry = EncodeListEntry(key);
    if (entry.size() > MaxListEntryBytes || record.bytes + entry.size() > MaxListBytes) {
        ++record.not_written;
        if (!record_capped_logged) {
            record_capped_logged = true;
            std::printf("Precompile record: list full (%zu MiB), later captures not written\n",
                        MaxListBytes >> 20);
        }
        return;
    }
    if (std::fwrite(entry.data(), 1, entry.size(), record_file) != entry.size() ||
        std::fflush(record_file) != 0) {
        ++record.not_written;
        std::printf("Precompile record: write failed (%s), recording stopped\n",
                    std::strerror(errno));
        std::fclose(record_file);
        record_file = nullptr;
        return;
    }
    ++record.written;
    record.bytes += entry.size();
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
    t_state.Stop();
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
    t_state.Watch({&stage, 1});
    t_state.recorder = &recorder;
    info.srt_info.walker_func(reinterpret_cast<const u32*>(slots.Address(0)), flat.data());
    const u32 faults = t_state.faults;
    t_state.Stop();
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
    Report(false);
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
    t_state.Watch(c.stages);
    const bool completed = Guarded([&] {
        // Checked right before the lookup, so the code cannot change in between.
        stale = Stale(cache, c);
        checked = true;
        if (!stale) {
            selected = cache.RefreshGraphicsKey(selection);
        }
    });
    result.faults = t_state.faults;
    result.fault = t_state.fault;
    t_state.Stop();
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

namespace {
/// The value of an environment switch that names a file; null when unset or empty.
const char* PathSetting(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : nullptr;
}

/// The list at `path`, none when it cannot be used (logged).
std::optional<ParsedList> LoadList(const char* path) {
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
    auto parsed = ParseList(bytes, MaxCaptures);
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
    auto parsed = list_path ? LoadList(list_path) : std::nullopt;
    std::FILE* record_file = record_path ? OpenRecord(record_path) : nullptr;
    if (!replay && !record_file && !parsed) {
        return nullptr;
    }
    if (!g_reserved.record) {
        g_reserved.record = Reserve(RecordSlotSize * NumRecordSlots);
        g_reserved.shadow = Reserve(ShadowArenaSize);
        if (!g_reserved.record || !g_reserved.shadow) {
            std::printf("Precompile replay: could not reserve address space, off\n");
            g_reserved = {};
            if (record_file) {
                std::fclose(record_file);
            }
            return nullptr;
        }
        (void)Common::Decoder::Instance(); // built now: the fault handler must not allocate
        Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
        std::atexit(ReportAtExit);
    }
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
}

void PipelineCache::ReplayPrecompile() {
    if (precompile) {
        precompile->Replay(*this);
    }
}

} // namespace Vulkan
