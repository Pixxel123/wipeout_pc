// SPDX-License-Identifier: GPL-2.0-or-later
// Which shader stages may contract float operations under GOW3_FAST_SHADER_MATH (fast_math.h).
// No game or Vulkan device needed.
#include <cassert>
#include <cstdio>

#include "shader_recompiler/backend/spirv/fast_math.h"

using Shader::SwStage;
using Shader::Backend::SPIRV::AllowsContraction;

int main() {
    // Off (the default): every stage keeps NoContraction, as before.
    for (const auto stage : {SwStage::Fragment, SwStage::Compute, SwStage::Vertex, SwStage::Geometry,
                             SwStage::TessellationControl, SwStage::TessellationEval}) {
        assert(!AllowsContraction(false, stage));
    }
    // On: pixel and compute shaders may fuse; the stages before rasterization keep it, so
    // positions stay bit-identical across pipelines (depth pre-pass plus an EQUAL depth test).
    assert(AllowsContraction(true, SwStage::Fragment));
    assert(AllowsContraction(true, SwStage::Compute));
    assert(!AllowsContraction(true, SwStage::Vertex));
    assert(!AllowsContraction(true, SwStage::Geometry));
    assert(!AllowsContraction(true, SwStage::TessellationControl));
    assert(!AllowsContraction(true, SwStage::TessellationEval));
    std::puts("fast shader math: ok");
}
