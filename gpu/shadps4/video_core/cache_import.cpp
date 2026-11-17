// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <iterator>

#include "video_core/cache_import.h"

namespace Storage {

namespace {
// gow3: the numbers of Storage::BlobType, which this file does not include so that the host test
// can build it without the rest of the library.
constexpr u32 ShaderMeta = 0, ShaderBinary = 1, PipelineKey = 2, ShaderProfile = 3,
              PipelineBinary = 4;

constexpr const char* PipelineBinaries = "pipeline-binaries";

std::vector<u8> ReadFile(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void ImportFolder(const std::filesystem::path& folder, const std::filesystem::path& relative,
                  CachePack& pack, std::vector<std::filesystem::path>& imported) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{folder / relative, error}) {
        const auto blob = LooseBlob(relative / entry.path().filename());
        if (!blob || !entry.is_regular_file(error)) {
            continue;
        }
        // Checked before reading so that a huge file is not read into memory for nothing.
        if (entry.file_size(error) > CachePack::MaxBlobBytes) {
            continue;
        }
        if (!pack.Put(blob->first, blob->second, ReadFile(entry.path())).empty()) {
            imported.push_back(entry.path());
        }
    }
}
} // namespace

std::optional<std::pair<u32, std::string>> LooseBlob(const std::filesystem::path& relative) {
    const auto extension = relative.extension().string();
    const auto name = relative.stem().string();
    const auto parent = relative.parent_path();
    if (parent.empty()) {
        if (extension == ".meta") {
            return std::pair{ShaderMeta, name};
        } else if (extension == ".spv") {
            return std::pair{ShaderBinary, name};
        } else if (extension == ".key") {
            return std::pair{PipelineKey, name};
        } else if (extension == ".bin") {
            return std::pair{ShaderProfile, name};
        }
    } else if (parent == PipelineBinaries && extension == ".bin") {
        return std::pair{PipelineBinary, name};
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> ImportLoose(const std::filesystem::path& folder,
                                               CachePack& pack) {
    std::vector<std::filesystem::path> imported;
    ImportFolder(folder, {}, pack, imported);
    ImportFolder(folder, PipelineBinaries, pack, imported);
    return imported;
}

} // namespace Storage
