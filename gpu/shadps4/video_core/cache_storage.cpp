// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

#include "video_core/cache_import.h"
#include "video_core/cache_storage.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

namespace {

// gow3: a pipeline binary that no build asked for in this many launches is removed at start.
// A race uses some hundreds of them; without this the pack only grows.
constexpr u32 UnusedLaunches = 20;
static_assert(UnusedLaunches < Storage::CachePack::MaxAge);

std::mutex submit_mutex{};
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

void Submit(std::packaged_task<void()>&& request) {
    {
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }
    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
}

// gow3: a disk that is full or read-only must not stop the game. The file keeps the last
// version that was written whole, and the entries of this session stay in memory.
void StopAppending(std::atomic<bool>& appending) {
    if (appending.exchange(false)) {
        LOG_WARNING(Render, "Cache pack: could not write, new entries stay in memory this session");
    }
}

// gow3: copies a blob into the caller's vector, which ends up empty when there is no blob.
template <typename T>
void CopyBlob(const std::vector<u8>* blob, std::vector<T>& data) {
    data.resize(blob ? blob->size() / sizeof(T) : 0);
    if (!data.empty()) {
        std::memcpy(data.data(), blob->data(), data.size() * sizeof(T));
    }
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests != 0; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
}

// gow3: the first start after the update finds the old loose files and no pack. They go into the
// pack so that nobody recompiles, and are deleted on the IO thread once the pack is on disk.
// False when the pack could not be written: the loose files stay for the next start.
static bool ImportLoosePack(CachePack& pack, const std::filesystem::path& folder) {
    const auto started = std::chrono::steady_clock::now();
    const auto imported = ImportLooseAndWrite(folder, pack);
    if (!imported) {
        return false;
    }
    if (imported->empty()) {
        return true;
    }
    const auto import_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
    std::printf("Cache pack: imported %zu files (%.1f MB) in %.0f ms\n", imported->size(),
                pack.LiveBytes() / 1048576.0, import_ms.count());
    Submit(std::packaged_task<void()>{[imported = *imported, folder] {
        std::error_code error;
        for (const auto& file : imported) {
            std::filesystem::remove(file, error);
        }
        std::filesystem::remove(folder / "pipeline-binaries", error); // only if now empty
    }});
    return true;
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    using namespace Common::FS;
    cache_path = GetUserPath(PathType::CacheDir) / Common::ElfInfo::Instance().GameSerial();
    std::error_code error;
    std::filesystem::create_directories(cache_path, error);

    const bool had_pack = std::filesystem::exists(cache_path / "cache.pack", error);
    const auto started = std::chrono::steady_clock::now();
    auto loaded = std::make_unique<CachePack>(cache_path / "cache.pack");
    const auto read_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
    size_t entries = 0;
    for (u32 type = 0; type <= u32(BlobType::PipelineBinary); ++type) {
        entries += loaded->Count(type);
    }
    std::printf("Cache pack: %zu entries, %.1f MB live, %.1f MB dead, read in %.0f ms\n", entries,
                loaded->LiveBytes() / 1048576.0, loaded->DeadBytes() / 1048576.0, read_ms.count());
    // gow3: before the rewrite decision, so the bytes of the binaries dropped here count as dead.
    if (const size_t dropped =
            loaded->EraseUnused(u32(BlobType::PipelineBinary), UnusedLaunches)) {
        std::printf("Cache pack: %zu pipeline binaries unused for %u launches removed\n", dropped,
                    UnusedLaunches);
    }
    // gow3: replaced and removed entries stay in the file, since it is only appended to. Once
    // they outweigh the live ones and are worth a write, the file is written again without them.
    const size_t live = loaded->LiveBytes();
    const size_t dead = loaded->DeadBytes();
    if (dead > (size_t{8} << 20) && dead > live) {
        loaded->Rewrite(loaded->Image());
    }
    appending = true;
    // gow3: after a failed import nothing is appended this session: the first append would
    // create a pack, and the next start would then skip the import and recompile everything.
    if (!had_pack && !ImportLoosePack(*loaded, cache_path)) {
        StopAppending(appending);
    }

    {
        std::scoped_lock lock{pack_mutex};
        pack = std::move(loaded);
        // gow3: after any rewrite above, which writes the ages as they were before this launch.
        QueueAppend(pack->StartLaunch());
    }
    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    std::scoped_lock lock{pack_mutex};
    pack.reset();
    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

// gow3: the memory side and the queue are updated under one hold of pack_mutex, so the file
// receives the records in the order the entries changed in memory: after a restart the last
// record of a name matches what this session last saw.
void DataBase::QueueAppend(std::vector<u8>&& record) {
    if (record.empty() || !appending) {
        return;
    }
    Submit(std::packaged_task<void()>{[this, record = std::move(record)] {
        if (appending && !pack->Append(record)) {
            StopAppending(appending);
        }
    }});
}

bool DataBase::SaveBytes(BlobType type, const std::string& name, std::span<const u8> data) {
    std::scoped_lock lock{pack_mutex};
    if (!pack) {
        return false;
    }
    auto record = pack->Put(u32(type), name, data);
    if (record.empty()) {
        return false; // over the name or size limit
    }
    QueueAppend(std::move(record));
    return true;
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    return SaveBytes(type, name, data);
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    return SaveBytes(type, name,
                     {reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(u32)});
}

void DataBase::Remove(BlobType type, const std::string& name) {
    std::scoped_lock lock{pack_mutex};
    if (pack) {
        QueueAppend(pack->Erase(u32(type), name));
    }
}

const std::vector<u8>* DataBase::FindUsed(BlobType type, const std::string& name) {
    if (!pack) {
        return nullptr;
    }
    // gow3: only the pipeline binaries age out; the shader store is bounded by the build stamp.
    // The record is queued, so the lookup never waits on the disk.
    if (type == BlobType::PipelineBinary) {
        QueueAppend(pack->Use(u32(type), name));
    }
    return pack->Find(u32(type), name);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    std::scoped_lock lock{pack_mutex};
    CopyBlob(FindUsed(type, name), data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    std::scoped_lock lock{pack_mutex};
    CopyBlob(FindUsed(type, name), data);
}

void DataBase::Peek(BlobType type, const std::string& name, std::vector<u8>& data) {
    std::scoped_lock lock{pack_mutex};
    CopyBlob(pack ? pack->Find(u32(type), name) : nullptr, data);
}

size_t DataBase::CountBlobs(BlobType type) {
    std::scoped_lock lock{pack_mutex};
    return pack ? pack->Count(u32(type)) : 0;
}

std::vector<std::string> DataBase::ListBlobs(BlobType type) {
    std::scoped_lock lock{pack_mutex};
    return pack ? pack->Names(u32(type)) : std::vector<std::string>{};
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func,
                           const std::function<bool()>& stop) {
    // gow3: one entry at a time, and `func` outside the lock, so saves and loads of other
    // threads are not held up by the walk.
    for (const auto& name : ListBlobs(type)) {
        if (stop && stop()) {
            return;
        }
        std::vector<u8> data;
        {
            std::scoped_lock lock{pack_mutex};
            const auto* blob = pack ? pack->Find(u32(type), name) : nullptr;
            if (!blob) {
                continue; // removed since the names were taken
            }
            data = *blob;
        }
        func(std::move(data));
    }
}

void DataBase::Clear() {
    std::scoped_lock lock{pack_mutex};
    if (!pack) {
        return;
    }
    size_t removed = 0;
    for (u32 type = 0; type < u32(BlobType::PipelineBinary); ++type) {
        removed += pack->Count(type);
    }
    pack->EraseIf([](u32 type) { return type == u32(BlobType::PipelineBinary); });
    // gow3: the file is written again whole, so the next start never reads the old entries. If
    // that fails, nothing more is appended: new entries after the old ones would pass for valid.
    // With appends stopped the file is left alone too: a rewrite could create a pack that hides
    // loose files not yet imported.
    if (appending) {
        Submit(std::packaged_task<void()>{[this, image = pack->Image()] {
            if (appending && !pack->Rewrite(image)) {
                StopAppending(appending);
            }
        }});
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} entries): it is rebuilt for this build",
                removed);
}

} // namespace Storage
