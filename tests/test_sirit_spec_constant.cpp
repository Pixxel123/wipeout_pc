// SPDX-License-Identifier: GPL-2.0-or-later
// Sirit's OpSpecConstant (the guest render size of reduced passes): standalone, no device.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "sirit/sirit.h"

int main() {
    Sirit::Module module{0x00010000};
    module.AddCapability(spv::Capability::Shader);
    const Sirit::Id u32 = module.TypeInt(32, false);
    const Sirit::Id width = module.SpecConstant(u32, 1920u);
    module.Decorate(width, spv::Decoration::SpecId, 0u);
    const std::vector<uint32_t> words = module.Assemble();
    bool spec = false, decorated = false;
    for (size_t i = 5; i < words.size();) {
        const uint32_t op = words[i] & 0xffff, count = words[i] >> 16;
        if (op == uint32_t(spv::Op::OpSpecConstant) && words[i + 3] == 1920u) spec = true;
        if (op == uint32_t(spv::Op::OpDecorate) && words[i + 2] == uint32_t(spv::Decoration::SpecId) &&
            words[i + 3] == 0u) decorated = true;
        assert(count > 0);
        i += count;
    }
    assert(spec && decorated);
    std::puts("Sirit spec constant: PASS");
}
