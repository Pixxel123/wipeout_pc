// File format of the precompile list (vk_precompile_list.h): what GOW3_PRECOMPILE_RECORD
// writes and GOW3_PRECOMPILE_LIST reads back in a later launch.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "video_core/renderer_vulkan/vk_precompile_list.h"

using namespace Vulkan::Precompile;

namespace {

RecordedKey SampleKey(u64 seed) {
    constexpr u64 Table = 0x2000'0000, Nested = 0x3000'1000;
    RecordedKey key{};
    key.key_hash = 0x1234'5678'9ABC'DEF0ULL ^ seed;
    key.vertex_sgpr_offset = 2;
    key.instance_sgpr_offset = 3;
    key.regs = {{0x2C0C, 0x100 + u32(seed)}, {0xA000, 7}, {ListRegsWords - 1, 9}};
    RecordedStage fragment{};
    fragment.sw_stage = 0;
    fragment.hw_stage = 0;
    fragment.hash = 0xF5F5'0000ULL + seed;
    fragment.user_data[0] = u32(Table);
    fragment.roots = {{0, Table}};
    RecordedStage vertex{};
    vertex.sw_stage = 3;
    vertex.hw_stage = 1;
    vertex.hash = 0x7575'0000ULL + seed;
    key.stages = {fragment, vertex};
    key.words = {{Table, 8, u32(Nested)}, {Table, 12, 0}, {Table, 64, 0xAAAA5555},
                 {Nested, 0, u32(seed)}};
    key.pointers = {{Table, 8, Nested}};
    return key;
}

std::vector<u8> File(const std::vector<RecordedKey>& keys) {
    auto file = EncodeListHeader();
    for (const auto& key : keys) {
        const auto entry = EncodeListEntry(key);
        file.insert(file.end(), entry.begin(), entry.end());
    }
    return file;
}

/// Synthetic program code: the start token, `header - 2` filler words, then the OrbShdr
/// header at dword `header` with `hash` and a length of `length` bytes.
std::vector<u32> SampleCode(u64 hash, u32 header = 4, u32 length = 24) {
    std::vector<u32> code(header + CodeHeaderWords, 0xBF810000);
    code[0] = CodeStartToken;
    code[1] = header / 2 - 1;
    auto* bytes = reinterpret_cast<u8*>(code.data() + header);
    std::memset(bytes, 0, CodeHeaderWords * 4);
    std::memcpy(bytes, CodeSignature.data(), CodeSignature.size());
    const u32 length_word = (length << 8) | 0x5;
    std::memcpy(bytes + CodeHeaderLengthAt, &length_word, sizeof(length_word));
    std::memcpy(bytes + CodeHeaderHashAt, &hash, sizeof(hash));
    return code;
}

void Append(std::vector<u8>& file, const std::vector<u8>& entry) {
    file.insert(file.end(), entry.begin(), entry.end());
}

void TestRoundTrip() {
    const std::vector<RecordedKey> keys = {SampleKey(1), SampleKey(2)};
    const auto list = ParseList(File(keys), 100);
    assert(list.status == ParsedList::Status::Ok);
    assert(list.version == ListVersion);
    assert(list.rejected == 0 && list.reasons.empty());
    assert(list.keys == keys);
    // A header alone is an empty list.
    const auto empty = ParseList(EncodeListHeader(), 100);
    assert(empty.status == ParsedList::Status::Ok && empty.keys.empty() && empty.rejected == 0);
}

void TestHeaderRejects() {
    auto file = File({SampleKey(1)});
    auto other_version = file;
    const u32 version = ListVersion + 1;
    std::memcpy(other_version.data() + ListMagic.size(), &version, sizeof(version));
    const auto by_version = ParseList(other_version, 100);
    assert(by_version.status == ParsedList::Status::OtherVersion);
    assert(by_version.version == version && by_version.keys.empty());

    auto other_layout = file;
    const u32 regs_words = ListRegsWords + 1;
    std::memcpy(other_layout.data() + ListMagic.size() + 4, &regs_words, sizeof(regs_words));
    assert(ParseList(other_layout, 100).status == ParsedList::Status::OtherLayout);

    auto not_a_list = file;
    not_a_list[0] = 'X';
    assert(ParseList(not_a_list, 100).status == ParsedList::Status::NotAList);
    assert(ParseList(std::span<const u8>{file.data(), 5}, 100).status ==
           ParsedList::Status::NotAList);
    // Cut inside the header after the magic.
    assert(ParseList(std::span<const u8>{file.data(), ListMagic.size() + 6}, 100).status !=
           ParsedList::Status::Ok);
}

void TestTruncated() {
    const auto file = File({SampleKey(1), SampleKey(2)});
    const size_t first_end = EncodeListHeader().size() + EncodeListEntry(SampleKey(1)).size();
    // Every cut inside the second entry keeps the first and drops the rest as cut off.
    for (size_t size = first_end + 1; size < file.size(); ++size) {
        const auto list = ParseList(std::span<const u8>{file.data(), size}, 100);
        assert(list.status == ParsedList::Status::Ok);
        assert(list.keys.size() == 1 && list.keys[0] == SampleKey(1));
        assert(list.rejected == 1);
        assert(list.reasons.size() == 1 && list.reasons[0] == "entry 1: cut off");
    }
}

void TestCorruptAndInvalid() {
    // A flipped byte in the first entry's body fails its checksum; the next entry still loads.
    auto corrupt = File({SampleKey(1), SampleKey(2)});
    corrupt[EncodeListHeader().size() + 4 + 8 + 20] ^= 0x40;
    const auto list = ParseList(corrupt, 100);
    assert(list.keys.size() == 1 && list.keys[0] == SampleKey(2));
    assert(list.rejected == 1 && list.reasons[0] == "entry 0: checksum mismatch");

    // Well framed, but unsafe to replay.
    const auto rejected = [](RecordedKey key, const char* why) {
        const auto parsed = ParseList(File({SampleKey(1), key}), 100);
        assert(parsed.keys.size() == 1 && parsed.rejected == 1);
        assert(parsed.reasons[0] == std::string{"entry 1: "} + why);
    };
    auto key = SampleKey(3);
    key.pointers[0].offset = 12; // its second word (16) was not recorded
    rejected(key, "pointer without its words");
    key = SampleKey(3);
    key.stages[0].roots[0].first = ListUserDataWords - 1;
    rejected(key, "user-data pointer out of range");
    key = SampleKey(3);
    key.stages[0].roots[0].first = 0xFFFFFFFFu; // sgpr + 1 would wrap to 0
    rejected(key, "user-data pointer out of range");
    key = SampleKey(3);
    key.stages[1].hw_stage = ListHwStages;
    rejected(key, "stage out of range");
    key = SampleKey(3);
    key.stages[1].sw_stage = 0;
    rejected(key, "stage twice");
    key = SampleKey(3);
    key.regs.push_back({ListRegsWords, 1});
    rejected(key, "register index out of range");
    key = SampleKey(3);
    key.words.push_back({1, MaxWordOffset + 4, 0});
    rejected(key, "guest word offset out of range");
    key = SampleKey(3);
    key.stages.clear();
    rejected(key, "bad stage count");

    // A count that claims more than the body holds is malformed, without allocating for it.
    auto huge = File({SampleKey(1)});
    const size_t body = EncodeListHeader().size() + 4 + 8;
    const u32 count = 0xFFFFFFFF;
    std::memcpy(huge.data() + body + 20, &count, sizeof(count)); // the register count
    const u64 checksum = Fnv1a(std::span<const u8>{huge.data() + body, huge.size() - body});
    std::memcpy(huge.data() + body - 8, &checksum, sizeof(checksum));
    const auto parsed = ParseList(huge, 100);
    assert(parsed.keys.empty() && parsed.reasons[0] == "entry 0: malformed");
}

void TestCap() {
    const auto list = ParseList(File({SampleKey(1), SampleKey(2), SampleKey(3)}), 2, 1);
    assert(list.keys.size() == 2 && list.rejected == 1);
    assert(list.reasons.size() == 1 && list.reasons[0] == "entry 2: past the entry cap");
}

void TestCode() {
    const auto key = SampleKey(1);
    const u64 fragment = key.stages[0].hash, vertex = key.stages[1].hash;
    auto file = EncodeListHeader();
    Append(file, EncodeListCode(fragment, SampleCode(fragment)));
    Append(file, EncodeListCode(vertex, SampleCode(vertex, 6, 40)));
    Append(file, EncodeListEntry(key));
    // Recorded again by a later key of the same program: kept once.
    Append(file, EncodeListCode(fragment, SampleCode(fragment)));
    const auto list = ParseList(file, 100);
    assert(list.status == ParsedList::Status::Ok && list.rejected == 0);
    assert(list.keys.size() == 1 && list.keys[0] == key);
    assert(list.code.size() == 2);
    assert(list.code.at(fragment) == SampleCode(fragment));
    assert(list.code.at(vertex) == SampleCode(vertex, 6, 40));
}

void TestCodeRejects() {
    static constexpr u64 Hash = 0xC0DE'0000'1111ULL;
    const auto rejected = [](const std::vector<u8>& record, const char* why) {
        auto file = EncodeListHeader();
        Append(file, EncodeListCode(Hash, SampleCode(Hash)));
        Append(file, record);
        const auto parsed = ParseList(file, 100);
        assert(parsed.code.size() == 1 && parsed.code.at(Hash) == SampleCode(Hash));
        assert(parsed.rejected == 1);
        assert(parsed.reasons[0] == std::string{"entry 1: "} + why);
    };
    // Another program's code under this hash: the header names the other one.
    rejected(EncodeListCode(0xBAD, SampleCode(Hash)), "code hash mismatch");
    // Same hash, other bytes.
    auto other = SampleCode(Hash);
    other[2] ^= 1;
    rejected(EncodeListCode(Hash, other), "code differs from an earlier record");
    auto code = SampleCode(Hash);
    code[0] = 0;
    rejected(EncodeListCode(Hash, code), "code without its start token");
    rejected(EncodeListCode(Hash, std::vector<u32>{CodeStartToken}),
             "code without its start token");
    code = SampleCode(Hash);
    code.pop_back(); // the header's last word
    rejected(EncodeListCode(Hash, code), "code header out of range");
    code = SampleCode(Hash);
    code[1] = 0xFFFFFFFF; // (code[1] + 1) * 2 must not wrap to 0
    rejected(EncodeListCode(Hash, code), "code header out of range");
    code = SampleCode(Hash);
    code[4] ^= 0xFF; // 'O' of OrbShdr
    rejected(EncodeListCode(Hash, code), "code header without its signature");
    rejected(EncodeListCode(Hash, SampleCode(Hash, 4, 0)), "code length out of range");
    rejected(EncodeListCode(Hash, SampleCode(Hash, 4, 49)), "code length out of range");

    // A word count past the bound or the body is malformed, without allocating for it.
    auto huge = EncodeListCode(Hash, SampleCode(Hash));
    const u32 count = 0xFFFFFFFF;
    std::memcpy(huge.data() + 4 + 8 + 4 + 8, &count, sizeof(count));
    const u64 checksum = Fnv1a(std::span<const u8>{huge.data() + 12, huge.size() - 12});
    std::memcpy(huge.data() + 4, &checksum, sizeof(checksum));
    rejected(huge, "malformed");
    rejected(EncodeListCode(Hash, std::vector<u32>(MaxCodeBytes / 4 + 1, CodeStartToken)),
             "malformed");
}

void TestOldVersion() {
    // A version 1 list (no code) is another version: the boot needs the code.
    auto file = File({SampleKey(1)});
    const u32 version = 1;
    std::memcpy(file.data() + ListMagic.size(), &version, sizeof(version));
    const auto list = ParseList(file, 100);
    assert(list.status == ParsedList::Status::OtherVersion && list.version == 1);
}

} // namespace

int main() {
    TestRoundTrip();
    TestHeaderRejects();
    TestTruncated();
    TestCorruptAndInvalid();
    TestCap();
    TestCode();
    TestCodeRejects();
    TestOldVersion();
    std::puts("precompile-list-test: ok");
    return 0;
}
