// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/cache_pack.h"

namespace Storage {

/// The blob type and name of a loose cache file, by its path relative to the cache folder:
/// "<name>.meta" ShaderMeta, "<name>.spv" ShaderBinary, "<name>.key" PipelineKey,
/// "<name>.bin" ShaderProfile, "pipeline-binaries/<name>.bin" PipelineBinary. Anything else: nullopt.
std::optional<std::pair<u32, std::string>> LooseBlob(const std::filesystem::path& relative);

/// Puts every loose file under `folder` into `pack` (memory only) and returns their paths.
/// Files the pack refuses (too big) and files that are not cache files are left out.
std::vector<std::filesystem::path> ImportLoose(const std::filesystem::path& folder,
                                               CachePack& pack);

} // namespace Storage
