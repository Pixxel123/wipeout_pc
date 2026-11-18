// Shader scan of the precompile diagnostics (vk_precompile_scan.h): finds program code in a
// buffer of guest memory by its OrbShdr header and resolves where the code starts.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/renderer_vulkan/vk_precompile_scan.h"

using namespace Vulkan::Precompile;

namespace {

constexpr u64 MemoryBase = 0x10'0000'0000ULL;

/// Fake guest memory at MemoryBase, filled with a byte that is no signature or start token.
struct Memory {
    std::vector<u8> bytes = std::vector<u8>(0x10000, 0x5A);

    /// A program at `start` bytes in: the start token, the OrbShdr header at dword `header`
    /// with `hash` and a length of `length` bytes. Returns the header's offset.
    size_t Program(size_t start, u64 hash, u32 header, u32 length) {
        u32 token = CodeStartToken, at = header / 2 - 1;
        std::memcpy(bytes.data() + start, &token, sizeof(token));
        std::memcpy(bytes.data() + start + 4, &at, sizeof(at));
        return Header(start + header * 4, hash, length);
    }
    /// An OrbShdr header alone; returns its offset.
    size_t Header(size_t at, u64 hash, u32 length) {
        u8* header = bytes.data() + at;
        std::memset(header, 0, CodeHeaderWords * 4);
        std::memcpy(header, CodeSignature.data(), CodeSignature.size());
        const u32 length_word = (length << 8) | 0x5;
        std::memcpy(header + CodeHeaderLengthAt, &length_word, sizeof(length_word));
        std::memcpy(header + CodeHeaderHashAt, &hash, sizeof(hash));
        return at;
    }
    std::span<const u8> Span(size_t from, size_t to) const {
        return std::span<const u8>{bytes}.subspan(from, to - from);
    }
    ReadFn Reader() const {
        return [this](u64 address, void* out, size_t size) {
            if (address < MemoryBase || address - MemoryBase + size > bytes.size()) {
                return false;
            }
            std::memcpy(out, bytes.data() + (address - MemoryBase), size);
            return true;
        };
    }
};

void TestTwoPrograms() {
    Memory memory;
    memory.Program(0x100, 0x1111, 8, 64);
    memory.Program(0x400, 0x2222, 40, 400); // its length reaches past the header
    std::vector<FoundCode> out;
    FindShaderCode(memory.bytes, MemoryBase, memory.Reader(), out);
    assert((out == std::vector<FoundCode>{{0x1111, MemoryBase + 0x100},
                                          {0x2222, MemoryBase + 0x400}}));
}

void TestNoStart() {
    Memory memory;
    // A header alone, one whose start token puts it elsewhere, and one the recorder's check
    // rejects (a length of zero).
    memory.Header(0x200, 0x3333, 64);
    memory.Program(0x400, 0x4444, 8, 64);
    memory.Header(0x480, 0x5555, 64);
    memory.Program(0x800, 0x6666, 8, 0);
    std::vector<FoundCode> out;
    FindShaderCode(memory.bytes, MemoryBase, memory.Reader(), out);
    assert((out == std::vector<FoundCode>{{0x4444, MemoryBase + 0x400}}));
}

void TestStartBeforeBytes() {
    Memory memory;
    // The start lies two pages and more below the buffer, which begins off a page boundary.
    const size_t header = memory.Program(0x104, 0x7777, 0x2000 / 4, 0x2100);
    for (const size_t from : {size_t(0x2004), size_t(0x2000), size_t(0x1F00), header}) {
        std::vector<FoundCode> out;
        FindShaderCode(memory.Span(from, memory.bytes.size()), MemoryBase + from,
                       memory.Reader(), out);
        assert((out == std::vector<FoundCode>{{0x7777, MemoryBase + 0x104}}));
    }
    // Not found when the memory below the buffer cannot be read.
    const auto none = [](u64, void*, size_t) { return false; };
    std::vector<FoundCode> out;
    FindShaderCode(memory.Span(0x2004, memory.bytes.size()), MemoryBase + 0x2004, none, out);
    assert(out.empty());
}

void TestSplitAcrossBuffers() {
    Memory memory;
    const size_t header = memory.Program(0x100, 0x8888, 64, 256);
    // Two consecutive buffers as a pass reads them: the second begins with the last
    // ScanOverlap bytes of the first. Every cut through the header, or right after it.
    for (size_t cut = header + 1; cut <= header + CodeHeaderWords * 4 + 8; ++cut) {
        std::vector<FoundCode> out;
        FindShaderCode(memory.Span(0, cut), MemoryBase, memory.Reader(), out);
        FindShaderCode(memory.Span(cut - ScanOverlap, memory.bytes.size()),
                       MemoryBase + cut - ScanOverlap, memory.Reader(), out);
        assert((out == std::vector<FoundCode>{{0x8888, MemoryBase + 0x100}}));
    }
}

} // namespace

int main() {
    TestTwoPrograms();
    TestNoStart();
    TestStartBeforeBytes();
    TestSplitAcrossBuffers();
    return EXIT_SUCCESS;
}
