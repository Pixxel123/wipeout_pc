// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
/// The game's own 1920x1080 render (no render resolution patch).
constexpr bool NativeRender(Size size) {
    return size == Size{};
}
/// The largest size a render resolution patch is generated for.
inline constexpr Size MaxGuest{7680, 4320};
/// The session's guest size from the saved render size: a size below the game's own (`floor`,
/// {0, 0} for a title without one) leaves the game on its whole buffer, nothing above MaxGuest
/// is patched, and both sides are even.
constexpr Size ClampGuest(Size wanted, Size floor) {
    if (wanted.width < floor.width || wanted.height < floor.height) wanted = floor;
    return {std::min(wanted.width, MaxGuest.width) & ~1u, std::min(wanted.height, MaxGuest.height) & ~1u};
}
inline Size ForPreset(int preset, Size output) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    constexpr std::array<double, 5> scales{1.0, 1.5, 1.7, 2.0, 3.0};
    const double scale = scales[std::clamp(preset, 0, 4)];
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
/// 1 for the guest's size, 2 for exactly half of it (the scene must halve too), else 0.
constexpr uint32_t SizeDivisor(Size image, Size guest, Size scene, bool half_res = true) {
    if (image == guest) return 1;
    return half_res && image.width * 2 == guest.width && image.height * 2 == guest.height &&
                   scene.width % 2 == 0 && scene.height % 2 == 0
               ? 2u : 0u;
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution
