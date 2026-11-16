// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Vulkan {

/// gow3 diagnostic GOW3_PRECOMPILE_RECORD (vk_precompile_replay.cpp): records the guest inputs
/// of each pipeline new in this launch into a precompile list, for a later launch's start-up
/// rebuild (GOW3_PRECOMPILE_BOOT, vk_precompile_boot.cpp).
struct PrecompileReplay;
struct PrecompileReplayDeleter {
    void operator()(PrecompileReplay* replay) const;
};
using PrecompileReplayPtr = std::unique_ptr<PrecompileReplay, PrecompileReplayDeleter>;

/// Null unless GOW3_PRECOMPILE_RECORD=<file> is set.
PrecompileReplayPtr MakePrecompileReplay();

} // namespace Vulkan
