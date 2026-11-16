// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Vulkan {

/// gow3 diagnostic GOW3_PRECOMPILE_REPLAY (vk_precompile_replay.cpp): records the guest inputs
/// of each pipeline new in this launch and replays its selection from them a few frames later.
struct PrecompileReplay;
struct PrecompileReplayDeleter {
    void operator()(PrecompileReplay* replay) const;
};
using PrecompileReplayPtr = std::unique_ptr<PrecompileReplay, PrecompileReplayDeleter>;

/// Null unless GOW3_PRECOMPILE_REPLAY is 1 (selection only) or 2 (also the SPIR-V check).
PrecompileReplayPtr MakePrecompileReplay();

} // namespace Vulkan
