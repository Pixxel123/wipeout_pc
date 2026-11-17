// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstring>
#include <fstream>
#include <xxhash.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "video_core/cache_pack.h"

namespace Storage {

namespace {

// gow3: the file is little-endian and values are copied as they sit in memory.
static_assert(std::endian::native == std::endian::little);

constexpr size_t HeaderBytes = 8;
// type, name_size, data_size before the name; the checksum after the data.
constexpr size_t RecordHeadBytes = 12;
constexpr size_t ChecksumBytes = 8;

size_t RecordBytes(size_t name_size, size_t data_size) {
    return RecordHeadBytes + name_size + data_size + ChecksumBytes;
}

void Write(std::vector<u8>& out, const void* bytes, size_t size) {
    const auto* first = static_cast<const u8*>(bytes);
    out.insert(out.end(), first, first + size);
}

template <typename T>
T Read(std::span<const u8> bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

void AppendHeader(std::vector<u8>& out) {
    Write(out, &CachePack::Magic, sizeof(u32));
    Write(out, &CachePack::Version, sizeof(u32));
}

void AppendRecord(std::vector<u8>& out, u32 type, std::string_view name,
                  std::span<const u8> data, bool removed) {
    const size_t start = out.size();
    const u32 name_size = u32(name.size());
    const u32 data_size = removed ? CachePack::Removed : u32(data.size());
    Write(out, &type, sizeof(u32));
    Write(out, &name_size, sizeof(u32));
    Write(out, &data_size, sizeof(u32));
    Write(out, name.data(), name.size());
    Write(out, data.data(), data.size());
    const u64 checksum = XXH3_64bits(out.data() + start, out.size() - start);
    Write(out, &checksum, sizeof(u64));
}

// Reports whether every byte reached the file, so a full disk shows up as a failure.
bool WriteBytes(const std::filesystem::path& path, std::ios::openmode mode,
               std::span<const u8> bytes) {
    std::ofstream stream(path, std::ios::binary | mode);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    stream.flush();
    const bool written = bool(stream);
    stream.close();
    return written && !stream.fail();
}

// Waits until the file's bytes are on the disk itself, not only in the system's write cache.
// Streams cannot do this, so the file is opened again with the system's own calls.
bool SyncToDisk(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const bool synced = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return synced;
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        return false;
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
#endif
}

} // namespace

CachePack::CachePack(std::filesystem::path file_) : file(std::move(file_)) {
    Load();
}

void CachePack::Load() {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0) {
        return;
    }
    // gow3: a file that is not a pack, or one too big to read into memory, is kept aside for a
    // look rather than deleted, and the cache starts over.
    const auto set_aside = [&] {
        auto bad = file;
        bad += ".bad";
        std::filesystem::remove(bad, error);
        std::filesystem::rename(file, bad, error);
    };
    if (size > MaxPackBytes) {
        set_aside();
        return;
    }
    std::vector<u8> bytes(size);
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) {
            return;
        }
    }
    if (size < HeaderBytes || Read<u32>(bytes, 0) != Magic || Read<u32>(bytes, 4) != Version) {
        set_aside();
        return;
    }

    // gow3: records are only ever appended, so damage is a cut-off tail (the game stopped
    // mid-write) or garbage from a bad write. Everything before the first bad record is kept.
    size_t good = HeaderBytes;
    while (good < bytes.size()) {
        const auto rest = std::span<const u8>(bytes).subspan(good);
        if (rest.size() < RecordHeadBytes) {
            break;
        }
        const u32 type = Read<u32>(rest, 0);
        const u32 name_size = Read<u32>(rest, 4);
        const u32 data_size = Read<u32>(rest, 8);
        const bool removed = data_size == Removed;
        if (name_size == 0 || name_size > MaxNameBytes || (!removed && data_size > MaxBlobBytes)) {
            break;
        }
        const size_t record_size = RecordBytes(name_size, removed ? 0 : data_size);
        if (rest.size() < record_size ||
            Read<u64>(rest, record_size - ChecksumBytes) !=
                XXH3_64bits(rest.data(), record_size - ChecksumBytes)) {
            break;
        }
        const std::string_view name(reinterpret_cast<const char*>(rest.data()) + RecordHeadBytes,
                                    name_size);
        if (removed) {
            Remove(type, name);
            dead_bytes += record_size;
        } else {
            Store(type, name, rest.subspan(RecordHeadBytes + name_size, data_size));
        }
        good += record_size;
    }
    // Cut the damage off the file too, so the next append follows a good record.
    if (good < bytes.size()) {
        std::filesystem::resize_file(file, good, error);
    }
}

void CachePack::Store(u32 type, std::string_view name, std::span<const u8> data) {
    auto& entries = types[type];
    auto it = entries.find(name);
    if (it != entries.end()) {
        const size_t old_size = RecordBytes(name.size(), it->second.size());
        live_bytes -= old_size;
        dead_bytes += old_size;
        it->second.assign(data.begin(), data.end());
    } else {
        entries.emplace(std::string(name), std::vector<u8>(data.begin(), data.end()));
    }
    live_bytes += RecordBytes(name.size(), data.size());
}

bool CachePack::Remove(u32 type, std::string_view name) {
    const auto entries = types.find(type);
    if (entries == types.end()) {
        return false;
    }
    const auto it = entries->second.find(name);
    if (it == entries->second.end()) {
        return false;
    }
    const size_t size = RecordBytes(name.size(), it->second.size());
    live_bytes -= size;
    dead_bytes += size;
    entries->second.erase(it);
    return true;
}

const std::vector<u8>* CachePack::Find(u32 type, std::string_view name) const {
    const auto entries = types.find(type);
    if (entries == types.end()) {
        return nullptr;
    }
    const auto it = entries->second.find(name);
    return it == entries->second.end() ? nullptr : &it->second;
}

std::vector<std::string> CachePack::Names(u32 type) const {
    std::vector<std::string> names;
    if (const auto entries = types.find(type); entries != types.end()) {
        names.reserve(entries->second.size());
        for (const auto& [name, data] : entries->second) {
            names.push_back(name);
        }
    }
    return names;
}

size_t CachePack::Count(u32 type) const {
    const auto entries = types.find(type);
    return entries == types.end() ? 0 : entries->second.size();
}

size_t CachePack::LiveBytes() const {
    return live_bytes;
}

size_t CachePack::DeadBytes() const {
    return dead_bytes;
}

std::vector<u8> CachePack::Put(u32 type, std::string_view name, std::span<const u8> data) {
    if (name.empty() || name.size() > MaxNameBytes || data.size() > MaxBlobBytes) {
        return {};
    }
    Store(type, name, data);
    std::vector<u8> record;
    AppendRecord(record, type, name, data, false);
    return record;
}

std::vector<u8> CachePack::Erase(u32 type, std::string_view name) {
    if (!Remove(type, name)) {
        return {};
    }
    std::vector<u8> record;
    AppendRecord(record, type, name, {}, true);
    dead_bytes += record.size();
    return record;
}

void CachePack::EraseIf(const std::function<bool(u32 type)>& keep) {
    for (auto it = types.begin(); it != types.end();) {
        if (keep(it->first)) {
            ++it;
            continue;
        }
        for (const auto& [name, data] : it->second) {
            const size_t size = RecordBytes(name.size(), data.size());
            live_bytes -= size;
            dead_bytes += size;
        }
        it = types.erase(it);
    }
}

std::vector<u8> CachePack::Image() const {
    std::vector<u8> image;
    image.reserve(HeaderBytes + live_bytes);
    AppendHeader(image);
    for (const auto& [type, entries] : types) {
        for (const auto& [name, data] : entries) {
            AppendRecord(image, type, name, data, false);
        }
    }
    return image;
}

bool CachePack::Append(std::span<const u8> record) {
    // A refused Put or an Erase of a missing name has nothing to write; that is not a failure.
    if (record.empty()) {
        return true;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (!error && size > 0) {
        return WriteBytes(file, std::ios::app, record);
    }
    std::vector<u8> first;
    first.reserve(HeaderBytes + record.size());
    AppendHeader(first);
    first.insert(first.end(), record.begin(), record.end());
    return WriteBytes(file, std::ios::app, first);
}

bool CachePack::Rewrite(std::span<const u8> image) {
    // gow3: write a temporary file and rename it over the pack, so a full disk or a crash
    // part-way through leaves the old pack whole. The data reaches the disk before the rename:
    // the first start deletes the loose files right after, and a power cut must not keep those
    // deletes but lose the pack that replaced them.
    auto temp = file;
    temp += ".tmp";
    std::error_code error;
    bool renamed = false;
    if (WriteBytes(temp, std::ios::trunc, image) && SyncToDisk(temp)) {
#ifdef _WIN32
        renamed = MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        std::filesystem::rename(temp, file, error);
        renamed = !error;
#endif
    }
    if (!renamed) {
        std::filesystem::remove(temp, error);
    }
    return renamed;
}

} // namespace Storage
