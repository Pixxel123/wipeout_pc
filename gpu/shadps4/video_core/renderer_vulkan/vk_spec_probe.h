// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shader_recompiler/specialization.h"

namespace Vulkan::SpecProbe {

// gow3: the elements StageSpecialization's constructor builds per bound resource
// (shader_recompiler/specialization.h), repeated here instead of shared: any edit under
// shader_recompiler/ changes the shader build stamp, and every saved shader is dropped once.
// spec-match-test checks Matches against the constructor, so a change there that is not copied
// here fails the test.

inline Shader::VsAttribSpecialization MakeVsAttrib(const Shader::Gcn::VertexAttribute& desc,
                                                   const AmdGpu::Buffer& sharp,
                                                   const Shader::RuntimeInfo& runtime_info) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    Shader::VsAttribSpecialization spec{};
    if (const auto step_rate = desc.GetStepRate(); step_rate != InstanceIdType::None) {
        spec.divisor = step_rate == InstanceIdType::OverStepRate0
                           ? runtime_info.sw.vs.step_rate_0
                           : (step_rate == InstanceIdType::OverStepRate1
                                  ? runtime_info.sw.vs.step_rate_1
                                  : 1);
    }
    spec.num_class = AmdGpu::GetNumberClass(sharp.GetNumberFmt());
    spec.dst_select = sharp.DstSelect();
    return spec;
}

inline Shader::BufferSpecialization MakeBuffer(const Shader::BufferResource& desc,
                                               const AmdGpu::Buffer& sharp) {
    Shader::BufferSpecialization spec{};
    spec.stride = sharp.GetStride();
    spec.is_formatted = desc.is_formatted;
    spec.swizzle_enable = sharp.swizzle_enable;
    if (spec.is_formatted) {
        spec.data_format = static_cast<u32>(sharp.GetDataFmt());
        spec.num_format = static_cast<u32>(sharp.GetNumberFmt());
        spec.dst_select = sharp.DstSelect();
        spec.num_conversion = sharp.GetNumberConversion();
    }
    if (spec.swizzle_enable) {
        spec.index_stride = sharp.index_stride;
        spec.element_size = sharp.element_size;
    }
    return spec;
}

inline Shader::ImageSpecialization MakeImage(const Shader::ImageResource& desc,
                                             const AmdGpu::Image& sharp,
                                             const Shader::Info& info) {
    Shader::ImageSpecialization spec{};
    spec.type = sharp.GetViewType(desc.is_array);
    spec.is_integer = AmdGpu::IsInteger(sharp.GetNumberFmt());
    spec.is_storage = desc.is_written;
    spec.is_cube = sharp.IsCube();
    if (spec.is_storage) {
        spec.dst_select = sharp.DstSelect();
    } else {
        spec.is_srgb = sharp.GetNumberFmt() == AmdGpu::NumberFormat::Srgb;
    }
    spec.num_conversion = sharp.GetNumberConversion();
    spec.num_bindings = desc.NumBindings(info);
    return spec;
}

inline Shader::FMaskSpecialization MakeFMask(const AmdGpu::Image& sharp) {
    Shader::FMaskSpecialization spec{};
    spec.width = sharp.width;
    spec.height = sharp.height;
    return spec;
}

inline Shader::SamplerSpecialization MakeSampler(const AmdGpu::Sampler& sharp) {
    Shader::SamplerSpecialization spec{};
    spec.force_unnormalized = sharp.force_unnormalized;
    spec.force_degamma = sharp.force_degamma;
    return spec;
}

/**
 * gow3: whether `stored == StageSpecialization(info, runtime_info, profile, start)`, without
 * building that specialization (two runtime-info copies, the fetch data, one element per
 * resource). It reads the sharps the constructor reads and compares them as operator== does,
 * with the stored specialization on the left. False also means "not known": holes, tessellation
 * stages and resource lists whose sizes differ from the stored ones take the full path.
 */
[[nodiscard]] inline bool Matches(const Shader::StageSpecialization& stored,
                                  const Shader::Info& info,
                                  const Shader::RuntimeInfo& runtime_info,
                                  const Shader::Backend::Bindings& start) {
    if (!stored.Valid()) {
        return false;
    }
    // The constructor rewrites the runtime info from guest memory for these stages.
    if (info.sw_stage == Shader::SwStage::TessellationControl ||
        info.sw_stage == Shader::SwStage::TessellationEval) {
        return false;
    }
    if (stored.runtime_info != runtime_info) {
        return false;
    }

    // The fetch data compares through VertexAttribute::operator==, which ignores the fields
    // GetSharp applies (inst_offset, data_format, num_format). The attributes read below are
    // therefore the fresh parse, as in the constructor, never the stored ones.
    const auto fetch = Shader::Gcn::ParseFetchShader(info);
    if (stored.fetch_shader_data != fetch) {
        return false;
    }
    if (info.sw_stage == Shader::SwStage::Vertex && fetch) {
        const auto& attributes = fetch->attributes;
        if (stored.vs_attribs.size() != attributes.size()) {
            return false;
        }
        for (size_t i = 0; i < attributes.size(); ++i) {
            const auto sharp = attributes[i].GetSharp(info);
            const auto spec = sharp ? MakeVsAttrib(attributes[i], sharp, runtime_info)
                                    : Shader::VsAttribSpecialization{};
            if (stored.vs_attribs[i] != spec) {
                return false;
            }
        }
    } else if (!stored.vs_attribs.empty()) {
        return false;
    }

    if (stored.buffers.size() != info.buffers.size() ||
        stored.images.size() != info.images.size() ||
        stored.fmasks.size() != info.fmasks.size() ||
        stored.samplers.size() != info.samplers.size()) {
        return false;
    }
    // operator== compares buffers and images only where the right-hand (new) specialization has
    // a bound sharp, so this draw's sharps gate the compares. A mismatch returns at once: its
    // bit is set, so the shortcut below cannot apply.
    bool any_bound = false;
    for (size_t i = 0; i < stored.buffers.size(); ++i) {
        const auto& desc = info.buffers[i];
        if (const auto sharp = desc.GetSharp(info)) {
            any_bound = true;
            if (stored.buffers[i] != MakeBuffer(desc, sharp)) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < stored.images.size(); ++i) {
        const auto& desc = info.images[i];
        if (const auto sharp = desc.GetSharp(info)) {
            any_bound = true;
            if (stored.images[i] != MakeImage(desc, sharp, info)) {
                return false;
            }
        }
    }
    // FMasks compare in full, and a bound one counts for the shortcut.
    for (size_t i = 0; i < stored.fmasks.size(); ++i) {
        const auto sharp = info.fmasks[i].GetSharp(info);
        any_bound |= static_cast<bool>(sharp);
        const auto spec = sharp ? MakeFMask(sharp) : Shader::FMaskSpecialization{};
        if (stored.fmasks[i] != spec) {
            return false;
        }
    }

    // operator=='s shortcut for stages with no bindings, taken before the start bindings.
    if (stored.bitset.none() && !any_bound) {
        return true;
    }
    if (stored.start != start) {
        return false;
    }
    for (size_t i = 0; i < stored.samplers.size(); ++i) {
        const auto sharp = info.samplers[i].GetSharp(info);
        const auto spec = sharp ? MakeSampler(sharp) : Shader::SamplerSpecialization{};
        if (stored.samplers[i] != spec) {
            return false;
        }
    }
    return true;
}

} // namespace Vulkan::SpecProbe
