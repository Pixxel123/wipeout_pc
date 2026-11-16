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
// counted. Check A: the same GraphicsPipelineKey (which holds each stage's permutation).
// Check B (GOW3_PRECOMPILE_REPLAY=2): one throwaway first translation per program from the
// shadow, its SPIR-V compared with the stored SPIR-V of the permutation the draw used.
// Nothing shared is written: program_cache is only read, under programs_mutex.
//
// Linux only (the fault handler edits the walker's registers in the signal context).

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
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
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_precompile_replay.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan {

namespace {

using namespace Precompile;
using Shader::HwStage;
using Shader::SwStage;

constexpr u32 MaxCaptures = 2000;
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
/// A guest read that fails instead of faulting, so the fault handler can use it.
bool ReadGuest(u64 address, void* out, size_t size) {
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
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
/// the SRT walker patch never rewrites a walker the game still uses. Any other fault while
/// watching is only noted and goes on to the recovery point (runtime_fault_recover).
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
    if (!walker_read) {
        t.Note(address); // a descriptor or fetch-shader read: back to the recovery point
        return false;
    }
    const bool is_pointer = operands[0].reg.value == ZYDIS_REGISTER_RDI;
    u64 value;
    if (t.recorder && g_reserved.InRecord(address)) {
        value = t.recorder->Load(u64(gregs[REG_RDI]), address, is_pointer);
    } else {
        // Outside the recorded words: a zero, or for a pointer the dead page (not 0, whose
        // next read would fault again outside the shadow).
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
        for (const size_t guard : layout.GuardPages()) {
            Protect(next + guard, PageSize, false);
        }
        const u64 address = next;
        next += size;
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
    u64 captured, same, different, faulted, not_found, skipped, capture_failed, code_gone, words;
    u64 spirv_equal, spirv_different, spirv_faulted, spirv_unchecked;
    bool operator==(const Tally&) const = default;
};

enum class Detail { Different, Faulted, NotFound, CaptureFailed, Spirv, Count };

} // namespace

struct PrecompileReplay {
    explicit PrecompileReplay(int mode_)
        : mode{mode_}, slots{g_reserved.record, RecordSlotSize, NumRecordSlots} {}

    void Capture(PipelineCache& cache);
    void Replay(PipelineCache& cache);
    void Report(bool final);

    int mode; ///< 1: selection, 2: also SPIR-V
    u64 submissions = 0;
    Tally tally{}, reported{};

private:
    const char* RecordStage(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordWalker(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordFetch(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    void ReplayOne(PipelineCache& cache, KeyCapture& c);
    void CheckSpirv(PipelineCache& cache, const KeyCapture& c, const PipelineSelection& sel);
    std::string Difference(const KeyCapture& c, const GraphicsPipelineKey& key) const;
    void Log(Detail kind, const std::string& text);

    RecordSlots slots;
    ShadowArena arena;
    std::unordered_set<u64> seen;          ///< key hashes already captured or skipped
    std::unordered_set<u64> spirv_checked; ///< program hashes
    std::deque<KeyCapture> waiting;
    Shader::Pools pools;
    std::array<u32, size_t(Detail::Count)> details{};
    std::chrono::steady_clock::time_point last_report{};
    bool cap_logged = false, arena_logged = false;
};

namespace {
PrecompileReplay* g_active; ///< reported at exit unless destroyed before

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

void PrecompileReplay::Report(bool final) {
    const auto now = std::chrono::steady_clock::now();
    if (!final && (now - last_report < ReportInterval || tally == reported)) {
        return;
    }
    last_report = now;
    reported = tally;
    const auto& t = tally;
    std::printf("%s\n",
                fmt::format("Precompile replay: {} captured, {} same key, {} different key, {} "
                            "faulted (outside shadow), {} not found, {} skipped (cap/hs/compute), "
                            "{} capture failed, {} code gone, {} waiting, {} guest words; SPIR-V "
                            "equal {} / different {} / faulted {} / not checked {}{}",
                            t.captured, t.same, t.different, t.faulted, t.not_found, t.skipped,
                            t.capture_failed, t.code_gone, waiting.size(), t.words, t.spirv_equal,
                            t.spirv_different, t.spirv_faulted, t.spirv_unchecked,
                            final ? " (exit)" : "")
                    .c_str());
}

void PrecompileReplay::Capture(PipelineCache& cache) {
    const auto& sel = cache.sel;
    const u64 key_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    if (!seen.insert(key_hash).second) {
        return;
    }
    if (sel.regs->stage_enable.hs_en) {
        ++tally.skipped; // tessellation reads constants at selection time: not prepared either
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
    waiting.push_back(std::move(c));
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

void PrecompileReplay::ReplayOne(PipelineCache& cache, KeyCapture& c) {
    bool present = false;
    Guarded([&] {
        present = std::ranges::all_of(c.programs, [](const auto& program) {
            return CodeStillThere(program.first, program.second);
        });
    });
    if (!present) {
        ++tally.code_gone;
        return;
    }
    const ShadowLayout layout{c.words, PageSize};
    const auto address = arena.Map(layout);
    if (!address) {
        ++tally.skipped;
        if (!arena_logged) {
            arena_logged = true;
            std::printf("Precompile replay: no room for the shadow copy of key %016llx (%zu "
                        "bytes), skipped\n",
                        static_cast<unsigned long long>(c.key_hash), layout.Size());
        }
        return;
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

    // Check A: a lookup-only selection on a private PipelineSelection.
    PrepWorker worker{};
    PipelineSelection selection{};
    selection.regs = c.regs.get();
    selection.worker = &worker;
    selection.draw_indirect_params = c.draw_indirect_params;
    bool selected = false;
    t_state.Watch(c.stages);
    const bool completed = Guarded([&] { selected = cache.RefreshGraphicsKey(selection); });
    const u32 faults = t_state.faults;
    const u64 fault = t_state.fault;
    t_state.Stop();

    if (!completed || worker.faulted || faults != 0) {
        ++tally.faulted;
        const char* where = g_reserved.InShadow(fault)  ? "shadow guard or dead page"
                            : g_reserved.InRecord(fault) ? "record slots"
                            : fault                     ? "guest or host memory"
                                                        : "unknown address";
        Log(Detail::Faulted, fmt::format("key {:016x}: read outside the recorded words at {:#x} "
                                         "({}), {} fault(s)",
                                         c.key_hash, fault, where, faults));
    } else if (!selected) {
        ++tally.not_found;
        Log(Detail::NotFound, fmt::format("key {:016x}: program or permutation not found",
                                          c.key_hash));
    } else if (selection.graphics_key == c.key) {
        ++tally.same;
        if (mode == 2) {
            CheckSpirv(cache, c, selection);
        }
    } else {
        ++tally.different;
        Log(Detail::Different, fmt::format("key {:016x} replayed as {:016x}: {}", c.key_hash,
                                           std::hash<GraphicsPipelineKey>{}(selection.graphics_key),
                                           Difference(c, selection.graphics_key)));
    }
    arena.Unmap(*address, layout);
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

void PrecompileReplay::CheckSpirv(PipelineCache& cache, const KeyCapture& c,
                                  const PipelineSelection& sel) {
    for (const auto& stage : c.stages) {
        if (!spirv_checked.insert(stage.hash).second) {
            continue;
        }
        // The stored SPIR-V of the permutation the draw used, and that permutation's bindings.
        const int perm = PermutationOf(stage.hash, c.key.stage_hashes[u32(stage.sw_stage)]);
        std::optional<Shader::Backend::Bindings> start;
        {
            std::shared_lock lock{cache.programs_mutex};
            const auto found = cache.program_cache.find(stage.hash);
            if (perm >= 0 && found != cache.program_cache.end() &&
                size_t(perm) < found->second->modules.size() &&
                found->second->modules[perm].spec.Valid()) {
                start = found->second->modules[perm].spec.start;
            }
        }
        std::vector<u32> stored;
        if (start) {
            const auto name = fmt::format("{:#018x}_{}", stage.hash, perm);
            Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary, name, stored);
        }
        const auto params = AmdGpu::GetParams(*c.regs->ProgramForStage(u32(stage.hw_stage)));
        if (stored.empty() || params.hash != stage.hash) {
            ++tally.spirv_unchecked;
            continue;
        }
        // A first translation from the shadow (the copied registers hold its user data). Only
        // after check A found the same key: the inputs match a translation that succeeded, so
        // the translator's asserts are not expected to fire.
        Shader::Info info{stage.hw_stage, stage.sw_stage, params};
        auto runtime_info = sel.runtime_infos[u32(stage.sw_stage)];
        std::vector<u32> spirv;
        t_state.Watch(c.stages);
        const bool completed = Guarded([&] {
            const auto ir =
                Shader::TranslateProgram(params.code, pools, info, runtime_info, cache.profile);
            auto binding = *start;
            spirv = Shader::Backend::SPIRV::EmitSPIRV(cache.profile, runtime_info, ir, binding);
        });
        const u32 faults = t_state.faults;
        const u64 fault = t_state.fault;
        t_state.Stop();
        if (!completed || faults != 0) {
            ++tally.spirv_faulted;
            Log(Detail::Spirv, fmt::format("SPIR-V of {:016x}/{}: translation read outside the "
                                           "recorded words at {:#x}",
                                           stage.hash, perm, fault));
        } else if (spirv == stored) {
            ++tally.spirv_equal;
        } else {
            ++tally.spirv_different;
            size_t word = 0;
            while (word < spirv.size() && word < stored.size() && spirv[word] == stored[word]) {
                ++word;
            }
            Log(Detail::Spirv, fmt::format("SPIR-V of {:016x}/{} differs: {} words, stored {}, "
                                           "first difference at word {}",
                                           stage.hash, perm, spirv.size(), stored.size(), word));
        }
    }
}

void PrecompileReplayDeleter::operator()(PrecompileReplay* replay) const {
    if (g_active == replay) {
        replay->Report(true);
        g_active = nullptr;
    }
    delete replay;
}

PrecompileReplayPtr MakePrecompileReplay() {
    const char* env = std::getenv("GOW3_PRECOMPILE_REPLAY");
    const int mode = env ? std::atoi(env) : 0;
    if (mode != 1 && mode != 2) {
        return nullptr;
    }
#ifndef __linux__
    std::printf("Precompile replay: not supported on this platform\n");
    return nullptr;
#else
    if (!g_reserved.record) {
        g_reserved.record = Reserve(RecordSlotSize * NumRecordSlots);
        g_reserved.shadow = Reserve(ShadowArenaSize);
        if (!g_reserved.record || !g_reserved.shadow) {
            std::printf("Precompile replay: could not reserve address space, off\n");
            g_reserved = {};
            return nullptr;
        }
        (void)Common::Decoder::Instance(); // built now: the fault handler must not allocate
        Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
        std::atexit(ReportAtExit);
    }
    auto* replay = new PrecompileReplay(mode);
    g_active = replay;
    std::printf("Precompile replay: on (%s)\n",
                mode == 2 ? "selection and SPIR-V" : "selection only");
    return PrecompileReplayPtr{replay};
#endif
}

void PipelineCache::CapturePrecompile() {
    precompile->Capture(*this);
}

void PipelineCache::SkipPrecompile() {
    ++precompile->tally.skipped;
}

void PipelineCache::ReplayPrecompile() {
    if (precompile) {
        precompile->Replay(*this);
    }
}

} // namespace Vulkan
