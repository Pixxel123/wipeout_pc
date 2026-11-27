// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Vulkan {

/// gow3 diagnostic GOW3_PRECOMPILE_RECORD (vk_precompile_record.cpp): records the guest inputs
/// of each pipeline new in this launch into a precompile list, for a later launch's start-up
/// rebuild (GOW3_PRECOMPILE_BOOT, vk_precompile_boot.cpp).
struct PrecompileRecord;
struct PrecompileRecordDeleter {
    void operator()(PrecompileRecord* record) const;
};
using PrecompileRecordPtr = std::unique_ptr<PrecompileRecord, PrecompileRecordDeleter>;

/// Null unless GOW3_PRECOMPILE_RECORD=<file> is set.
PrecompileRecordPtr MakePrecompileRecord();

} // namespace Vulkan
