// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <span>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/cache_pack.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace PipelineBinaryCache {
using Key = std::vector<u8>;
struct Binary {
    Key key;
    std::vector<u8> data;
};
/// gow3: the pack's limit on one entry, so a binary the pack would refuse is refused here too.
constexpr size_t MaxBytes = Storage::CachePack::MaxBlobBytes;
constexpr u32 Magic = 0x31425047, Version = 1; // "GPB1", little-endian on Windows and Linux
constexpr u32 MaxBinaries = 32;

std::vector<u8> Encode(const Key& global, const Key& pipeline, u64 graphics_hash,
                       const std::vector<Binary>& binaries);
std::optional<std::vector<Binary>> Decode(std::span<const u8> bytes, const Key& global,
                                          const Key& pipeline, u64 graphics_hash);
/// True when `bytes` is an encoded binary saved under `global`.
bool SavedUnder(std::span<const u8> bytes, const Key& global);
} // namespace PipelineBinaryCache

/// `unoptimized`, when given, is set when a PipelineBuild::Fast build compiled without driver
/// optimisations (a binary cache miss); such a pipeline is never stored in the cache.
/// PipelineBuild::RestoreOnly restores a saved binary or returns vk::Result::ePipelineCompileRequired
/// without compiling anything: the binary cache is off, the key is invalid, nothing is saved, or
/// the saved binary is refused. A restore counts as a hit, a miss is left for the caller to count.
vk::ResultValue<vk::UniquePipeline> CreateCachedGraphicsPipeline(
    vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
    const GraphicsPipelineKey& key, const vk::PipelineBinaryKeyKHR& global_key,
    PipelineBuild build = PipelineBuild::Optimized, bool* unoptimized = nullptr);
void ReportPipelineBinaryCache();
/// Removes every PipelineBinary blob not saved under `global`; returns how many.
size_t DropForeignPipelineBinaries(const PipelineBinaryCache::Key& global);

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);

} // namespace Vulkan
