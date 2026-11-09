// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the Output resolution rows apply to the game's own 1920x1080 render.
#pragma once

#include <cstdint>
#include <utility>

#include "scene_resolution.h"

namespace Vulkan::OutputResolution {

/// A fixed Output resolution row applies to the game's own 1920x1080 render; any other render
/// resolution outputs at its own size (the presets render below it and upscale back).
inline std::pair<uint32_t, uint32_t> FixedOrGuest(uint32_t fixed_width, uint32_t fixed_height,
                                                  uint32_t guest_width, uint32_t guest_height) {
    if (SceneResolution::NativeRender({guest_width, guest_height})) return {fixed_width, fixed_height};
    return {guest_width, guest_height};
}

} // namespace Vulkan::OutputResolution
