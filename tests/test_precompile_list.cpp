// File format of the precompile list (vk_precompile_list.h): what GOW3_PRECOMPILE_RECORD
// writes and GOW3_PRECOMPILE_LIST reads back in a later launch.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include "video_core/renderer_vulkan/vk_precompile_list.h"

using namespace Vulkan::Precompile;

namespace {

RecordedKey SampleKey(u64 seed) {
    constexpr u64 Table = 0x2000'0000, Nested = 0x3000'1000;
    RecordedKey key{};
    key.key_hash = 0x1234'5678'9ABC'DEF0ULL ^ seed;
    key.vertex_sgpr_offset = ListVsUserDataSgpr + 2;
    key.instance_sgpr_offset = 0;
    key.regs = {{0x2C0C, 0x100 + u32(seed)}, {0xA000, 7}, {ListRegsWords - 1, 9}};
    RecordedStage fragment{};
    fragment.sw_stage = 0;
    fragment.hw_stage = 0;
    fragment.hash = 0xF5F5'0000ULL + seed;
    fragment.spec_digest = 0x5EC0'0000ULL + seed;
    fragment.user_data[0] = u32(Table);
    fragment.roots = {{0, Table}};
    RecordedStage vertex{};
    vertex.sw_stage = 3;
    vertex.hw_stage = 1;
    vertex.hash = 0x7575'0000ULL + seed;
    vertex.spec_digest = 0x5EC1'0000ULL + seed;
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
    key = SampleKey(3);
    key.regs.push_back({ListPsInControlReg, 0x40 | (ListMaxPsInputs + 1)});
    rejected(key, "too many pixel shader inputs");
    for (const u32 offset : {1u, ListVsUserDataSgpr - 1, ListVsUserDataSgpr + ListUserDataWords,
                             0x10000 + ListVsUserDataSgpr}) {
        key = SampleKey(3);
        key.vertex_sgpr_offset = offset;
        rejected(key, "draw-indirect sgpr offset out of range");
        key = SampleKey(3);
        key.instance_sgpr_offset = offset;
        rejected(key, "draw-indirect sgpr offset out of range");
    }
    // The limits themselves are valid.
    key = SampleKey(3);
    key.regs.push_back({ListPsInControlReg, ListMaxPsInputs});
    key.vertex_sgpr_offset = ListVsUserDataSgpr;
    key.instance_sgpr_offset = ListVsUserDataSgpr + ListUserDataWords - 1;
    const auto valid = ParseList(File({key}), 100);
    assert(valid.keys.size() == 1 && valid.rejected == 0);

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

void TestCodeBody() {
    const char* check = "123456789";
    assert(Crc32({reinterpret_cast<const u8*>(check), 9}) == 0xCBF43926);
    // The header's crc32 covers the code from the start token through the header's first 16
    // bytes.
    constexpr u64 Hash = 0xC0DE'0000'2222ULL;
    auto code = SampleCode(Hash, 6, 40);
    auto* bytes = reinterpret_cast<u8*>(code.data());
    const size_t header = CodeHeaderAt(code) * 4;
    const u32 crc = Crc32({static_cast<const u8*>(bytes), header + 16});
    std::memcpy(bytes + header + CodeHeaderCrcAt, &crc, sizeof(crc));
    assert(!CheckCode(Hash, code) && !CheckCodeBody(code));
    code[2] ^= 0x100; // one body byte
    assert(!CheckCode(Hash, code) && CheckCodeBody(code));
}

void TestOldVersion() {
    // A version 1 list (no code) is another version: the boot needs the code.
    auto file = File({SampleKey(1)});
    const u32 version = 1;
    std::memcpy(file.data() + ListMagic.size(), &version, sizeof(version));
    const auto list = ParseList(file, 100);
    assert(list.status == ParsedList::Status::OtherVersion && list.version == 1);
}

void TestStagesDigest() {
    // Another launch records the same pipeline with other key hash, guest addresses, registers
    // and table words: the stages' programs and specs say it is the same.
    auto relaunched = SampleKey(1);
    relaunched.key_hash ^= 0xFF;
    relaunched.regs[0].second += 0x1000;
    relaunched.stages[0].user_data[0] += 0x40;
    relaunched.stages[0].roots[0].second += 0x40;
    for (auto& word : relaunched.words) {
        word.base += 0x40;
    }
    relaunched.words[2].value ^= 1;
    relaunched.pointers[0].base += 0x40;
    const auto digest = [](const RecordedKey& key) { return StagesDigest(key.stages); };
    assert(digest(relaunched) == digest(SampleKey(1)));
    // Another permutation of one stage, or another program, is a new entry.
    auto other_spec = SampleKey(1);
    other_spec.stages[1].spec_digest ^= 1;
    assert(digest(other_spec) != digest(SampleKey(1)));
    auto other_program = SampleKey(1);
    other_program.stages[0].hash ^= 1;
    assert(digest(other_program) != digest(SampleKey(1)));
    // The stages count as a whole: a key with only one of them is another entry.
    auto one_stage = SampleKey(1);
    one_stage.stages.pop_back();
    assert(digest(one_stage) != digest(SampleKey(1)));
}

void TestEntryDigest() {
    // The same stages drawn with another render state are another pipeline: both are kept.
    auto other_state = SampleKey(1);
    other_state.key_hash ^= 0xFF;
    other_state.regs[0].second += 0x1000;
    assert(StagesDigest(other_state.stages) == StagesDigest(SampleKey(1).stages));
    assert(EntryDigest(other_state) != EntryDigest(SampleKey(1)));
    // The same pipeline at other guest addresses is one entry.
    auto moved = SampleKey(1);
    moved.stages[0].user_data[0] += 0x40;
    for (auto& word : moved.words) {
        word.base += 0x40;
    }
    assert(EntryDigest(moved) == EntryDigest(SampleKey(1)));
    // Other stages under the same key hash are another entry.
    auto other_spec = SampleKey(1);
    other_spec.stages[1].spec_digest ^= 1;
    assert(EntryDigest(other_spec) != EntryDigest(SampleKey(1)));
}

void TestAppendKeeps() {
    // No file yet: written anew, without a reason to log.
    const auto none = PlanListAppend({});
    assert(!none.keep && !none.why && none.keys.empty());

    const auto key = SampleKey(1);
    const u64 fragment = key.stages[0].hash;
    auto file = EncodeListHeader();
    Append(file, EncodeListCode(fragment, SampleCode(fragment)));
    Append(file, EncodeListEntry(key));
    Append(file, EncodeListEntry(SampleKey(2)));
    const auto plan = PlanListAppend(file);
    assert(plan.keep && !plan.why && plan.bytes == file.size());
    assert(plan.keys == (std::set<u64>{EntryDigest(key), EntryDigest(SampleKey(2))}));
    assert(plan.code == std::set<u64>{fragment});
    assert(plan.entries == 2);
    // The entry cap counts entries, the dedupe set distinct stages.
    Append(file, EncodeListEntry(key));
    const auto twice = PlanListAppend(file);
    assert(twice.entries == 3 && twice.keys.size() == 2);

    // A header alone is kept as an empty list.
    const auto empty = PlanListAppend(EncodeListHeader());
    assert(empty.keep && empty.bytes == EncodeListHeader().size() && empty.keys.empty());
}

void TestAppendAfterCutOff() {
    // A launch that ended inside a write leaves a cut-off entry: it is dropped, so what is
    // appended after it still frames.
    const auto whole = File({SampleKey(1), SampleKey(2)});
    const size_t first_end = EncodeListHeader().size() + EncodeListEntry(SampleKey(1)).size();
    for (size_t size = first_end + 1; size < whole.size(); ++size) {
        const auto plan = PlanListAppend(std::span<const u8>{whole.data(), size});
        assert(plan.keep && plan.bytes == first_end);
        assert(plan.keys == std::set<u64>{EntryDigest(SampleKey(1))});
        std::vector<u8> file{whole.begin(), whole.begin() + plan.bytes};
        Append(file, EncodeListEntry(SampleKey(3)));
        const auto list = ParseList(file, 100);
        assert(list.rejected == 0 && list.keys.size() == 2 && list.keys[1] == SampleKey(3));
    }
    // A body that fails its checksum ends what is kept, with every entry after it: its size
    // field may be the bad part, and then nothing after it frames.
    auto corrupt = whole;
    corrupt[EncodeListHeader().size() + 4 + 8 + 20] ^= 0x40;
    const auto plan = PlanListAppend(corrupt);
    assert(plan.keep && plan.bytes == EncodeListHeader().size() && plan.keys.empty());
}

void TestAppendAfterBadSize() {
    // A bad size field mid-file: the next "entry" starts inside a body and cannot pass.
    const auto whole = File({SampleKey(1), SampleKey(2), SampleKey(3)});
    const size_t first_end = EncodeListHeader().size() + EncodeListEntry(SampleKey(1)).size();
    auto bad = whole;
    const u32 size = 16;
    std::memcpy(bad.data() + first_end, &size, sizeof(size));
    const auto plan = PlanListAppend(bad);
    assert(plan.keep && plan.bytes == first_end);
    assert(plan.keys == std::set<u64>{EntryDigest(SampleKey(1))});
    std::vector<u8> file{bad.begin(), bad.begin() + plan.bytes};
    Append(file, EncodeListEntry(SampleKey(4)));
    const auto list = ParseList(file, 100);
    assert(list.rejected == 0 && list.keys.size() == 2 && list.keys[1] == SampleKey(4));
}

void TestAppendAfterZeroTail() {
    // A power loss can leave the file's last blocks zero: each 12 zero bytes frame as an empty
    // entry whose checksum fails, so the zeros are dropped before appending.
    for (const size_t zeros : {1, 12, 40, 4096}) {
        auto file = File({SampleKey(1), SampleKey(2)});
        const size_t end = file.size();
        file.resize(end + zeros, 0);
        const auto plan = PlanListAppend(file);
        assert(plan.keep && plan.bytes == end && plan.keys.size() == 2);
        file.resize(plan.bytes);
        Append(file, EncodeListEntry(SampleKey(3)));
        const auto list = ParseList(file, 100);
        assert(list.rejected == 0 && list.keys.size() == 3);
    }
}

void TestAppendReplaces() {
    auto other_version = File({SampleKey(1)});
    const u32 version = ListVersion - 1;
    std::memcpy(other_version.data() + ListMagic.size(), &version, sizeof(version));
    const auto by_version = PlanListAppend(other_version);
    assert(!by_version.keep && by_version.why && by_version.keys.empty());

    auto other_layout = File({SampleKey(1)});
    const u32 stages = ListStages + 1;
    std::memcpy(other_layout.data() + ListMagic.size() + 12, &stages, sizeof(stages));
    assert(!PlanListAppend(other_layout).keep && PlanListAppend(other_layout).why);

    const std::vector<u8> garbage(64, 0x5A);
    assert(!PlanListAppend(garbage).keep && PlanListAppend(garbage).why);
    const auto header = EncodeListHeader();
    assert(!PlanListAppend(std::span<const u8>{header.data(), 10}).keep);
}

void TestRoomCountsKeptBytes() {
    // The size cap counts the kept entries too, so a list never grows past what loads.
    assert(ListHasRoom(0, 100));
    assert(ListHasRoom(MaxListBytes - 100, 100));
    assert(!ListHasRoom(MaxListBytes - 99, 100));
    assert(!ListHasRoom(0, MaxListEntryBytes + 1));
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
    TestCodeBody();
    TestOldVersion();
    TestStagesDigest();
    TestEntryDigest();
    TestAppendKeeps();
    TestAppendAfterCutOff();
    TestAppendAfterBadSize();
    TestAppendAfterZeroTail();
    TestAppendReplaces();
    TestRoomCountsKeptBytes();
    std::puts("precompile-list-test: ok");
    return 0;
}
