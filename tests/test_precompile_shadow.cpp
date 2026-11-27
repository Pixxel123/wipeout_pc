// Shadow memory layout and record slots of the precompile replay diagnostic
// (vk_precompile_shadow.h).
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

using namespace Vulkan::Precompile;

namespace {

u32 ReadU32(const u8* at) {
    u32 value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

u64 ReadU64(const u8* at) {
    u64 value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

void TestLayout() {
    constexpr size_t Page = 4096;
    constexpr u64 Table = 0x2000'0000, Nested = 0x3000'1000, Code = 0x4000'0040;
    // A table read at 8..15 and 64 (a pointer at 8), a nested table at 0..15, and a 5000-byte
    // code range that needs two pages.
    std::vector<GuestWord> words = {
        {Table, 8, 0x11111111}, {Table, 12, 0x22222222}, {Table, 64, 0xAAAA5555},
        {Nested, 0, 1}, {Nested, 4, 2}, {Nested, 8, 3}, {Nested, 12, 4},
    };
    for (u32 i = 0; i < 1250; ++i) {
        words.push_back({Code, i * 4, 0xC0DE0000 | i});
    }
    const std::vector<GuestPointer> pointers = {{Table, 8, Nested}};
    const ShadowLayout layout{words, Page};

    // One guard page per region, each right after the region's last word.
    // Pages: table 1 + guard, nested table 1 + guard, code 2 + guard.
    assert(layout.GuardPages().size() == 3);
    assert(layout.Size() == 7 * Page);
    for (const size_t guard : layout.GuardPages()) {
        assert(guard % Page == 0);
    }

    std::vector<u8> memory(layout.Size());
    const u64 address = 0x7000'0000'0000ULL;
    const u64 dead = 0xDEAD000;
    layout.Fill(memory.data(), address, words, pointers, dead);
    const auto at = [&](u64 base, u32 offset) {
        const u64 shadow = *layout.Translate(base, address) + offset;
        assert(shadow >= address && shadow + 4 <= address + memory.size());
        return memory.data() + (shadow - address);
    };
    // The words sit at their offsets from the translated base; gaps read as zero.
    assert(ReadU32(at(Table, 64)) == 0xAAAA5555);
    assert(ReadU32(at(Table, 16)) == 0);
    assert(ReadU32(at(Nested, 12)) == 4);
    assert(ReadU32(at(Code, 4996)) == (0xC0DE0000 | 1249));
    // The pointer now leads to the nested table's shadow copy; its high bits stay.
    assert(ReadU64(at(Table, 8)) == ((0x2222'2222'1111'1111ULL & ~GuestAddressMask) |
                                     *layout.Translate(Nested, address)));
    // The last word of each region touches its guard page.
    for (const auto& [base, last] : {std::pair{Table, 64u}, {Nested, 12u}, {Code, 4996u}}) {
        const u64 end = *layout.Translate(base, address) + last + 4 - address;
        bool before_guard = false;
        for (const size_t guard : layout.GuardPages()) {
            before_guard |= end == guard;
        }
        assert(before_guard);
    }
    // Nothing read through it: no shadow address.
    assert(!layout.Translate(0x5000'0000, address));

    // A pointer to a table nothing was read from points at the dead page.
    const std::vector<GuestPointer> to_nowhere = {{Table, 8, 0x6000'0000}};
    std::vector<u8> other(layout.Size());
    layout.Fill(other.data(), address, words, to_nowhere, dead);
    assert((ReadU64(other.data() + (*layout.Translate(Table, address) + 8 - address)) &
            GuestAddressMask) == dead);
}

void TestRepoint() {
    // A V# whose first half is a table pointer: the stride (bits 48-61) must survive.
    constexpr u64 Sharp = 0x3FFF'0000'0000'0000ULL | 0x12'3456'7800ULL;
    constexpr u64 Shadow = 0x7F12'3456'7000ULL;
    static_assert(Repoint(Sharp, Shadow) == (0x3FFF'0000'0000'0000ULL | Shadow));
    static_assert(Repoint(0x1000, Shadow) == Shadow);
}

void TestEmptyLayout() {
    const ShadowLayout layout{{}, 4096};
    assert(layout.Size() == 0);
    assert(layout.GuardPages().empty());
}

void TestSlots() {
    constexpr u64 Address = 0x7100'0000'0000ULL, SlotSize = 16ull << 20;
    RecordSlots slots{Address, SlotSize, 4};
    // Slot 0 is the user data.
    assert(slots.SlotOf(Address) == 0u);
    assert(slots.Base(0) == 0);
    // Pointers get slots in order and keep them; the last slot stays the dead end.
    assert(slots.Find(0x1000) == 1u);
    assert(slots.Find(0x2000) == 2u);
    assert(slots.Find(0x1000) == 1u);
    assert(!slots.Find(0x3000));
    assert(slots.Dead() == Address + 3 * SlotSize);
    assert(slots.Base(2) == 0x2000);
    // Only a slot's own fake pointer names it, and only once handed out.
    assert(slots.SlotOf(Address + SlotSize) == 1u);
    assert(!slots.SlotOf(Address + SlotSize + 8));
    assert(!slots.SlotOf(slots.Dead()));
    assert(!slots.SlotOf(Address - SlotSize));
    slots.Reset();
    assert(!slots.SlotOf(Address + SlotSize));
    assert(slots.Find(0x2000) == 1u);
}

// Guest memory for the recorder: two tables, the first pointing at the second.
constexpr u64 TableA = 0x2000'0000, TableB = 0x3000'0000;
bool ReadFake(u64 address, void* out, size_t size) {
    u64 value = 0;
    if (address == TableA + 8 && size == 8) {
        value = 0xFFFF'0000'0000'0000ULL | TableB; // high bits the walker masks off
    } else if (address == TableA + 16 && size == 4) {
        value = 0x1234;
    } else if (address == TableB + 4 && size == 4) {
        value = 0x5678;
    } else {
        return false;
    }
    std::memcpy(out, &value, size);
    return true;
}

void TestRecorder() {
    constexpr u64 Address = 0x7100'0000'0000ULL, SlotSize = 16ull << 20;
    RecordSlots slots{Address, SlotSize, 8};
    std::vector<u32> user_data(16);
    const u64 root = TableA;
    std::memcpy(&user_data[4], &root, sizeof(root));
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
    std::vector<std::pair<u32, u64>> roots;
    words.reserve(16);
    pointers.reserve(4);
    roots.reserve(4);
    WalkRecorder recorder{slots, user_data, ReadFake, words, pointers, roots};

    // The walker's root load (s[4:5]) gets the fake pointer of table A.
    const u64 a = recorder.Load(slots.Address(0), slots.Address(0) + 16, true);
    assert(a == slots.Address(1) && slots.Base(1) == TableA);
    assert(roots.size() == 1 && roots[0].first == 4 && roots[0].second == TableA);
    // A data word of table A, then the pointer at A+8 to table B (masked to 48 bits).
    assert(recorder.Load(a, a + 16, false) == 0x1234);
    const u64 b = recorder.Load(a, a + 8, true);
    assert(b == slots.Address(2) && slots.Base(2) == TableB);
    assert(recorder.Load(b, b + 4, false) == 0x5678);
    assert(!recorder.error);
    // Recorded: the data words and both halves of the pointer, with the guest pointer each was
    // read through; the pointer itself with its target.
    assert(words.size() == 4);
    assert(words[0].base == TableA && words[0].offset == 16 && words[0].value == 0x1234);
    assert(words[1].base == TableA && words[1].offset == 8 && words[1].value == u32(TableB));
    assert(words[2].offset == 12 && words[2].value == 0xFFFF0000);
    assert(words[3].base == TableB && words[3].offset == 4 && words[3].value == 0x5678);
    assert(pointers.size() == 1 && pointers[0].base == TableA && pointers[0].offset == 8 &&
           pointers[0].target == TableB);

    // The recorded words replay through a shadow layout to the same values.
    const ShadowLayout layout{words, 4096};
    std::vector<u8> memory(layout.Size());
    const u64 shadow = 0x7200'0000'0000ULL;
    layout.Fill(memory.data(), shadow, words, pointers, 0);
    const u64 b_pointer = ReadU64(memory.data() + (*layout.Translate(TableA, shadow) + 8 - shadow));
    assert(b_pointer >> 48 == 0xFFFF); // the bits the walker masks off, as recorded
    const u64 b_shadow = b_pointer & GuestAddressMask;
    assert(b_shadow == *layout.Translate(TableB, shadow));
    assert(ReadU32(memory.data() + (b_shadow + 4 - shadow)) == 0x5678);

    // An unreadable word fails the recording; the walk goes on with zeros and dead pointers.
    assert(recorder.Load(b, b + 64, false) == 0);
    assert(recorder.error && std::strcmp(recorder.error, "guest word unreadable") == 0);
    assert(recorder.Load(a, a + 8, true) == slots.Dead());
    assert(words.size() == 4);
}

void TestRecorderLimits() {
    constexpr u64 Address = 0x7100'0000'0000ULL, SlotSize = 16ull << 20;
    RecordSlots slots{Address, SlotSize, 8};
    std::vector<u32> user_data(16);
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
    std::vector<std::pair<u32, u64>> roots;
    words.reserve(16);
    roots.reserve(4);
    {
        // Past the user data, or through a pointer it never handed out.
        WalkRecorder recorder{slots, user_data, ReadFake, words, pointers, roots};
        assert(recorder.Load(slots.Address(0), slots.Address(0) + 60, true) == slots.Dead());
        assert(std::strcmp(recorder.error, "read past the user data") == 0);
    }
    {
        WalkRecorder recorder{slots, user_data, ReadFake, words, pointers, roots};
        assert(recorder.Load(slots.Address(3), slots.Address(3) + 4, false) == 0);
        assert(std::strcmp(recorder.error, "read through an unknown pointer") == 0);
    }
    {
        // No room left for a word: refused before reading, never by growing the vector.
        std::vector<GuestWord> full;
        full.reserve(1);
        slots.Reset();
        const u64 root = TableA;
        std::memcpy(&user_data[0], &root, sizeof(root));
        WalkRecorder recorder{slots, user_data, ReadFake, full, pointers, roots};
        const u64 a = recorder.Load(slots.Address(0), slots.Address(0), true);
        assert(recorder.Load(a, a + 16, false) == 0);
        assert(std::strcmp(recorder.error, "too many guest words") == 0);
        assert(full.empty() && full.capacity() == 1);
    }
}

} // namespace

int main() {
    TestLayout();
    TestRepoint();
    TestEmptyLayout();
    TestSlots();
    TestRecorder();
    TestRecorderLimits();
    return EXIT_SUCCESS;
}
