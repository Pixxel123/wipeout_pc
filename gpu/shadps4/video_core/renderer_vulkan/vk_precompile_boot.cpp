// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_BOOT=1 with GOW3_PRECOMPILE_LIST=<file>: after a recompiler
// change dropped the shader store's translated shaders (StartOnDemand), the programs of a list
// recorded by an earlier launch (GOW3_PRECOMPILE_RECORD) are translated back into the store
// before the game runs, so the first race reads them from disk as on a second visit.
//
// Each entry is replayed as a selection on a private PipelineSelection, through
// RefreshGraphicsKey itself, so stage order, runtime info and the bindings carried from the
// fragment to the vertex stage are those of a draw. Its registers, user data and table words
// come from the list and sit in a shadow copy (vk_precompile_shadow.h) together with the
// recorded program code. GetProgram hands each stage to BootProgram instead of program_cache:
// a program not seen yet in this rebuild is translated as permutation 0; a later entry of the
// same program that specialises differently is translated as the next permutation, numbered in
// first-seen order. Each translation is written as the shader compiler writes it (SPIR-V, then
// the meta with the program's first Info), and nothing of the pipeline cache's own state is
// touched: program_cache, the pending builds, the failure lists and the pipelines.
//
// Single-threaded and blocking, Linux only (the fault handler). Every SRT walker this rebuild
// creates is watched: a read of one that faults is served as zero and rejects the entry, so the
// SRT walker patch never rewrites a walker that the store then saves. A walker that reads a
// word or follows a pointer the recording did not would read the shadow's gaps or a stale guest
// address without faulting, so each walk is first run on the recorded words alone (CheckWalk).

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#ifdef __linux__
#include <sys/resource.h>
#endif

#include "common/hash.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_precompile_guard.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan {

namespace {

using namespace Precompile;
using Shader::HwStage;
using Shader::SwStage;

constexpr u32 MaxEntries = 4000;
constexpr u32 MaxDetails = 8;        ///< detail lines per reason
constexpr size_t MaxRecordedWords = 4096, MaxRecordedPointers = 256; ///< as when recorded
constexpr size_t MaxPermutations = 64; ///< IndexDiskShaders reads indices below 64
/// Size of the SRT walker code generator (flatten_extended_userdata_pass.cpp).
constexpr u64 WalkerCodeSize = 32ull << 20;

/// Why an entry was not translated.
enum class Reject {
    NotVertexFragment, ///< only vertex + fragment keys are recorded
    NoCode,            ///< a program's code is not in the list
    NoRoom,            ///< no room for the shadow copy
    Faulted,           ///< a read outside the recorded words faulted
    NotSelected,       ///< the selection stopped (an unsupported state)
    SpecChanged,       ///< a translation specialises unlike the lookup that asked for it
    StrayWalk,         ///< a walker reads a word or follows a pointer the recording did not
    TooManyPermutations,
    Count,
};

constexpr std::array<const char*, size_t(Reject::Count)> RejectNames = {
    "not vertex + fragment", "no code", "no room", "faulted", "not selected", "spec changed",
    "walk outside the recording", "too many permutations",
};

/// Synthetic base of a stage's code in the shadow layout: no guest pointer (48 bits) has it.
constexpr u64 CodeBase(u32 sw_stage) {
    return (1ull << 63) | sw_stage;
}

/// Whether AmdGpu::GetParams finds this code's header without searching, with the real
/// AmdGpu::BinaryInfo (the list checked the same through byte offsets).
bool HeaderFoundFirst(std::span<const u32> code, u64 hash) {
    if (CheckCode(hash, code)) {
        return false;
    }
    const auto* info =
        std::bit_cast<const AmdGpu::BinaryInfo*>(code.data() + CodeHeaderAt(code));
    return info->Valid() && info->shader_hash == hash && info->length <= code.size_bytes();
}

bool VertexFragmentOnly(const RecordedKey& key, const AmdGpu::Regs& regs) {
    return regs.stage_enable.raw == AmdGpu::ShaderStageEnable::Vs &&
           std::ranges::all_of(key.stages, [](const RecordedStage& stage) {
               return (stage.sw_stage == u32(SwStage::Fragment) &&
                       stage.hw_stage == u32(HwStage::Fragment)) ||
                      (stage.sw_stage == u32(SwStage::Vertex) &&
                       stage.hw_stage == u32(HwStage::Vertex));
           });
}

AmdGpu::ShaderProgram& ProgramAt(AmdGpu::Regs& regs, u32 hw_stage) {
    return const_cast<AmdGpu::ShaderProgram&>(*regs.ProgramForStage(hw_stage));
}

/// A program translated in this rebuild: the Info of its first translation, which every meta
/// of the program carries (as PublishShaders writes it), and its permutations' specs by index.
struct BootProgram {
    Shader::Info info;
    std::vector<Shader::StageSpecialization> specs;
};

/// The shadow copy of one entry: its words and each stage's code, with a guard page after each.
struct Shadow {
    std::vector<GuestWord> words;
    std::optional<ShadowLayout> layout;
    u64 address = 0;
};

/// The recorded words of the entry being checked, by guest address (base + offset), sorted.
const std::vector<std::pair<u64, u32>>* g_recorded;

/// WalkRecorder's guest read for a check walk: recorded words only. Runs in the fault handler.
bool ReadRecorded(u64 address, void* out, size_t size) {
    auto* bytes = static_cast<u8*>(out);
    for (size_t at = 0; at < size; at += sizeof(u32)) {
        const auto found = std::ranges::lower_bound(*g_recorded, address + at, {},
                                                    &std::pair<u64, u32>::first);
        if (found == g_recorded->end() || found->first != address + at) {
            return false;
        }
        std::memcpy(bytes + at, &found->second, sizeof(u32));
    }
    return true;
}

u64 PeakMemoryMiB() {
#ifdef __linux__
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? u64(usage.ru_maxrss) >> 10 : 0;
#else
    return 0;
#endif
}

} // namespace

struct PrecompileBoot {
    PrecompileBoot(PipelineCache& cache_, const ParsedList& list_) : cache{cache_}, list{list_} {}

    void Run();
    PipelineCache::Result Program(const Shader::ShaderParams& params, HwStage hw_stage,
                                  SwStage sw_stage, Shader::Backend::Bindings& binding,
                                  const Shader::RuntimeInfo& runtime_info);

private:
    void Entry(u32 index, const RecordedKey& key);
    std::optional<Reject> Select(const RecordedKey& key, Shadow& shadow);
    bool MapShadow(const RecordedKey& key, Shadow& shadow);
    std::unique_ptr<AmdGpu::Regs> Registers(const RecordedKey& key, const Shadow& shadow) const;
    bool CheckWalk(const Shader::Info& info, u64 hash);
    std::optional<size_t> Translate(BootProgram& program, bool initial,
                                    const Shader::ShaderParams& params, HwStage hw_stage,
                                    SwStage sw_stage, const Shader::Backend::Bindings& binding,
                                    const Shader::RuntimeInfo& runtime_info,
                                    const Shader::StageSpecialization* lookup);
    void Stop(Reject why, std::string detail);
    void Watch();
    [[nodiscard]] bool Failed() const;
    void Detail(Reject why, const std::string& text);
    void Report() const;

    PipelineCache& cache;
    const ParsedList& list;
    std::unordered_map<u64, BootProgram> programs; ///< node-based: specs point at their Info
    Shader::Pools pools; ///< the rebuild's own: a fault jumps out of a translation
    ShadowArena arena;
    ThreadState::Range walker_code{}; ///< where the SRT walkers of this rebuild are generated
    RecordSlots slots{g_reserved.record, RecordSlotSize, NumRecordSlots};
    std::vector<GuestWord> walk_words;
    std::vector<GuestPointer> walk_pointers;
    std::vector<std::pair<u32, u64>> walk_roots;

    // The entry being translated.
    const RecordedKey* entry = nullptr;
    std::vector<std::pair<u64, u32>> recorded; ///< its words by guest address
    std::optional<Reject> stopped;
    std::string stop_detail;

    u32 translated = 0, permutations = 0;
    std::array<u32, size_t(Reject::Count)> rejects{};
    std::chrono::steady_clock::time_point started;
};

void PrecompileBoot::Run() {
    started = std::chrono::steady_clock::now();
    // Every walker translated from here on is generated after the current end of the walker
    // code, inside the generator's 32 MiB (nothing of the store was loaded yet).
    const u64 walkers = u64(Shader::RegisterWalkerCode(nullptr, 0));
    walker_code = {walkers, walkers + WalkerCodeSize};
    walk_words.reserve(MaxRecordedWords);
    walk_pointers.reserve(MaxRecordedPointers);
    walk_roots.reserve(ListUserDataWords);
    const u32 count = u32(std::min<size_t>(list.keys.size(), MaxEntries));
    for (u32 index = 0; index < count; ++index) {
        Entry(index, list.keys[index]);
    }
    Report();
}

void PrecompileBoot::Entry(u32 index, const RecordedKey& key) {
    Shadow shadow;
    entry = &key;
    recorded.clear();
    for (const auto& word : key.words) {
        recorded.emplace_back(word.base + word.offset, word.value);
    }
    std::ranges::sort(recorded);
    stopped.reset();
    stop_detail.clear();
    const auto why = Select(key, shadow);
    if (shadow.layout) {
        arena.Unmap(shadow.address, *shadow.layout);
    }
    // A fault can jump out of a program's first translation before Program drops it, and the
    // Infos' user data pointed into this entry's registers.
    std::erase_if(programs, [](const auto& item) { return item.second.specs.empty(); });
    for (auto& [hash, program] : programs) {
        program.info.user_data = {};
    }
    entry = nullptr;
    if (!why) {
        ++translated;
        return;
    }
    ++rejects[size_t(*why)];
    Detail(*why, fmt::format("entry {} (recorded key {:016x}) rejected, {}{}{}", index,
                             key.key_hash, RejectNames[size_t(*why)],
                             stop_detail.empty() ? "" : ": ", stop_detail));
}

/// Replays the entry's selection from its shadow copy; why it was not translated, or none.
std::optional<Reject> PrecompileBoot::Select(const RecordedKey& key, Shadow& shadow) {
    for (const auto& stage : key.stages) {
        const auto code = list.code.find(stage.hash);
        if (code == list.code.end() || !HeaderFoundFirst(code->second, stage.hash)) {
            stop_detail = fmt::format("no code for program {:016x}", stage.hash);
            return Reject::NoCode;
        }
    }
    if (!MapShadow(key, shadow)) {
        return Reject::NoRoom;
    }
    const auto regs = Registers(key, shadow);
    if (!VertexFragmentOnly(key, *regs)) {
        return Reject::NotVertexFragment;
    }
    PipelineSelection selection{};
    selection.regs = regs.get();
    selection.boot = this;
    selection.draw_indirect_params = {u16(key.vertex_sgpr_offset), key.instance_sgpr_offset};
    bool selected = false;
    Watch();
    const bool completed = Guarded([&] { selected = cache.RefreshGraphicsKey(selection); });
    const bool faulted = FaultState().faults != 0;
    FaultState().Stop();
    if (stopped) {
        return stopped;
    }
    if (!completed || faulted) {
        const auto& state = FaultState();
        stop_detail = fmt::format("read outside the recorded words at {:#x} ({})", state.fault,
                                  FaultPlace(state.fault));
        return Reject::Faulted;
    }
    return selected ? std::nullopt : std::optional{Reject::NotSelected};
}

/// The entry's words and each stage's code, laid out with guard pages and filled in.
bool PrecompileBoot::MapShadow(const RecordedKey& key, Shadow& shadow) {
    shadow.words = key.words;
    for (const auto& stage : key.stages) {
        // Padded to whole pages: the program address register holds bits 8 and up, and a
        // region ends right before its guard page.
        const auto& code = list.code.at(stage.hash);
        const size_t bytes = code.size() * sizeof(u32);
        const size_t padded = (bytes + PageSize - 1) / PageSize * PageSize;
        for (u32 i = 0; i < code.size(); ++i) {
            shadow.words.push_back({CodeBase(stage.sw_stage), i * 4, code[i]});
        }
        if (padded > bytes) {
            shadow.words.push_back({CodeBase(stage.sw_stage), u32(padded - 4), 0});
        }
    }
    shadow.layout.emplace(shadow.words, PageSize);
    const auto address = arena.Map(*shadow.layout);
    if (!address) {
        shadow.layout.reset();
        return false;
    }
    shadow.address = *address;
    shadow.layout->Fill(reinterpret_cast<u8*>(*address), *address, shadow.words, key.pointers,
                        g_reserved.shadow);
    return true;
}

/// The recorded registers, with each stage's code address and user-data pointers led into
/// the shadow copy; stages not recorded have no program.
std::unique_ptr<AmdGpu::Regs> PrecompileBoot::Registers(const RecordedKey& key,
                                                        const Shadow& shadow) const {
    auto regs = std::make_unique<AmdGpu::Regs>();
    regs->reg_array.fill(0);
    for (const auto& [index, value] : key.regs) {
        regs->reg_array[index] = value;
    }
    for (u32 hw_stage = 0; hw_stage < ListHwStages; ++hw_stage) {
        ProgramAt(*regs, hw_stage).address = 0;
    }
    for (const auto& stage : key.stages) {
        auto& program = ProgramAt(*regs, stage.hw_stage);
        const u64 code = *shadow.layout->Translate(CodeBase(stage.sw_stage), shadow.address);
        program.address = code >> 8; // 40 bits: the address without its low 8
        program.user_data = stage.user_data;
        for (const auto& [sgpr, target] : stage.roots) {
            u64 pointer;
            std::memcpy(&pointer, &program.user_data[sgpr], sizeof(pointer));
            pointer = Repoint(pointer, shadow.layout->Translate(target, shadow.address)
                                           .value_or(g_reserved.shadow));
            std::memcpy(&program.user_data[sgpr], &pointer, sizeof(pointer));
        }
    }
    return regs;
}

/// GetProgram for a boot selection: the program's permutation for this stage, translated and
/// written to the store when it is new.
PipelineCache::Result PrecompileBoot::Program(const Shader::ShaderParams& params,
                                              HwStage hw_stage, SwStage sw_stage,
                                              Shader::Backend::Bindings& binding,
                                              const Shader::RuntimeInfo& runtime_info) {
    if (stopped) {
        return {};
    }
    auto [it, is_new] = programs.try_emplace(params.hash);
    auto& program = it->second;
    std::optional<size_t> perm;
    if (is_new) {
        perm = Translate(program, true, params, hw_stage, sw_stage, binding, runtime_info,
                         nullptr);
        if (!perm) {
            programs.erase(it);
            return {};
        }
    } else {
        // As GetProgram looks a permutation up: the first Info with this draw's user data.
        if (!CheckWalk(program.info, params.hash)) {
            return {};
        }
        program.info.pgm_base = params.Base();
        program.info.user_data = params.user_data;
        program.info.RefreshFlatBuf();
        if (Failed()) {
            return {};
        }
        const Shader::StageSpecialization spec{program.info, runtime_info, cache.profile,
                                               binding};
        const auto found = std::ranges::find(program.specs, spec);
        perm = found != program.specs.end()
                   ? std::optional{size_t(std::distance(program.specs.begin(), found))}
                   : Translate(program, false, params, hw_stage, sw_stage, binding,
                               runtime_info, &spec);
        if (!perm) {
            return {};
        }
    }
    program.info.AddBindings(binding);
    return std::make_tuple(&program.info, vk::ShaderModule{},
                           program.specs[*perm].fetch_shader_data,
                           HashCombine(params.hash, u64(*perm)));
}

/// Translates a new permutation as the shader compiler does (QueueShader), checks it and
/// writes it; its index, or none when the entry stops.
std::optional<size_t> PrecompileBoot::Translate(BootProgram& program, bool initial,
                                                const Shader::ShaderParams& params,
                                                HwStage hw_stage, SwStage sw_stage,
                                                const Shader::Backend::Bindings& binding,
                                                const Shader::RuntimeInfo& runtime_info,
                                                const Shader::StageSpecialization* lookup) {
    const size_t perm = program.specs.size();
    if (perm >= MaxPermutations) {
        Stop(Reject::TooManyPermutations, fmt::format("program {:016x}", params.hash));
        return std::nullopt;
    }
    const auto& profile = cache.profile;
    Shader::Info info{hw_stage, sw_stage, params};
    auto runtime = runtime_info;
    const auto ir = Shader::TranslateProgram(params.code, pools, info, runtime, profile);
    if (Failed() || !CheckWalk(info, params.hash)) {
        return std::nullopt;
    }
    Shader::StageSpecialization spec{info, runtime, profile, binding};
    auto emit_binding = binding;
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, ir, emit_binding);
    if (Failed()) {
        return std::nullopt;
    }
    // The race looks this permutation up with the program's first Info and a fresh runtime
    // info: a spec that differs from that lookup would never be found.
    const auto first = initial ? Shader::StageSpecialization{info, runtime_info, profile, binding}
                               : *lookup;
    if (!(spec == first)) {
        Stop(Reject::SpecChanged, fmt::format("program {:016x} permutation {}", params.hash,
                                              perm));
        return std::nullopt;
    }
    if (initial) {
        program.info = std::move(info);
    }
    spec.info = &program.info;
    RegisterShaderBinary(std::move(spv), params.hash, perm);
    RegisterShaderMeta(program.info, spec.fetch_shader_data, spec,
                       HashCombine(params.hash, u64(perm)), perm);
    program.specs.push_back(std::move(spec));
    ++permutations;
    return perm;
}

/// Runs the program's walker on the stage's recorded user data and words alone, served by the
/// fault handler as when they were recorded: false (the entry stops) when it reads a word or
/// follows a pointer the recording did not. The shadow copy cannot tell: its gaps read as
/// zero and a pointer it does not lead in still holds a guest address, both without a fault.
bool PrecompileBoot::CheckWalk(const Shader::Info& info, u64 hash) {
    if (!info.srt_info.walker_func) {
        return true;
    }
    const auto stage = std::ranges::find(entry->stages, hash, &RecordedStage::hash);
    if (stage == entry->stages.end()) {
        Stop(Reject::StrayWalk, fmt::format("program {:016x} is not recorded", hash));
        return false;
    }
    std::vector<u32> flat(info.srt_info.flattened_bufsize_dw);
    walk_words.clear();
    walk_pointers.clear();
    walk_roots.clear();
    slots.Reset();
    WalkRecorder recorder{slots, stage->user_data, ReadRecorded, walk_words, walk_pointers,
                          walk_roots};
    auto& state = FaultState();
    const u32 faults = state.faults;
    g_recorded = &recorded;
    state.recorder = &recorder;
    info.srt_info.walker_func(reinterpret_cast<const u32*>(slots.Address(0)), flat.data());
    state.recorder = nullptr;
    g_recorded = nullptr;
    const char* why = recorder.error;
    if (!why && state.faults != faults) {
        why = "a read outside the record slots faulted";
    }
    if (!why && !std::ranges::all_of(walk_pointers, [&](const GuestPointer& pointer) {
            return std::ranges::find(entry->pointers, pointer) != entry->pointers.end();
        })) {
        why = "a table pointer the recording did not follow";
    }
    if (!why && !std::ranges::all_of(walk_roots, [&](const auto& root) {
            return std::ranges::find(stage->roots, root) != stage->roots.end();
        })) {
        why = "a user-data pointer the recording did not follow";
    }
    if (why) {
        Stop(Reject::StrayWalk, fmt::format("program {:016x}: {}", hash, why));
    }
    return !why;
}

/// Stops the entry: the selection goes on to its end without translating anything more.
void PrecompileBoot::Stop(Reject why, std::string detail) {
    if (!stopped) {
        stopped = why;
        stop_detail = std::move(detail);
    }
}

/// Notes this thread's faults; reads of the walkers of this rebuild are served as zero.
void PrecompileBoot::Watch() {
    FaultState().Watch({&walker_code, 1});
}

/// Whether the entry stopped or a read outside the recorded words faulted.
bool PrecompileBoot::Failed() const {
    return stopped || FaultState().faults != 0;
}

/// Up to MaxDetails lines per reason.
void PrecompileBoot::Detail(Reject why, const std::string& text) {
    if (rejects[size_t(why)] <= MaxDetails) {
        std::printf("Precompile boot: %s\n", text.c_str());
    }
}

void PrecompileBoot::Report() const {
    std::string reasons;
    u32 rejected = 0;
    for (size_t i = 0; i < rejects.size(); ++i) {
        rejected += rejects[i];
        if (rejects[i]) {
            reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ", rejects[i],
                                   RejectNames[i]);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    std::printf("%s\n",
                fmt::format("Precompile boot: {} entries, {} translated, {} programs, {} "
                            "permutations written, {} rejected{}, {} ms, peak memory {} MiB",
                            list.keys.size(), translated, programs.size(), permutations,
                            rejected, reasons.empty() ? "" : " (" + reasons + ")", ms,
                            PeakMemoryMiB())
                    .c_str());
}

PipelineCache::Result PipelineCache::BootProgram(PipelineSelection& sel, HwStage hw_stage,
                                                 SwStage sw_stage,
                                                 const Shader::ShaderParams& params,
                                                 Shader::Backend::Bindings& binding,
                                                 const Shader::RuntimeInfo& runtime_info) {
    return sel.boot->Program(params, hw_stage, sw_stage, binding, runtime_info);
}

bool PipelineCache::RebuildFromPrecompileList() {
    const char* boot = std::getenv("GOW3_PRECOMPILE_BOOT");
    const char* path = std::getenv("GOW3_PRECOMPILE_LIST");
    if (!boot || boot[0] != '1' || !path || !path[0]) {
        return false;
    }
#ifndef __linux__
    std::printf("Precompile boot: not supported on this platform\n");
    return false;
#else
    auto parsed = LoadListFile(path, MaxEntries);
    if (!parsed || !SetUpFaultGuard()) {
        return false;
    }
    auto boot_run = std::make_unique<PrecompileBoot>(*this, *parsed);
    boot_run->Run();
    return true;
#endif
}

} // namespace Vulkan
