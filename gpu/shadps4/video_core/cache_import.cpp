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

// nullopt when the file cannot be read whole. The original is deleted after the import, so a
// failed read must never turn into an empty entry.
std::optional<std::vector<u8>> ReadFile(const std::filesystem::path& file, size_t size) {
    std::vector<u8> data(size);
    std::ifstream in{file, std::ios::binary};
    if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(size))) {
        return std::nullopt;
    }
    return data;
}

void ImportFolder(const std::filesystem::path& folder, const std::filesystem::path& relative,
                  CachePack& pack, std::vector<std::filesystem::path>& imported) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{folder / relative, error}) {
        const auto blob = LooseBlob(relative / entry.path().filename());
        if (!blob || !entry.is_regular_file(error)) {
            continue;
        }
        // Checked before reading so that a huge file is not read into memory for nothing. An
        // empty file is never a valid entry.
        const auto size = entry.file_size(error);
        if (error || size == 0 || size > CachePack::MaxBlobBytes) {
            continue;
        }
        const auto data = ReadFile(entry.path(), size);
        if (data && !pack.Put(blob->first, blob->second, *data).empty()) {
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

std::optional<std::vector<std::filesystem::path>> ImportLooseAndWrite(
    const std::filesystem::path& folder, CachePack& pack) {
    auto imported = ImportLoose(folder, pack);
    if (!imported.empty() && !pack.Rewrite(pack.Image())) {
        return std::nullopt;
    }
    return imported;
}

} // namespace Storage
