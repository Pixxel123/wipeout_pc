// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <span>
#include <string>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_precompile_load.h"
#include "video_core/renderer_vulkan/vk_precompile_record.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_async_compiler.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;
struct AsyncShaderCompilation;
struct AsyncGraphicsCompilation;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< gow3: permutation of the previous lookup, compared first
    /// gow3: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;
    std::shared_ptr<AsyncShaderCompilation> pending;
    bool compilation_failed = false;
    std::vector<Shader::StageSpecialization> failed_permutations;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// gow3: whether the pixel shader runs once per sample: the pipeline asks for it (DB_EQAA
/// ps_iter_samples with sample inputs: sampleShadingEnable, and only then do sample
/// barycentrics and Sample-qualified inputs use SampleId or the Sample decoration), or the
/// shader reads the sample ID (SampleId). VK_NV_framebuffer_mixed_samples allows neither with
/// fewer colour than raster samples. `fs->loads` is empty for a shader from the disk cache.
bool FragmentShadesPerSample(const AmdGpu::Regs& regs, const Shader::Info* fs,
                             const Shader::Profile& profile);

/// gow3: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    bool compilation_pending = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
    /// gow3 (GOW3_PRECOMPILE_LOAD): with `worker`, on the GPU command thread: the lookup first
    /// reads a program missing from program_cache from the store (LoadDiskProgram). It still
    /// translates nothing: a missing program or permutation fails the selection.
    bool from_store = false;
    /// gow3: set: a start-up rebuild from a precompile list (vk_precompile_boot.cpp) translates
    /// the programs, not program_cache.
    struct PrecompileBoot* boot{};
};

/// gow3: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
    /// gow3 (GOW3_PRECOMPILE_LOAD_TRACE): why the lookup failed (a static text) and the
    /// SwStage it failed at, so the loader's trace can say which program or permutation it was.
    const char* failure = nullptr;
    u32 failed_stage = 0;
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    /// Loads the pipelines of earlier sessions. gow3: called by the presenter once the window
    /// can show a loading screen; `progress(done, total)` follows the pipelines read.
    void WarmUp(const std::function<void(u32 done, u32 total)>& progress = {});
    /// gow3: the same load on low-priority threads while the game runs; the GPU thread takes
    /// the results a few at a time (PumpWarmUp). False when it cannot (zip archive store).
    bool StartBackgroundWarmUp();
    void StartOnDemand();
    void Sync();
    void ReportCompilerStats();

    /// gow3: replaces fast first builds whose optimised rebuild is done (GPU command thread,
    /// only while the draw recording thread is idle) and queues the next rebuilds.
    void SwapRebuiltPipelines();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);

    /// gow3: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// gow3: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// gow3: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    /// gow3 diagnostic (GOW3_PRECOMPILE_RECORD): reports the recording every few seconds.
    /// GPU command thread, between submissions.
    void ReportPrecompile();
    /// gow3 diagnostic (GOW3_PRECOMPILE_LOAD): builds list entries while loading. GPU command
    /// thread, between submissions.
    void PumpPrecompileLoad();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool QueueShader(Program& program, Shader::HwStage stage, Shader::SwStage l_stage,
                     const Shader::ShaderParams& params, Shader::RuntimeInfo runtime_info,
                     Shader::Backend::Bindings binding, size_t permutation, bool initial,
                     std::optional<Shader::StageSpecialization> specialization = {});
    void PublishShaders(bool wait = false);
    void PublishGraphics(bool wait = false);
    void QueueRebuilds();
    void FinishCompilations();
    void SeedWorkerCaches();
    bool CheckCacheProfile();
    struct BackgroundWarmup;
    struct WarmStage;
    void ReadWarmEntry(BackgroundWarmup& warm, std::vector<u8>&& data);
    bool MergeWarmStage(WarmStage& stage);
    void PumpWarmUp();
    void StopWarmUp();
    bool AsyncCompilation() const;
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();
    friend struct PrecompileRecord;
    void CapturePrecompile();
    friend struct PrecompileBoot;
    /// gow3 diagnostic GOW3_PRECOMPILE_BOOT (vk_precompile_boot.cpp): translates a precompile
    /// list into the store after a recompiler change dropped its shaders; false when off.
    bool RebuildFromPrecompileList();
    Result BootProgram(PipelineSelection& sel, Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                       const Shader::ShaderParams& params, Shader::Backend::Bindings& binding,
                       const Shader::RuntimeInfo& runtime_info);
    friend struct PrecompileLoad;
    /// gow3 (GOW3_PRECOMPILE_LOAD): neither in use nor saved in the store, so the loader may
    /// translate it and number its permutations from 0. GPU command thread.
    [[nodiscard]] bool ProgramNeeded(u64 hash) const {
        return !program_cache.contains(hash) && !disk_permutations.contains(hash);
    }
    /// gow3 (GOW3_PRECOMPILE_LOAD): the Info GetProgram binds a program in use or saved with;
    /// false while there is none yet. GPU command thread.
    bool InUseInfo(u64 hash, Shader::Info& info);
    /// gow3 (GOW3_PRECOMPILE_LOAD): permutations the loader saved, for LoadDiskProgram;
    /// `saved` keeps those it indexed.
    void AddSavedPermutations(std::vector<std::pair<u64, u32>>& saved);
    /// gow3 (GOW3_PRECOMPILE_LOAD): GetProgram, a program not in program_cache; while loading
    /// the draw may wait for the loader here. Its pipeline selections may read programs from
    /// the store into program_cache.
    void PrecompileLoadFirstUse(u64 hash);
    /// gow3: hands the pipeline `sel` selected to the compile workers, as a draw's background
    /// build; null when their queue is full. `live`: the Infos the published pipeline binds,
    /// `loader`: the loader's build (AsyncGraphicsCompilation::loader).
    std::shared_ptr<AsyncGraphicsCompilation> StartGraphicsBuild(
        const PipelineSelection& sel, const std::array<const Shader::Info*, MaxShaderStages>& live,
        bool loader);
    /// gow3 (GOW3_PRECOMPILE_LOAD): the pipeline a loader selection (`worker`, `from_store`)
    /// found, handed to the compile workers unless it is built or being built. GPU command
    /// thread, while the selection's shadow copy is mapped: the build's snapshot reads it.
    /// `why`, when set, gets why it was dropped (GOW3_PRECOMPILE_LOAD_TRACE).
    Precompile::PipelineStep StartLoadPipeline(const PipelineSelection& selection,
                                               const PrepWorker& worker,
                                               const char** why = nullptr);
    /// gow3 (GOW3_PRECOMPILE_LOAD): whether every build the loader handed over is done by
    /// `until_ns` (CompilePolicy::Now), waiting for them until then.
    bool WaitLoadBuilds(u64 until_ns);
    /// gow3 (GOW3_PRECOMPILE_LOAD): builds the loader handed over that are not published yet.
    [[nodiscard]] size_t LoadBuildsRunning() const;
    /// gow3 (GOW3_PRECOMPILE_LOAD): a draw of a pipeline the loader is building waits for it
    /// while loading, from the loading state's hold budget; true when it is built.
    bool WaitLoadBuild(const AsyncGraphicsCompilation& build);
    /// gow3 (GOW3_PRECOMPILE_LOAD): a draw's pipeline miss, before it builds the pipeline
    /// itself: while loading, waits for the loader's pipelines not handed over or still
    /// building, from the loading state's hold budget; true when it held. False without it.
    bool PrecompileLoadHoldPipelines();
    /// gow3 (GOW3_PRECOMPILE_LOAD): `wait(left_ns)` while loading with hold budget left, the
    /// time it takes counted as held; false (no wait) otherwise, else what `wait` returned.
    bool PrecompileLoadWait(const std::function<bool(u64 left_ns)>& wait);
    /// gow3 (GOW3_PRECOMPILE_LOAD_TRACE): a draw starts its own build of the pipeline `sel`
    /// selected; with the trace a line on what the list has of its programs. GPU command thread.
    void PrecompileLoadTraceDraw();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    CompilerPool<Shader::Pools> compiler_pools{8};
    std::unique_ptr<AsyncCompiler> compiler, capture_compiler;
    std::vector<vk::UniquePipelineCache> worker_pipeline_caches;
    bool worker_caches_seeded = false;
    /// gow3: GOW3_PARALLEL_WARMUP: WarmUp builds the cached pipelines on these threads.
    struct WarmupPool;
    WarmupPool* warmup_pool = nullptr;
    std::mutex warmup_results_mutex;
    std::vector<std::pair<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>>> warmup_graphics;
    std::vector<std::pair<ComputePipelineKey, std::unique_ptr<ComputePipeline>>> warmup_compute;
    /// gow3: StartBackgroundWarmUp state; owned here, deleted by StopWarmUp.
    BackgroundWarmup* background = nullptr;
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<AsyncGraphicsCompilation>> pending_graphics;
    /// gow3: published fast builds whose optimised rebuild waits for the end of the cold burst
    /// (QueueRebuilds), then the queued ones until they are swapped in or dropped.
    std::vector<std::shared_ptr<AsyncGraphicsCompilation>> waiting_rebuilds, rebuilds;
    bool fast_first_builds = false; ///< gow3: first builds on the workers skip optimisations
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    std::vector<Program*> pending_programs;
    /// gow3: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start
    /// gow3 (Gow3Toggle::DiskShaders): with on-demand startup, a program missing from
    /// program_cache is first read from the store at its saved permutation indices, instead of
    /// being translated again.
    bool disk_shaders = false;
    bool disk_store_untrusted = false; ///< the bit went off live: the marker was reset
    std::unordered_map<u64, std::vector<u32>> disk_permutations; ///< pgm hash -> saved indices
    std::unordered_set<std::string> disk_pipeline_keys;          ///< "g_0x..."/"c_0x..." saved
    /// Loaded modules no draw has used yet (handles only, for the "used" count).
    std::unordered_set<vk::ShaderModule> disk_modules_unused;
    struct DiskShaderStats {
        u64 programs, permutations, rejected, matched, compiled, spirv_bytes, load_ns;
        u64 unused_peak; ///< most modules loaded and not used yet at once
        u64 keys_skipped, keys_written;
    } disk_stats{}, disk_reported{};
    std::chrono::steady_clock::time_point disk_report{};
    [[nodiscard]] bool DiskShadersActive() const;
    void IndexDiskShaders();
    bool LoadDiskProgram(u64 hash);
    /// Index for a new permutation: a slot the store left empty, else the next one.
    [[nodiscard]] size_t NewPermutationIndex(const Program& program) const;
    /// False when this pipeline key is already in the store (nothing to write).
    bool NewPipelineKey(const std::string& name);
    void ReportDiskShaders();

    PrecompileRecordPtr precompile; ///< gow3: the recorder, null unless GOW3_PRECOMPILE_RECORD
    PrecompileLoadPtr precompile_load; ///< gow3: the loader, null unless GOW3_PRECOMPILE_LOAD
    /// gow3 (GOW3_PRECOMPILE_LOAD): the loader's builds published and failed (PublishGraphics).
    u64 load_builds_published = 0, load_builds_failed = 0;

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

} // namespace Vulkan
