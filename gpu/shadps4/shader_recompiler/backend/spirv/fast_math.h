// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_FAST_SHADER_MATH=1 (Profile::fast_shader_math), backported from shadPS4 #5455: shaders
// no longer request SignedZeroInfNanPreserve, and pixel and compute shaders may contract float
// operations (fused multiply-add). Off by default: a game that relies on inf/NaN or exact rounding
// can render differently.
#pragma once

#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

/// Whether float operations of `stage` may be contracted. The stages before rasterization keep
/// NoContraction, so positions stay bit-identical across pipelines (a depth pre-pass followed by
/// an EQUAL depth test must see the same values).
[[nodiscard]] constexpr bool AllowsContraction(bool fast_shader_math, SwStage stage) noexcept {
    return fast_shader_math && (stage == SwStage::Fragment || stage == SwStage::Compute);
}

} // namespace Shader::Backend::SPIRV
