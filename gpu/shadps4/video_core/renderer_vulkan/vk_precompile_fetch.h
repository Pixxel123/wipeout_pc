// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_*): a fetch shader's parse as a precompile list holds it
// (FetchDescription, vk_precompile_list.h) and as the translator reads it (FetchShaderData):
// the recorder writes the one, the rebuild hands the other to ParseFetchShader.

#pragma once

#include <optional>
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"

namespace Vulkan::Precompile {

/// `data`, the parse of the fetch shader whose pointer is at user-data register `sgpr_base`.
[[nodiscard]] inline FetchDescription DescribeFetch(const Shader::Gcn::FetchShaderData& data,
                                                    u32 sgpr_base) {
    FetchDescription fetch{data.size, data.vertex_offset_sgpr, data.instance_offset_sgpr,
                           u8(sgpr_base), {}};
    for (const auto& a : data.attributes) {
        fetch.attributes.push_back({a.semantic, a.dest_vgpr, a.num_elements, a.sgpr_base,
                                    a.dword_offset, a.instance_data, a.inst_offset,
                                    a.data_format, a.num_format});
    }
    return fetch;
}

/// What ParseFetchShader gives for the key's vertex program while the rebuild replays it; none
/// for a key without a parse (no fetch shader, or a version 3 list).
[[nodiscard]] inline std::optional<Shader::Gcn::FetchShaderOverride> FetchOverrideOf(
    const RecordedKey& key) {
    if (!key.fetch) {
        return std::nullopt;
    }
    const auto vertex = std::ranges::find(key.stages, ListVertexStage, &RecordedStage::sw_stage);
    if (vertex == key.stages.end()) { // rejected on load (CheckFetch)
        return std::nullopt;
    }
    const auto& fetch = *key.fetch;
    Shader::Gcn::FetchShaderOverride parse{vertex->hash, fetch.sgpr_base, {}};
    parse.data.size = fetch.size;
    parse.data.vertex_offset_sgpr = fetch.vertex_offset_sgpr;
    parse.data.instance_offset_sgpr = fetch.instance_offset_sgpr;
    for (const auto& a : fetch.attributes) {
        auto& attribute = parse.data.attributes.emplace_back();
        attribute.semantic = a.semantic;
        attribute.dest_vgpr = a.dest_vgpr;
        attribute.num_elements = a.num_elements;
        attribute.sgpr_base = a.sgpr_base;
        attribute.dword_offset = a.dword_offset;
        attribute.instance_data = a.instance_data;
        attribute.inst_offset = a.inst_offset;
        attribute.data_format = a.data_format;
        attribute.num_format = a.num_format;
    }
    return parse;
}

} // namespace Vulkan::Precompile
