// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: the launcher's "Native (window)" output size. It is computed from the swapchain, never
// from the presenter's frame: that frame takes the displayed image's aspect, which under "native"
// is the output, so an output fitted to it picks its own next size and never settles.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace Vulkan::OutputResolution {

/// FSR 4.1.1's largest asset tier (fsr411/fsr411.cpp): larger panels keep this output.
inline constexpr uint32_t MaxWidth = 3840;
inline constexpr uint32_t MaxHeight = 2160;

/// The window fitted to an image of `ratio` (width / height), truncated as the presenter does.
inline std::pair<int32_t, int32_t> Fit(int32_t width, int32_t height, float ratio) {
    if (float(width) / float(height) > ratio) {
        return {int32_t(float(height) * ratio), height};
    }
    return {width, int32_t(float(width) / ratio)};
}

inline bool Is16x9(uint32_t width, uint32_t height) {
    return height && std::abs(double(width) / double(height) - 16.0 / 9.0) < 0.01;
}

/// The largest 16:9 frame in the window, both sides even (the render targets halve them);
/// {0, 0} when there is none.
inline std::pair<uint32_t, uint32_t> Frame(uint32_t width, uint32_t height) {
    if (!width || !height) {
        return {0, 0};
    }
    const auto [w, h] = Fit(int32_t(width), int32_t(height), 16.0f / 9.0f);
    if (w < 2 || h < 2) {
        return {0, 0};
    }
    return {uint32_t(w) & ~1u, uint32_t(h) & ~1u};
}

/// The output for a window: Frame() capped at MaxWidth x MaxHeight. {0, 0} when there is none or
/// the game's image is not 16:9 (a render resolution patched to another aspect).
inline std::pair<uint32_t, uint32_t> Output(uint32_t window_width, uint32_t window_height,
                                            uint32_t game_width, uint32_t game_height) {
    if (!Is16x9(game_width, game_height)) {
        return {0, 0};
    }
    const auto [w, h] = Frame(window_width, window_height);
    if (w > MaxWidth) {
        return {MaxWidth, MaxHeight};
    }
    return {w, h};
}

/// A fixed Output resolution row applies to the game's own 1920x1080 render; any other render
/// resolution outputs at its own size (the presets render below it and upscale back).
inline std::pair<uint32_t, uint32_t> FixedOrGuest(uint32_t fixed_width, uint32_t fixed_height,
                                                  uint32_t guest_width, uint32_t guest_height) {
    if (guest_width == 1920 && guest_height == 1080) return {fixed_width, fixed_height};
    return {guest_width, guest_height};
}

/// True once a size has held for kStableFrames calls: a window drag changes the size every
/// frame, and each applied output rebuilds the upscaler and its targets.
class Settled {
public:
    static constexpr uint32_t kStableFrames = 30;

    bool Update(uint32_t width, uint32_t height) {
        if (width != width_ || height != height_) {
            width_ = width;
            height_ = height;
            frames_ = 0;
            return false;
        }
        frames_ = std::min(frames_ + 1, kStableFrames);
        return frames_ == kStableFrames;
    }

private:
    uint32_t width_ = 0, height_ = 0, frames_ = 0;
};

} // namespace Vulkan::OutputResolution
