// The reader of program code from the game files (vk_precompile_files.h) on the synthetic
// archives of tests/data/precompile_files (tests/data/make_precompile_files.py) and eboot.bin
// files written here: the table of contents found by name hash, a stored and a zlib entry, a wide
// block size table, an entry with two programs, an entry listed twice, the folder order and the
// first readable eboot.bin, missing files and cut or corrupt archives; with a filter, only the
// wanted programs, and no file read once all of them are found.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>
#if __has_include(<miniz.h>)
#include <miniz.h>
#else
#include <zlib.h>
#endif
#include "video_core/renderer_vulkan/vk_precompile_files.h"

using namespace Vulkan::Precompile;
namespace fs = std::filesystem;

namespace {

const fs::path Data = fs::path{__FILE__}.parent_path() / "data" / "precompile_files";

std::vector<u8> ReadFile(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

void WriteFile(const fs::path& path, const std::vector<u8>& bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void Put32(std::vector<u8>& out, u32 value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(u8(value >> (8 * i)));
    }
}

/// A program's code as the game's files hold it (make_precompile_files.py's blob).
std::vector<u8> Blob(u64 program, std::initializer_list<u32> body = {1, 2, 3, 4}) {
    const u32 words = u32(body.size()) + 2;
    std::vector<u8> out;
    Put32(out, 0xBEEB03FF);
    Put32(out, words / 2 - 1);
    for (const u32 word : body) {
        Put32(out, word);
    }
    const char signature[8] = "OrbShdr";
    out.insert(out.end(), signature, signature + 8);
    Put32(out, ((words + 8) * 4) << 8);
    Put32(out, 0);
    Put32(out, u32(program));
    Put32(out, u32(program >> 32));
    // The crc32 covers the start token through the header's first 16 bytes, not the hash.
    Put32(out, u32(crc32(0, out.data(), uInt(out.size() - 8))));
    Put32(out, 0);
    return out;
}

std::vector<u8> Cat(std::initializer_list<std::vector<u8>> parts) {
    std::vector<u8> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

std::string Hex(const std::array<u8, 16>& digest) {
    std::string out;
    char byte[3];
    for (const u8 b : digest) {
        std::snprintf(byte, sizeof(byte), "%02x", b);
        out += byte;
    }
    return out;
}

/// A fresh folder `name` under the test's temporary folder.
fs::path Folder(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "test_precompile_files" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

fs::path Index(const fs::path& dir, const std::string& text) {
    const fs::path path = dir / "index.txt";
    WriteFile(path, {text.begin(), text.end()});
    return path;
}

struct Read {
    GameCode code;
    u32 calls = 0, first_done = 99, first_total = 0, last_done = 0, last_total = 0;
};

Read Run(std::initializer_list<fs::path> dirs, const fs::path& index,
         const std::set<u64>& wanted = {}) {
    Read read;
    const std::vector<fs::path> list{dirs};
    read.code = ReadGameCode(
        list, index,
        [&](u32 done, u32 total) {
            if (read.calls++ == 0) {
                read.first_done = done;
                read.first_total = total;
            }
            assert(done <= total);
            read.last_done = done;
            read.last_total = total;
        },
        wanted);
    return read;
}

bool Has(const GameCode& code, u64 program, const std::vector<u8>& want) {
    const auto found = code.by_program.find(program);
    return found != code.by_program.end() && found->second == want;
}

bool Missing(const GameCode& code, const std::string& text) {
    for (const auto& line : code.missing) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// The update folder (its eboot.bin with 0x55) and the game's (both archives, eboot.bin with an
/// older 0x55 and 0x66).
fs::path Base(const std::string& name) {
    const fs::path base = Folder(name);
    fs::copy_file(Data / "data00.psarc", base / "data00.psarc");
    fs::copy_file(Data / "data01.psarc", base / "data01.psarc");
    WriteFile(base / "eboot.bin",
              Cat({std::vector<u8>(12), Blob(0x55, {7, 7, 7, 7}), std::vector<u8>(4), Blob(0x66)}));
    return base;
}

fs::path Update() {
    const fs::path update = Folder("update");
    WriteFile(update / "eboot.bin", Cat({std::vector<u8>(3), Blob(0x55, {9, 9, 9, 9})}));
    return update;
}

const std::string FullIndex =
    "eboot.bin\ndata00.psarc\ta.bin\ndata00.psarc\tB.BIN\ndata01.psarc\tbig.bin\n";

void TestMd5() {
    assert(Hex(Md5("")) == "d41d8cd98f00b204e9800998ecf8427e");
    assert(Hex(Md5("abc")) == "900150983cd24fb0d6963f7d28e17f72");
    assert(Hex(Md5("1234567890123456789012345678901234567890123456789012345678901234567890"
                   "1234567890")) == "57edf4a22be3c955ac49da2e2107b67a");
    // The padding boundaries (Python's hashlib).
    assert(Hex(Md5(std::string(55, 'a'))) == "ef1772b6dff9a122358552954ad0df65");
    assert(Hex(Md5(std::string(56, 'a'))) == "3b0c8ac703f828b04c6c197006d17218");
    assert(Hex(Md5(std::string(63, 'a'))) == "b06521f39153d618550606be297466d5");
    assert(Hex(Md5(std::string(64, 'a'))) == "014842d480b571495a4a0363793f7367");
}

void TestReadsEveryIndexedFile() {
    const fs::path base = Base("base"), update = Update();
    // B.BIN: the archive ignores case, so the manifest's b.bin; its TOC entry is listed twice.
    const auto read = Run({update, base}, Index(base, FullIndex));
    const auto& code = read.code;
    assert(code.missing.empty());
    assert(code.files_read == 4); // the update's eboot.bin, a.bin, b.bin once, big.bin
    assert(code.by_program.size() == 5); // not 0x66: only in the game's eboot.bin, not read
    assert(Has(code, 0x11, Blob(0x11)));          // after a full stored block
    assert(Has(code, 0x22, Blob(0x22)));          // zlib blocks, two programs in one entry
    assert(Has(code, 0x33, Blob(0x33, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                       17, 18, 19})));
    assert(Has(code, 0x44, Blob(0x44)));          // a 3-byte size table, across two blocks
    assert(Has(code, 0x55, Blob(0x55, {9, 9, 9, 9}))); // the update folder first
    assert(read.first_done == 0 && read.first_total == 5);
    assert(read.last_done == 5 && read.last_total == 5);

    // The other folder order: the game's eboot.bin first, and only it.
    const auto other = Run({base, update}, Index(base, FullIndex));
    assert(Has(other.code, 0x55, Blob(0x55, {7, 7, 7, 7})) && Has(other.code, 0x66, Blob(0x66)));
    assert(other.code.files_read == 4);
    // Windows line ends.
    const auto crlf = Run({update, base}, Index(base, "eboot.bin\r\ndata00.psarc\ta.bin\r\n"));
    assert(crlf.code.missing.empty() && Has(crlf.code, 0x11, Blob(0x11)));
    // Only the indexed entries are read.
    const auto one = Run({base}, Index(base, "data00.psarc\tc.bin\n"));
    assert(one.code.files_read == 1 && one.code.by_program.empty() && one.code.missing.empty());
}

void TestMissingFilesRejectOnlyTheirOwn() {
    const fs::path base = Base("base"), update = Update();
    const auto read = Run({base / "no-such-folder", update, base},
                          Index(base, FullIndex + "data00.psarc\tnope.bin\ndata09.psarc\tx.bin\n"));
    const auto& code = read.code;
    assert(code.missing.size() == 2);
    assert(Missing(code, "nope.bin") && Missing(code, "data09.psarc"));
    assert(code.by_program.size() == 5 && code.files_read == 4);
    assert(read.last_done == read.last_total);

    const auto none = Run({}, Index(base, FullIndex));
    assert(none.code.by_program.empty() && none.code.files_read == 0);
    assert(Missing(none.code, "eboot.bin") && Missing(none.code, "data00.psarc") &&
           Missing(none.code, "data01.psarc"));
    const auto no_index = Run({base}, base / "no-such-index");
    assert(no_index.code.by_program.empty() && no_index.code.missing.size() == 1);
    const std::string large(1 << 20, '\n');
    const auto too_large = Run({base}, Index(base, "eboot.bin" + large));
    assert(too_large.code.by_program.empty() && too_large.code.missing.size() == 1);
    assert(Missing(too_large.code, "larger than 1 MiB"));
}

void TestArchiveNamesAreFileNamesOnly() {
    // From the folder `sub`, each of these would name base/data00.psarc.
    const fs::path base = Base("base"), sub = base / "sub";
    fs::create_directories(sub);
    const std::string text = "../data00.psarc\ta.bin\n" + (base / "data00.psarc").string() +
                             "\ta.bin\nC:data00.psarc\ta.bin\n..\ta.bin\n.\ta.bin\n";
    const auto read = Run({sub}, Index(base, text));
    assert(read.code.by_program.empty() && read.code.files_read == 0);
    assert(read.code.missing.size() == 5);
    for (const auto& line : read.code.missing) {
        assert(line.ends_with(": not a file name"));
    }
    // A long name is cut in its missing line.
    const auto long_name = Run({sub}, Index(base, std::string(500, 'x') + ".psarc\ta.bin\n"));
    assert(long_name.code.missing.size() == 1 && long_name.code.missing[0].size() < 200);
    // A long path keeps its end, with the file name.
    const fs::path long_path = base / std::string(200, 'd') / "my-index.txt";
    const auto long_index = Run({sub}, long_path);
    const std::string tail = long_path.string().substr(long_path.string().size() - 120);
    assert(long_index.code.missing.size() == 1 &&
           long_index.code.missing[0] == "..." + tail + ": cannot read the index");
}

void TestCaseAndDuplicates() {
    const fs::path base = Folder("flags");
    auto small = ReadFile(Data / "data00.psarc");
    // Flags bit 0 clear: the raw name is hashed, and the TOC holds the upper-cased names' MD5.
    small[31] &= ~1; // flags: big-endian bytes 28-31
    WriteFile(base / "data00.psarc", small);
    const auto raw = Run({base}, Index(base, "data00.psarc\tA.BIN\ndata00.psarc\ta.bin\n"));
    assert(Has(raw.code, 0x11, Blob(0x11)) && raw.code.files_read == 1);
    assert(raw.code.missing.size() == 1 && Missing(raw.code, "a.bin: not in the archive"));
    // b.bin's first TOC entry (entry 1) too large: its second is read.
    small = ReadFile(Data / "data00.psarc");
    for (u32 at = 32 + 30 + 20; at < 32 + 30 + 25; ++at) {
        small[at] = 0xFF;
    }
    WriteFile(base / "data00.psarc", small);
    const auto dup = Run({base}, Index(base, "data00.psarc\tb.bin\n"));
    assert(dup.code.missing.empty() && dup.code.files_read == 1);
    assert(Has(dup.code, 0x22, Blob(0x22)) && dup.last_done == 1 && dup.last_total == 1);
}

void TestCutArchives() {
    const fs::path base = Base("cut");
    const auto small = ReadFile(Data / "data00.psarc");
    const auto big = ReadFile(Data / "data01.psarc");
    // Its table of contents cut: the archive is named, the other files are still read.
    WriteFile(base / "data00.psarc", {small.begin(), small.begin() + 40});
    // Its data cut: big.bin is named.
    WriteFile(base / "data01.psarc", {big.begin(), big.end() - 20});
    const auto read = Run({base}, Index(base, FullIndex));
    assert(Missing(read.code, "data00.psarc") && Missing(read.code, "big.bin"));
    assert(read.code.by_program.size() == 2 && Has(read.code, 0x66, Blob(0x66)));
    assert(read.last_done == read.last_total);

    // Every length and every byte changed: no crash. (The crc32 does not cover the hash.)
    const fs::path index = Index(base, FullIndex);
    fs::remove(base / "eboot.bin");
    for (const auto* archive : {&small, &big}) {
        const char* name = archive == &small ? "data00.psarc" : "data01.psarc";
        for (size_t size = 0; size < archive->size(); ++size) {
            WriteFile(base / name, {archive->begin(), archive->begin() + size});
            for (const auto& [program, code] : Run({base}, index).code.by_program) {
                assert(code.size() >= 56);
            }
        }
        for (size_t at = 0; at < archive->size(); ++at) {
            auto changed = *archive;
            changed[at] ^= 0xFF;
            WriteFile(base / name, changed);
            for (const auto& [program, code] : Run({base}, index).code.by_program) {
                assert(code.size() >= 56);
            }
        }
        WriteFile(base / name, *archive);
    }
}

void TestWantedOnly() {
    const fs::path base = Base("base"), update = Update();
    // 0x33, in b.bin with 0x22, is not kept. The update's eboot.bin, big.bin, then b.bin
    // (data00.psarc's first entry): 0x22 is found there, so a.bin is not read, and not named.
    const auto one = Run({update, base}, Index(base, FullIndex), {0x22});
    assert(one.code.missing.empty());
    assert(one.code.by_program.size() == 1 && Has(one.code, 0x22, Blob(0x22)));
    assert(one.code.files_read == 3);
    assert(one.last_done == one.last_total && one.last_total == 5);
    // A wanted program no file holds: every file is read, as without a filter.
    const auto lost = Run({update, base}, Index(base, FullIndex), {0x22, 0x99});
    assert(lost.code.missing.empty() && lost.code.files_read == 4);
    assert(lost.code.by_program.size() == 1 && Has(lost.code, 0x22, Blob(0x22)));
}

void TestFirstReadableEboot() {
    const fs::path base = Base("base"), update = Update(), empty = Folder("empty-eboot");
    const fs::path index = Index(base, "eboot.bin\n");
    // The first folder's eboot.bin, empty, cannot be read: it is named and the next is read.
    WriteFile(empty / "eboot.bin", {});
    const auto next = Run({empty, update}, index);
    assert(next.code.missing.size() == 1 && Missing(next.code, "cannot read"));
    assert(next.code.files_read == 1 && Has(next.code, 0x55, Blob(0x55, {9, 9, 9, 9})));
    assert(next.last_done == 2 && next.last_total == 2);
    // A program only in a later folder's eboot.bin is not found when the first can be read: the
    // game runs the update's eboot.bin, so the game's is never read. That is the rule's cost.
    const auto first = Run({update, base}, index, {0x55, 0x66});
    assert(first.code.missing.empty() && first.code.files_read == 1);
    assert(first.code.by_program.size() == 1 && Has(first.code, 0x55, Blob(0x55, {9, 9, 9, 9})));
    assert(first.last_done == 2 && first.last_total == 2);
}

void TestSecondCopy() {
    const fs::path update = Folder("copy-update"), base = Folder("copy-base");
    const auto small = ReadFile(Data / "data00.psarc");
    const fs::path index = Index(base, "data00.psarc\ta.bin\ndata00.psarc\tb.bin\n");
    // The update's copy holds everything wanted: the game's copy, cut to 20 bytes, is not
    // opened. Read without a filter, it is named "cut off".
    WriteFile(update / "data00.psarc", small);
    WriteFile(base / "data00.psarc", {small.begin(), small.begin() + 20});
    const auto first = Run({update, base}, index, {0x11, 0x22});
    assert(first.code.missing.empty() && first.code.files_read == 2);
    assert(Has(first.code, 0x11, Blob(0x11)) && Has(first.code, 0x22, Blob(0x22)));
    assert(first.code.by_program.size() == 2);
    assert(first.last_done == 4 && first.last_total == 4);
    assert(Missing(Run({update, base}, index).code, "data00.psarc: cut off"));
    // b.bin's two TOC entries in the update's copy name no file: a.bin is read from it, b.bin
    // from the game's copy, whose first entry it is, and nothing after it.
    auto no_b = small;
    for (const u32 entry : {1u, 2u}) {
        no_b[32 + 30 * entry] ^= 0xFF;
    }
    WriteFile(update / "data00.psarc", no_b);
    WriteFile(base / "data00.psarc", small);
    const auto second = Run({update, base}, index, {0x11, 0x22});
    assert(second.code.missing.empty() && second.code.files_read == 2);
    assert(Has(second.code, 0x11, Blob(0x11)) && Has(second.code, 0x22, Blob(0x22)));
    assert(second.code.by_program.size() == 2);
    assert(second.last_done == 4 && second.last_total == 4);
}

} // namespace

int main() {
    TestMd5();
    TestReadsEveryIndexedFile();
    TestMissingFilesRejectOnlyTheirOwn();
    TestArchiveNamesAreFileNamesOnly();
    TestCaseAndDuplicates();
    TestCutArchives();
    TestWantedOnly();
    TestFirstReadableEboot();
    TestSecondCopy();
    fs::remove_all(fs::temp_directory_path() / "test_precompile_files");
    std::puts("PASS: precompile files");
    return 0;
}
