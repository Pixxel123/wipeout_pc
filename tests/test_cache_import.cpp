// SPDX-License-Identifier: GPL-2.0-or-later
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "video_core/cache_import.h"

using Storage::CachePack;
using Storage::ImportLoose;
using Storage::LooseBlob;

namespace {
std::vector<u8> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

void Write(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream{file, std::ios::binary} << text;
}

// A function, since the comma in a braced pair would split the arguments of assert.
bool Is(const std::filesystem::path& relative, u32 type, const std::string& name) {
    return LooseBlob(relative) == std::pair{type, name};
}

void LooseBlobNames() {
    // The first numbers are the Storage::BlobType values, stated here because cache_storage.h
    // needs the whole library.
    assert(Is("0x0000000012345678.meta", 0, "0x0000000012345678"));
    assert(Is("0x0000000012345678_3.spv", 1, "0x0000000012345678_3"));
    assert(LooseBlob("g_0x00000000deadbeef.key")->first == 2);
    assert(LooseBlob("profile.bin")->first == 3);
    assert(Is("pipeline-binaries/0a1b.bin", 4, "0a1b"));
    assert(!LooseBlob("recent.txt") && !LooseBlob("a.spv.tmp") && !LooseBlob("other/x.bin") &&
           !LooseBlob("cache.pack"));
}

void Import() {
    const auto folder = std::filesystem::temp_directory_path() / "cache-import-test";
    std::filesystem::remove_all(folder);
    Write(folder / "a.meta", "m");
    Write(folder / "a_0.spv", "s");
    Write(folder / "pipeline-binaries/0a.bin", "b");
    Write(folder / "junk.txt", "j");
    Write(folder / "empty.spv", ""); // never a valid entry: left alone
    // A file that cannot be read must stay too. As root the open succeeds, so the check is
    // skipped there.
    Write(folder / "locked.meta", "l");
    std::filesystem::permissions(folder / "locked.meta", std::filesystem::perms::none);
    const bool locked = !std::ifstream{folder / "locked.meta"}.good();
    Write(folder / "b.spv.tmp", "t");
    // Over the blob limit: left out, and left on disk.
    Write(folder / "big.spv", std::string(CachePack::MaxBlobBytes + 1, 'x'));

    CachePack pack{folder / "cache.pack"};
    const auto imported = ImportLoose(folder, pack);
    assert(imported.size() == (locked ? 3u : 4u)); // a, a_0 and 0a, plus locked.meta as root
    assert(*pack.Find(0, "a") == Bytes("m") && *pack.Find(1, "a_0") == Bytes("s") &&
           *pack.Find(4, "0a") == Bytes("b"));
    assert(!pack.Find(1, "big") && !pack.Find(1, "empty") && (!locked || !pack.Find(0, "locked")));
    assert(std::filesystem::exists(folder / "empty.spv") && std::filesystem::exists(folder / "locked.meta"));
    assert(std::filesystem::exists(folder / "junk.txt")); // import deletes nothing itself
    assert(std::filesystem::exists(folder / "a.meta"));
    std::filesystem::permissions(folder / "locked.meta", std::filesystem::perms::owner_all);
    std::filesystem::remove_all(folder);
}
} // namespace

int main() {
    LooseBlobNames();
    Import();
    std::puts("cache import: all tests passed");
}
