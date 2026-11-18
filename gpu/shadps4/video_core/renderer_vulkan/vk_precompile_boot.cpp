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
// An assert in the translator ends the game instead: a marker file beside the list names the
// entry being translated, and the next start skips the entries it names (CrashMarker).

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <fmt/format.h>
#ifdef __linux__
#include <sys/resource.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
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

constexpr u32 MaxDetails = 8;        ///< detail lines per reason
constexpr auto ProgressInterval = std::chrono::seconds(1);
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
    StrayFetch,        ///< the fetch shader is read through a pointer the recording did not
    Exception,         ///< the translation threw
    TooManyPermutations,
    Count,
};

constexpr std::array<const char*, size_t(Reject::Count)> RejectNames = {
    "not vertex + fragment", "no code", "no room", "faulted", "not selected", "spec changed",
    "walk outside the recording", "fetch outside the recording", "exception",
    "too many permutations",
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
struct TranslatedProgram {
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

/// The crash-loop guard: `<list>.boot` names, one per line, the programs of the entries a start
/// ended in, plus those of the entry being translated. A translator assert exits the game
/// (_Exit) and the disk marker is saved only after the rebuild, so the next start rebuilds
/// again and skips every entry that uses one of those programs: an assert is most likely
/// the program's, whichever entry reached it.
class CrashMarker {
public:
    explicit CrashMarker(std::string path_) : path{std::move(path_)} {
        if (std::FILE* file = std::fopen(path.c_str(), "r")) {
            for (unsigned long long hash; std::fscanf(file, "%llx", &hash) == 1;) {
                earlier.insert(hash);
            }
            std::fclose(file);
        }
    }

    [[nodiscard]] const std::string& Path() const {
        return path;
    }
    /// Programs an earlier start ended in.
    [[nodiscard]] size_t Earlier() const {
        return earlier.size();
    }

    /// Whether the entry uses a program an earlier start ended in.
    [[nodiscard]] bool Skip(const RecordedKey& key) const {
        return std::ranges::any_of(key.stages, [&](const RecordedStage& stage) {
            return earlier.contains(stage.hash);
        });
    }

    /// Names the entry's programs (none for null) as being translated, before it starts;
    /// false when the file cannot be written.
    [[nodiscard]] bool Enter(const RecordedKey* key) {
        std::FILE* file = std::fopen(path.c_str(), "w");
        if (!file) {
            return false;
        }
        bool written = true;
        const auto put = [&](u64 hash) {
            written &= std::fprintf(file, "%016llx\n", static_cast<unsigned long long>(hash)) > 0;
        };
        std::ranges::for_each(earlier, put);
        if (key) {
            for (const auto& stage : key->stages) {
                put(stage.hash);
            }
        }
        return std::fclose(file) == 0 && written;
    }

    /// The rebuild ended: nothing to skip next time.
    void Finish() const {
        std::remove(path.c_str());
    }

private:
    std::string path;
    std::set<u64> earlier;
};

u64 PeakMemoryMiB() {
#ifdef __linux__
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? u64(usage.ru_maxrss) >> 10 : 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
               ? u64(counters.PeakWorkingSetSize) >> 20
               : 0;
#else
    return 0;
#endif
}

} // namespace

struct PrecompileBoot {
    PrecompileBoot(PipelineCache& cache_, const ParsedList& list_, std::string marker_path)
        : cache{cache_}, list{list_}, marker{std::move(marker_path)} {}

    bool Run();
    PipelineCache::Result Program(const Shader::ShaderParams& params, HwStage hw_stage,
                                  SwStage sw_stage, Shader::Backend::Bindings& binding,
                                  const Shader::RuntimeInfo& runtime_info);

private:
    void Entry(u32 index, const RecordedKey& key);
    std::optional<Reject> Select(const RecordedKey& key, Shadow& shadow);
    bool MapShadow(const RecordedKey& key, Shadow& shadow);
    std::unique_ptr<AmdGpu::Regs> Registers(const RecordedKey& key, const Shadow& shadow) const;
    const RecordedStage* StageOf(u64 hash);
    bool CheckWalk(const Shader::Info& info, u64 hash);
    bool CheckFetch(const Shader::Info& info, const Shader::StageSpecialization& spec, u64 hash);
    std::optional<size_t> Translate(TranslatedProgram& program,
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
    std::unordered_map<u64, TranslatedProgram> programs; ///< node-based: specs point at their Info
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

    CrashMarker marker;
    u32 translated = 0, permutations = 0, skipped = 0;
    std::array<u32, size_t(Reject::Count)> rejects{};
    std::chrono::steady_clock::time_point started;
};

/// False when the crash marker cannot be written: then nothing is translated.
bool PrecompileBoot::Run() {
    started = std::chrono::steady_clock::now();
    if (!marker.Enter(nullptr)) {
        std::printf("Precompile boot: cannot write %s (%s), rebuild off\n", marker.Path().c_str(),
                    std::strerror(errno));
        return false;
    }
    if (marker.Earlier()) {
        std::printf("Precompile boot: an earlier start ended in the rebuild, entries using the %zu "
                    "programs %s names are skipped\n",
                    marker.Earlier(), marker.Path().c_str());
    }
    // Every walker translated from here on is generated after the current end of the walker
    // code, inside the generator's 32 MiB. Its own bounds are private to the recompiler: the
    // range below overshoots the generator's end by what it held before, nothing at start.
    const u64 walkers = u64(Shader::RegisterWalkerCode(nullptr, 0));
    walker_code = {walkers, walkers + WalkerCodeSize};
    walk_words.reserve(MaxRecordedWords);
    walk_pointers.reserve(MaxRecordedPointers);
    walk_roots.reserve(ListUserDataWords);
    const u32 count = u32(list.keys.size()); // at most MaxListEntries (LoadListFile)
    std::printf("Precompile boot: translating %u entries into the shader store\n", count);
    auto last_progress = started;
    for (u32 index = 0; index < count; ++index) {
        if (const auto now = std::chrono::steady_clock::now();
            now - last_progress >= ProgressInterval) {
            last_progress = now;
            std::printf("Precompile boot: %u of %u entries, %u permutations written\n", index,
                        count, permutations);
        }
        const auto& key = list.keys[index];
        if (marker.Skip(key)) {
            if (++skipped <= MaxDetails) {
                std::printf("Precompile boot: entry %u (recorded key %016llx) skipped (crash "
                            "marker)\n",
                            index, static_cast<unsigned long long>(key.key_hash));
            }
            continue;
        }
        if (!marker.Enter(&key)) {
            std::printf("Precompile boot: cannot write %s (%s), rebuild stopped at entry %u\n",
                        marker.Path().c_str(), std::strerror(errno), index);
            break;
        }
        Entry(index, key);
    }
    marker.Finish();
    Report();
    return true;
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
    // Checked on load (ValidSgprOffset): the u16 keeps the whole offset.
    selection.draw_indirect_params = {u16(key.vertex_sgpr_offset), key.instance_sgpr_offset};
    bool selected = false;
    Watch();
    const bool completed = Guarded([&] {
        // Caught here, inside the recovery point: the shader compiler's jobs catch them too.
        try {
            selected = cache.RefreshGraphicsKey(selection);
        } catch (const std::exception& exception) {
            Stop(Reject::Exception, exception.what());
        } catch (...) {
            Stop(Reject::Exception, "unknown");
        }
    });
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
        // A code region is only marked out here (its first and last word) and copied in after
        // the fill. Padded to whole pages: the program address register holds bits 8 and up,
        // and a region ends right before its guard page.
        const size_t bytes = list.code.at(stage.hash).size() * sizeof(u32);
        const size_t padded = (bytes + PageSize - 1) / PageSize * PageSize;
        shadow.words.push_back({CodeBase(stage.sw_stage), 0, 0});
        shadow.words.push_back({CodeBase(stage.sw_stage), u32(padded - 4), 0});
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
    for (const auto& stage : key.stages) {
        const auto& code = list.code.at(stage.hash);
        const u64 at = *shadow.layout->Translate(CodeBase(stage.sw_stage), *address);
        std::memcpy(reinterpret_cast<void*>(at), code.data(), code.size() * sizeof(u32));
    }
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
        perm = Translate(program, params, hw_stage, sw_stage, binding, runtime_info, nullptr);
        if (!perm) {
            programs.erase(it);
            return {};
        }
    } else {
        // As GetProgram looks a permutation up: the first Info with this draw's user data.
        program.info.pgm_base = params.Base();
        program.info.user_data = params.user_data;
        program.info.RefreshFlatBuf();
        if (Failed() || !CheckWalk(program.info, params.hash)) {
            return {};
        }
        const Shader::StageSpecialization spec{program.info, runtime_info, cache.profile,
                                               binding};
        if (!CheckFetch(program.info, spec, params.hash)) {
            return {};
        }
        const auto found = std::ranges::find(program.specs, spec);
        perm = found != program.specs.end()
                   ? std::optional{size_t(std::distance(program.specs.begin(), found))}
                   : Translate(program, params, hw_stage, sw_stage, binding, runtime_info,
                               &spec);
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
/// writes it; its index, or none when the entry stops. `lookup` is the spec a later
/// permutation was looked up with, null for the program's first translation.
std::optional<size_t> PrecompileBoot::Translate(TranslatedProgram& program,
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
    if (!CheckFetch(info, spec, params.hash)) {
        return std::nullopt;
    }
    auto emit_binding = binding;
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, ir, emit_binding);
    if (Failed()) {
        return std::nullopt;
    }
    // The race looks this permutation up with the program's first Info and a fresh runtime
    // info: a spec that differs from that lookup would never be found.
    const auto expected =
        lookup ? *lookup : Shader::StageSpecialization{info, runtime_info, profile, binding};
    if (!(spec == expected)) {
        Stop(Reject::SpecChanged, fmt::format("program {:016x} permutation {}", params.hash,
                                              perm));
        return std::nullopt;
    }
    if (!lookup) {
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
/// follows a pointer the recording did not, or when the walk through the shadow copy (the
/// flattened user data `info` holds) read other values. The shadow copy cannot tell by itself:
/// its gaps read as zero and a pointer it does not lead in still holds a guest address, both
/// without a fault; a word recorded through one table but read through another, or half of a
/// pointer read as a plain word (repointed in the copy), reads other values there.
bool PrecompileBoot::CheckWalk(const Shader::Info& info, u64 hash) {
    if (!info.srt_info.walker_func) {
        return true;
    }
    const auto* stage = StageOf(hash);
    if (!stage) {
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
    // Past the user data, which the shadow walk read with its pointers repointed.
    const auto& walked = info.flattened_ud_buf;
    if (!why && (walked.size() != flat.size() || flat.size() < ListUserDataWords ||
                 !std::equal(flat.begin() + ListUserDataWords, flat.end(),
                             walked.begin() + ListUserDataWords))) {
        why = "the shadow copy gave other values than the recording";
    }
    if (why) {
        Stop(Reject::StrayWalk, fmt::format("program {:016x}: {}", hash, why));
    }
    return !why;
}

/// The entry's recorded stage of a program; null (the entry stops) when there is none.
const RecordedStage* PrecompileBoot::StageOf(u64 hash) {
    const auto stage = std::ranges::find(entry->stages, hash, &RecordedStage::hash);
    if (stage == entry->stages.end()) {
        Stop(Reject::StrayWalk, fmt::format("program {:016x} is not recorded", hash));
        return nullptr;
    }
    return &*stage;
}

/// Whether the fetch shader and its vertex buffer tables were read through user-data pointers
/// the recording followed (false: the entry stops). Any other pair of user-data words still
/// holds a guest address, which the committed guest pool serves without a fault.
bool PrecompileBoot::CheckFetch(const Shader::Info& info, const Shader::StageSpecialization& spec,
                                u64 hash) {
    if (!info.has_fetch_shader) {
        return true;
    }
    const auto* stage = StageOf(hash);
    if (!stage) {
        return false;
    }
    const auto followed = [&](u32 sgpr) {
        return std::ranges::any_of(stage->roots,
                                   [&](const auto& root) { return root.first == sgpr; });
    };
    bool recorded = followed(info.fetch_shader_sgpr_base);
    if (spec.fetch_shader_data) {
        for (const auto& attrib : spec.fetch_shader_data->attributes) {
            recorded = recorded && followed(attrib.sgpr_base);
        }
    }
    if (!recorded) {
        Stop(Reject::StrayFetch, fmt::format("program {:016x}", hash));
    }
    return recorded;
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
                            "permutations written, {} rejected{}, {} skipped (crash marker), "
                            "{} ms, peak memory {} MiB",
                            list.keys.size(), translated, programs.size(), permutations,
                            rejected, reasons.empty() ? "" : " (" + reasons + ")", skipped, ms,
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
    const char* path = std::getenv("GOW3_PRECOMPILE_LIST");
    if (!BootRequested() || !path || !path[0]) {
        return false;
    }
#if !defined(__linux__) && !defined(_WIN32)
    std::printf("Precompile boot: not supported on this platform\n");
    return false;
#else
    auto parsed = LoadListFile(path, MaxListEntries);
    if (!parsed || !SetUpFaultGuard()) {
        return false;
    }
    auto boot_run = std::make_unique<PrecompileBoot>(*this, *parsed, std::string{path} + ".boot");
    return boot_run->Run();
#endif
}

} // namespace Vulkan
