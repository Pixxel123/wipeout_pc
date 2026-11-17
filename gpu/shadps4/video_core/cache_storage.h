// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"
#include "video_core/cache_pack.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
    PipelineBinary,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    /// gow3: removes every cached blob but the pipeline binaries (an incompatible cache is
    /// rebuilt, not ignored; the binaries carry their own driver key).
    void Clear();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    /// gow3: a pipeline binary that is found counts as used this launch; one not used in 20
    /// launches is removed at start.
    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);
    /// gow3: Load without counting as a use, for a check that reads every blob.
    void Peek(BlobType type, const std::string& name, std::vector<u8>& data);
    /// gow3: deletes one blob. The file records the removal after the writes already submitted,
    /// so an earlier Save of the same name is undone and a later one is kept.
    void Remove(BlobType type, const std::string& name);

    /// gow3: `stop` returning true ends the walk early (background warm-up at exit).
    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func,
                     const std::function<bool()>& stop = {});
    /// gow3: number of blobs ForEachBlob visits (the loading screen's total).
    size_t CountBlobs(BlobType type);
    /// gow3: names of the blobs of `type`.
    std::vector<std::string> ListBlobs(BlobType type);

private:
    bool SaveBytes(BlobType type, const std::string& name, std::span<const u8> data);
    /// gow3: Find that marks a pipeline binary used; the caller holds pack_mutex.
    const std::vector<u8>* FindUsed(BlobType type, const std::string& name);
    /// gow3: hands a record to the IO thread; the caller holds pack_mutex (see the .cpp).
    void QueueAppend(std::vector<u8>&& record);

    std::filesystem::path cache_path{};
    bool opened{};
    std::unique_ptr<CachePack> pack;
    std::mutex pack_mutex;
    /// gow3: false after a failed write: the file stays as it was, new entries live in memory.
    std::atomic<bool> appending{true};
    std::jthread io_worker{}; ///< last: stopped before the pack it writes is destroyed
};

} // namespace Storage
