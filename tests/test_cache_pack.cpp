// SPDX-License-Identifier: GPL-2.0-or-later
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include "video_core/cache_pack.h"

using Storage::CachePack;

namespace {
std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

// A pack on disk is the header plus every record ever appended, so these sizes must agree.
void AssertSizesMatchFile(const CachePack& pack, const std::filesystem::path& file) {
    assert(8 + pack.LiveBytes() + pack.DeadBytes() == std::filesystem::file_size(file));
    assert(pack.Image().size() == 8 + pack.LiveBytes());
}

void RoundTrip(const std::filesystem::path& file) {
    {
        CachePack p{file};
        assert(p.Append(p.Put(1, "a", Bytes("one"))));
        assert(p.Append(p.Put(2, "a", Bytes("two"))));
    }
    {
        CachePack p{file};
        assert(*p.Find(1, "a") == Bytes("one"));
        assert(*p.Find(2, "a") == Bytes("two"));
        assert(p.Find(1, "b") == nullptr);
        assert(p.Count(1) == 1);
        assert(p.Names(2) == std::vector<std::string>{"a"});
        assert(p.DeadBytes() == 0);
        AssertSizesMatchFile(p, file);
    }
}

void LastRecordWinsAfterReopen(const std::filesystem::path& file) {
    {
        CachePack p{file};
        p.Append(p.Put(1, "a", Bytes("old")));
        p.Append(p.Put(1, "a", Bytes("new")));
        p.Append(p.Put(1, "b", Bytes("x")));
        p.Append(p.Erase(1, "b"));
    }
    {
        CachePack p{file};
        assert(*p.Find(1, "a") == Bytes("new"));
        assert(p.Find(1, "b") == nullptr);
        assert(p.DeadBytes() > 0);
        AssertSizesMatchFile(p, file);
    }
}

void TruncatedTailKeepsEarlierRecords(const std::filesystem::path& file) {
    std::uintmax_t size_after_first_record{};
    {
        CachePack p{file};
        assert(p.Append(p.Put(1, "a", Bytes("one"))));
        size_after_first_record = std::filesystem::file_size(file);
        assert(p.Append(p.Put(1, "b", Bytes("two"))));
    }
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 5);
    {
        CachePack p{file};
        assert(*p.Find(1, "a") == Bytes("one"));
        assert(p.Find(1, "b") == nullptr);
    }
    assert(std::filesystem::file_size(file) == size_after_first_record);
}

void CorruptRecordCutsTheLog(const std::filesystem::path& file) {
    std::uintmax_t size_after_first_record{};
    {
        CachePack p{file};
        assert(p.Append(p.Put(1, "a", Bytes("one"))));
        size_after_first_record = std::filesystem::file_size(file);
        assert(p.Append(p.Put(1, "b", Bytes("two"))));
        assert(p.Append(p.Put(1, "c", Bytes("three"))));
    }
    {
        // The second record's data starts after its 12-byte head and 1-byte name. Flipping a
        // data byte leaves every length valid, so only the checksum can catch it.
        std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
        const auto offset = std::streamoff(size_after_first_record) + 12 + 1;
        stream.seekg(offset);
        char byte{};
        stream.get(byte);
        stream.seekp(offset);
        stream.put(char(byte ^ 0x01));
    }
    {
        CachePack p{file};
        assert(p.Find(1, "a"));
        assert(!p.Find(1, "b"));
        assert(!p.Find(1, "c"));
    }
    assert(std::filesystem::file_size(file) == size_after_first_record);
}

void OversizedOrForeignFileStartsEmpty(const std::filesystem::path& file) {
    const auto bad = std::filesystem::path(file.string() + ".bad");
    {
        std::ofstream stream(file, std::ios::binary);
        stream << std::string(64, 'x');
    }
    {
        CachePack p{file};
        assert(p.Count(1) == 0);
    }
    assert(std::filesystem::exists(file.string() + ".bad"));
    assert(std::filesystem::file_size(bad) == 64);
    assert(!std::filesystem::exists(file));

    // A sparse file: its reported size is over the limit without using the disk space.
    { std::ofstream stream(file, std::ios::binary); }
    std::filesystem::resize_file(file, CachePack::MaxPackBytes + 1);
    {
        CachePack p{file};
        assert(p.Count(1) == 0);
    }
    assert(std::filesystem::exists(file.string() + ".bad"));
    assert(std::filesystem::file_size(bad) == CachePack::MaxPackBytes + 1);
    assert(!std::filesystem::exists(file));
    std::filesystem::remove(bad);
}

void FailedRewriteLeavesTheOldFile(const std::filesystem::path& root) {
    const auto missing_dir = root / "missing";
    {
        CachePack p{missing_dir / "cache.pack"};
        assert(!p.Rewrite(p.Image()));
    }
    assert(!std::filesystem::exists(missing_dir));

    const auto file = root / "rewrite.pack";
    {
        CachePack p{file};
        assert(p.Append(p.Put(1, "a", Bytes("one"))));
        assert(p.Append(p.Put(1, "b", Bytes("two"))));
    }
    // A directory in the temp file's place makes the write fail with the good pack present.
    std::filesystem::create_directory(file.string() + ".tmp");
    {
        CachePack p{file};
        p.Put(1, "a", Bytes("changed"));
        p.Erase(1, "b");
        assert(!p.Rewrite(p.Image()));
    }
    assert(!std::filesystem::exists(file.string() + ".tmp"));
    {
        CachePack p{file};
        assert(*p.Find(1, "a") == Bytes("one"));
        assert(*p.Find(1, "b") == Bytes("two"));
    }
}

void Limits(const std::filesystem::path& file) {
    CachePack p{file};
    assert(p.Put(1, std::string(CachePack::MaxNameBytes + 1, 'n'), Bytes("x")).empty());
    assert(p.Put(1, "", Bytes("x")).empty());
    assert(p.Put(1, "big", std::vector<u8>(CachePack::MaxBlobBytes + 1)).empty());
    assert(p.Count(1) == 0);
    assert(p.LiveBytes() == 0);
    assert(!p.Put(1, std::string(CachePack::MaxNameBytes, 'n'), Bytes("x")).empty());
    assert(!p.Put(1, "big", std::vector<u8>(CachePack::MaxBlobBytes)).empty());
    assert(p.Count(1) == 2);
    // A refused Put hands back nothing to write, which is not a failed append.
    assert(p.Append({}));
    assert(!std::filesystem::exists(file));
}

void ImageRoundTrip(const std::filesystem::path& file) {
    {
        CachePack p{file};
        p.Put(1, "a", Bytes("one"));
        p.Put(1, "b", Bytes("two"));
        p.Put(2, "c", Bytes("three"));
        p.Erase(1, "b");
        assert(p.Rewrite(p.Image()));
    }
    {
        CachePack p{file};
        assert(p.Count(1) + p.Count(2) == 2);
        assert(*p.Find(1, "a") == Bytes("one"));
        assert(p.Find(1, "b") == nullptr);
        assert(*p.Find(2, "c") == Bytes("three"));
        assert(p.DeadBytes() == 0);
        AssertSizesMatchFile(p, file);
    }
    assert(!std::filesystem::exists(file.string() + ".tmp"));
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("gow3-cache-pack-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    RoundTrip(root / "round_trip.pack");
    LastRecordWinsAfterReopen(root / "last_record_wins.pack");
    TruncatedTailKeepsEarlierRecords(root / "truncated.pack");
    CorruptRecordCutsTheLog(root / "corrupt.pack");
    OversizedOrForeignFileStartsEmpty(root / "foreign.pack");
    FailedRewriteLeavesTheOldFile(root);
    Limits(root / "limits.pack");
    ImageRoundTrip(root / "image.pack");
    std::filesystem::remove_all(root);
    std::puts("cache pack: all tests passed");
}
