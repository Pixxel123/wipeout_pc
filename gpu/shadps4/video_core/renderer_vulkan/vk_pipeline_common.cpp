// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_wave_stats.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <array>
#include <chrono>
#include "gow3_toggles.h"

namespace Vulkan {

namespace {
// gow3 (GOW3_FRAME_STATS): how draws and dispatches bind descriptors, to size a change that
// skips repeated pushes or batches set updates. Collected only with the stats on.
struct DescriptorStats {
    std::mutex mutex;
    u64 binds = 0, pushes = 0, set_updates = 0, descriptors = 0;
    u64 same_layout_and_writes = 0; ///< as the previous bind of this bind point: skippable
    u64 same_writes = 0;            ///< same contents, any layout: an upper bound
    std::array<u64, 2> last_hash{};
    std::array<VkPipelineLayout, 2> last_layout{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
DescriptorStats& Stats() {
    static DescriptorStats stats;
    return stats;
}
template <typename T>
u64 HandleBits(T handle) {
    return reinterpret_cast<u64>(static_cast<typename T::CType>(handle));
}
u64 HashWrites(const std::vector<vk::WriteDescriptorSet>& writes, u64& descriptors) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&h](u64 v) { h = (h ^ v) * 1099511628211ull; };
    for (const auto& w : writes) {
        mix(w.dstBinding);
        mix(w.dstArrayElement);
        mix(w.descriptorCount);
        mix(static_cast<u64>(w.descriptorType));
        descriptors += w.descriptorCount;
        for (u32 i = 0; i < w.descriptorCount; ++i) {
            if (w.pImageInfo) {
                mix(HandleBits(w.pImageInfo[i].sampler));
                mix(HandleBits(w.pImageInfo[i].imageView));
                mix(static_cast<u64>(w.pImageInfo[i].imageLayout));
            }
            if (w.pBufferInfo) {
                mix(HandleBits(w.pBufferInfo[i].buffer));
                mix(w.pBufferInfo[i].offset);
                mix(w.pBufferInfo[i].range);
            }
            if (w.pTexelBufferView) {
                mix(HandleBits(w.pTexelBufferView[i]));
            }
        }
    }
    return h;
}
void NoteBind(const std::vector<vk::WriteDescriptorSet>& writes, vk::PipelineLayout layout,
              bool compute, bool push) {
    auto& st = Stats();
    std::scoped_lock lk{st.mutex};
    u64 descriptors = 0;
    const u64 hash = HashWrites(writes, descriptors);
    const u32 point = compute ? 1 : 0;
    const auto vk_layout = static_cast<VkPipelineLayout>(layout);
    ++st.binds;
    ++(push ? st.pushes : st.set_updates);
    st.descriptors += descriptors;
    if (hash == st.last_hash[point]) {
        ++st.same_writes;
        if (vk_layout == st.last_layout[point]) {
            ++st.same_layout_and_writes;
        }
    }
    st.last_hash[point] = hash;
    st.last_layout[point] = vk_layout;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds >= 5.0) {
        const double b = std::max<double>(1.0, double(st.binds));
        std::printf("Descriptor binds: %.0f/s (pushes %.0f/s, set updates %.0f/s), %.1f descriptors "
                    "a bind; repeats of the previous bind: same layout and writes %.1f%%, same "
                    "writes %.1f%%\n",
                    st.binds / seconds, st.pushes / seconds, st.set_updates / seconds,
                    st.descriptors / b, 100.0 * st.same_layout_and_writes / b,
                    100.0 * st.same_writes / b);
        st.binds = st.pushes = st.set_updates = st.descriptors = 0;
        st.same_layout_and_writes = st.same_writes = 0;
        st.window = now;
    }
}
} // namespace

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }
    if (Gow3Stats::enabled) {
        NoteBind(set_writes, layout, IsCompute(), uses_push_descriptors);
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

// gow3: one histogram for every pipeline of the process, reported to last_run.log on news and
// every 256 pipelines (vk_wave_stats.h). AMD reports width 0 for graphics executables, so on
// AMD the key also carries the physical VGPR count from VK_AMD_shader_info (it differs between
// wave32 and wave64), and the ISA of the first executables of each stage goes to
// <shaders>/wave_dumps/ (wave32 code manipulates EXEC with the _b32 forms).
void Pipeline::RecordWaveSizes() const {
    if (!instance.IsPipelineExecutableInfoEnabled() || !pipeline) {
        return;
    }
    const auto [result, executables] = instance.GetDevice().getPipelineExecutablePropertiesKHR(
        vk::PipelineInfoKHR{.pipeline = *pipeline});
    if (result != vk::Result::eSuccess) {
        return;
    }
    static std::mutex mutex;
    static WaveSizeHistogram histogram;
    std::scoped_lock lock{mutex};
    bool news = false;
    for (const vk::PipelineExecutablePropertiesKHR& executable : executables) {
        const std::string stages = vk::to_string(executable.stages);
        std::string key = stages;
        // One stage bit per query: the last stage of the executable (vk_wave_stats.h).
        const std::uint32_t stage_bit = AmdShaderInfoStageBit(
            is_compute ? static_cast<std::uint32_t>(vk::ShaderStageFlagBits::eCompute)
                       : static_cast<std::uint32_t>(executable.stages));
        if (instance.IsAmdShaderInfoEnabled() && stage_bit != 0) {
            const auto stage = static_cast<vk::ShaderStageFlagBits>(stage_bit);
            const auto [stats_result, stats] = instance.GetDevice().getShaderInfoAMD(
                *pipeline, stage, vk::ShaderInfoTypeAMD::eStatistics);
            std::string stats_line = "stats unavailable";
            std::optional<std::uint32_t> vgprs;
            if (stats_result == vk::Result::eSuccess &&
                stats.size() >= sizeof(vk::ShaderStatisticsInfoAMD)) {
                vk::ShaderStatisticsInfoAMD info{};
                std::memcpy(&info, stats.data(), sizeof(info));
                vgprs = info.numPhysicalVgprs;
                key += "/vgpr" + std::to_string(info.numPhysicalVgprs);
                stats_line = "physical vgprs " + std::to_string(info.numPhysicalVgprs) +
                             ", available " + std::to_string(info.numAvailableVgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedVgprs) +
                             "; physical sgprs " + std::to_string(info.numPhysicalSgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedSgprs) +
                             "; lds " + std::to_string(info.resourceUsage.ldsUsageSizeInBytes) +
                             "; workgroup " + std::to_string(info.computeWorkGroupSize[0]) +
                             "x" + std::to_string(info.computeWorkGroupSize[1]) + "x" +
                             std::to_string(info.computeWorkGroupSize[2]);
            }
            if (histogram.ShouldDump(key, 3)) {
                const auto [isa_result, isa] = instance.GetDevice().getShaderInfoAMD(
                    *pipeline, stage, vk::ShaderInfoTypeAMD::eDisassembly);
                if (isa_result == vk::Result::eSuccess && !isa.empty()) {
                    const auto dir =
                        Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "wave_dumps";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    const std::string name = WaveDumpFileName(
                        stages, vgprs, histogram.DumpsOf(key), executable.subgroupSize);
                    std::ofstream out(dir / name, std::ios::binary);
                    out << "key " << key << "\n" << stats_line << "\n";
                    out.write(reinterpret_cast<const char*>(isa.data()),
                              static_cast<std::streamsize>(isa.size()));
                }
            }
        }
        news |= histogram.Add(std::move(key), executable.subgroupSize);
    }
    if (histogram.EndPipeline(news)) {
        std::printf("GPU: wave sizes after %u pipelines: %s\n", histogram.Pipelines(),
                    histogram.Summary().c_str());
    }
}

} // namespace Vulkan
