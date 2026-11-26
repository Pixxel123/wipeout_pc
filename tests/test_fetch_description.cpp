// SPDX-License-Identifier: GPL-2.0-or-later
// A fetch shader's parse through a precompile list (vk_precompile_fetch.h): the parse of an
// invented fetch shader, written to a list entry and read back, is what ParseFetchShader gives
// while the rebuild's override is set, field by field. Only invented code: the parser's
// documented shapes (fetch_shader.cpp), typed loads and the offset adds.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <future>
#include <vector>
#include "common/logging/log.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_precompile_fetch.h"

extern "C" { uint64_t runtime_disabled_optimizations = 0; } // gow3_toggles.h: 64 bits
void assert_fail_impl() { std::abort(); }
[[noreturn]] void unreachable_impl() { std::abort(); }
namespace Common::Log {
bool ShouldLog(Level) { return false; }
void Write(std::string_view, Level, const char*, int, const char*, std::string&&) {}
}

namespace {

using namespace Shader::Gcn;
using namespace Vulkan::Precompile;

// GCN encodings of the instructions the parser reads.
u32 LoadX4(u32 sdst, u32 sbase_pair, u32 dword_offset) { // s_load_dwordx4, immediate offset
    return 0xC0000000u | (2u << 22) | (sdst << 15) | (sbase_pair << 9) | (1u << 8) | dword_offset;
}
constexpr u32 WaitLgkm = 0xBF8C007Fu; // s_waitcnt lgkmcnt(0)
constexpr u32 WaitAll = 0xBF8C0000u;  // s_waitcnt 0
constexpr u32 Return = 0xBE802000u;   // s_setpc_b64 s[0:1]
/// buffer_load_format_x..xyzw (count 1-4) vdata, vaddr, s[srsrc*4..], 0 idxen offset:`offset`
std::array<u32, 2> BufferLoad(u32 count, u32 vdata, u32 vaddr, u32 srsrc, u32 offset = 0) {
    return {0xE0000000u | ((count - 1) << 18) | (1u << 13) | offset,
            vaddr | (vdata << 8) | (srsrc << 16) | (0x80u << 24)};
}
/// tbuffer_load_format_x..xyzw with a data and a number format.
std::array<u32, 2> TypedLoad(u32 count, u32 vdata, u32 vaddr, u32 srsrc, u32 dfmt, u32 nfmt,
                             u32 offset = 0) {
    return {0xE8000000u | (nfmt << 23) | (dfmt << 19) | ((count - 1) << 16) | (1u << 13) | offset,
            vaddr | (vdata << 8) | (srsrc << 16) | (0x80u << 24)};
}
u32 AddOffset(u32 vgpr, u32 sgpr) { // v_add_i32 v[vgpr], s[sgpr], v[vgpr]
    return (37u << 25) | (vgpr << 17) | (vgpr << 9) | sgpr;
}

struct Stub {
    std::vector<u32> code;
    u32 vertex_sgpr_offset = 0, instance_sgpr_offset = 0; ///< the key's draw-indirect offsets
};

void Add(std::vector<u32>& code, std::array<u32, 2> words) {
    code.insert(code.end(), words.begin(), words.end());
}

std::vector<Stub> Stubs() {
    std::vector<Stub> stubs;
    // Grouped: the loads, a wait, the buffer loads, a wait, the return.
    auto& grouped = stubs.emplace_back().code;
    grouped = {LoadX4(8, 1, 0), LoadX4(12, 1, 4), LoadX4(16, 1, 8), WaitLgkm};
    Add(grouped, BufferLoad(4, 4, 0, 2));
    Add(grouped, BufferLoad(3, 8, 0, 3));
    Add(grouped, BufferLoad(2, 12, 0, 4));
    grouped.insert(grouped.end(), {WaitAll, Return});
    // Interleaved: load, wait, buffer load, per attribute, through the same registers.
    auto& interleaved = stubs.emplace_back().code;
    interleaved = {LoadX4(4, 1, 0), WaitLgkm};
    Add(interleaved, BufferLoad(4, 4, 0, 1));
    interleaved.insert(interleaved.end(), {LoadX4(4, 1, 8), WaitLgkm});
    Add(interleaved, BufferLoad(1, 8, 0, 1, 0x24));
    interleaved.insert(interleaved.end(), {WaitAll, Return});
    // Typed loads per instance (index in v3, v1), with format overrides.
    auto& typed = stubs.emplace_back().code;
    typed = {LoadX4(8, 1, 12), LoadX4(12, 1, 16), WaitLgkm};
    Add(typed, TypedLoad(3, 20, 3, 2, 13, 7, 0x10));
    Add(typed, TypedLoad(2, 24, 1, 3, 10, 4));
    typed.insert(typed.end(), {WaitAll, Return});
    // Offset adds into v0 and v3, as an indirect draw's fetch shader has.
    auto& offsets = stubs.emplace_back();
    offsets.code = {AddOffset(0, 6), AddOffset(3, 7), LoadX4(8, 1, 0), WaitLgkm};
    Add(offsets.code, BufferLoad(4, 4, 0, 2));
    offsets.code.insert(offsets.code.end(), {WaitAll, Return});
    offsets.vertex_sgpr_offset = ListVsUserDataSgpr + 6;
    offsets.instance_sgpr_offset = ListVsUserDataSgpr + 7;
    return stubs;
}

constexpr u32 FetchSgpr = 4;
constexpr u64 VertexHash = 0x7575'0000'1234ULL;

/// An Info of the vertex program `hash` whose fetch shader pointer, at FetchSgpr, is `code`.
struct Program {
    std::array<u32, 16> ud{};
    Shader::Info info;
    Program(const u32* code, u64 hash) {
        std::memcpy(ud.data() + FetchSgpr, &code, sizeof(code));
        info.has_fetch_shader = true;
        info.fetch_shader_sgpr_base = FetchSgpr;
        info.pgm_hash = hash;
        info.user_data = ud;
    }
};

/// Every field, also those operator== leaves out (size, inst_offset, the format overrides).
bool Same(const FetchShaderData& a, const FetchShaderData& b) {
    if (a.size != b.size || a.vertex_offset_sgpr != b.vertex_offset_sgpr ||
        a.instance_offset_sgpr != b.instance_offset_sgpr ||
        a.attributes.size() != b.attributes.size()) {
        return false;
    }
    for (size_t i = 0; i < a.attributes.size(); ++i) {
        const auto& x = a.attributes[i];
        const auto& y = b.attributes[i];
        if (x.semantic != y.semantic || x.dest_vgpr != y.dest_vgpr ||
            x.num_elements != y.num_elements || x.sgpr_base != y.sgpr_base ||
            x.dword_offset != y.dword_offset || x.instance_data != y.instance_data ||
            x.inst_offset != y.inst_offset || x.data_format != y.data_format ||
            x.num_format != y.num_format) {
            return false;
        }
    }
    return true;
}

/// A list entry of the vertex program VertexHash with `fetch`, written and read back.
RecordedKey ThroughList(const FetchDescription& fetch, const Stub& stub) {
    RecordedKey key{};
    key.key_hash = 1;
    key.vertex_sgpr_offset = stub.vertex_sgpr_offset;
    key.instance_sgpr_offset = stub.instance_sgpr_offset;
    RecordedStage fragment{};
    fragment.hash = 0xF5F5;
    RecordedStage vertex{};
    vertex.sw_stage = ListVertexStage;
    vertex.hw_stage = 1;
    vertex.hash = VertexHash;
    vertex.roots = {{FetchSgpr, 0x1000}};
    key.stages = {fragment, vertex};
    key.fetch = fetch;
    auto file = EncodeListHeader();
    const auto entry = EncodeListEntry(key);
    file.insert(file.end(), entry.begin(), entry.end());
    const auto list = ParseList(file, 10);
    assert(list.status == ParsedList::Status::Ok && list.rejected == 0);
    assert(list.keys.size() == 1 && list.keys[0] == key);
    return list.keys[0];
}

void TestRoundTrips() {
    bool typed = false, offsets = false, inst_offset = false, per_instance = false;
    for (const auto& stub : Stubs()) {
        Program recorded{stub.code.data(), VertexHash};
        const auto parsed = ParseFetchShader(recorded.info);
        assert(parsed && parsed->size == stub.code.size() * 4);
        for (const auto& a : parsed->attributes) {
            typed = typed || a.data_format || a.num_format;
            inst_offset = inst_offset || a.inst_offset;
            per_instance = per_instance || a.instance_data;
        }
        offsets = offsets || parsed->vertex_offset_sgpr != -1;
        const auto key = ThroughList(DescribeFetch(*parsed, FetchSgpr), stub);
        const auto parse = FetchOverrideOf(key);
        assert(parse && parse->pgm_hash == VertexHash && parse->sgpr_base == FetchSgpr);

        // The rebuild's Info: its fetch shader pointer leads nowhere.
        Program rebuilt{nullptr, VertexHash};
        {
            const ScopedFetchShaderOverride scope{&*parse};
            assert(FetchShaderOverridden());
            const auto given = ParseFetchShader(rebuilt.info);
            assert(given && Same(*given, *parsed));
        }
        assert(!FetchShaderOverridden());
    }
    // Every field took a value other than its default in some stub.
    assert(typed && offsets && inst_offset && per_instance);
}

void TestOverrideScope() {
    const auto stubs = Stubs();
    Program own{stubs[1].code.data(), VertexHash};
    const auto own_parse = ParseFetchShader(own.info);
    const auto parse = FetchOverrideOf(
        ThroughList(DescribeFetch(*ParseFetchShader(Program{stubs[0].code.data(), 0}.info),
                                  FetchSgpr),
                    stubs[0]));
    assert(parse && !Same(parse->data, *own_parse));
    const ScopedFetchShaderOverride scope{&*parse};
    assert(Same(*ParseFetchShader(own.info), parse->data));
    // Another program, or another fetch shader register: its own code is parsed.
    Program other{stubs[1].code.data(), VertexHash + 1};
    assert(Same(*ParseFetchShader(other.info), *own_parse));
    Program moved{stubs[1].code.data(), VertexHash};
    moved.info.fetch_shader_sgpr_base = FetchSgpr + 2;
    std::memcpy(moved.ud.data() + FetchSgpr + 2, moved.ud.data() + FetchSgpr, 8);
    assert(Same(*ParseFetchShader(moved.info), *own_parse));
    // No fetch shader: none, as without the override.
    Program none{stubs[1].code.data(), VertexHash};
    none.info.has_fetch_shader = false;
    assert(!ParseFetchShader(none.info));
    // Another thread (a game draw) does not see it.
    std::async(std::launch::async, [&] {
        assert(!FetchShaderOverridden());
        assert(Same(*ParseFetchShader(own.info), *own_parse));
    }).get();
    // A null scope inside switches it off; its end puts it back.
    {
        const ScopedFetchShaderOverride off{nullptr};
        assert(!FetchShaderOverridden());
        assert(Same(*ParseFetchShader(own.info), *own_parse));
    }
    assert(Same(*ParseFetchShader(own.info), parse->data));
    // A compiler snapshot of the Info comes first.
    FetchShaderSnapshot snapshot;
    snapshot.info = &own.info;
    {
        const ScopedFetchShaderSnapshot inner{snapshot};
        assert(!ParseFetchShader(own.info));
    }
}

void TestEveryFieldInTheList() {
    // The list keeps all nine bytes of an attribute, also those operator== leaves out.
    const FetchDescription fetch{
        40, 2, -1, 6, {{0, 4, 4, 2, 0, 3, 0x22, 13, 7}, {1, 8, 2, 2, 4, 0, 0, 0, 0}}};
    Stub stub;
    stub.vertex_sgpr_offset = ListVsUserDataSgpr + 2;
    const auto key = ThroughList(fetch, stub);
    assert(key.fetch == fetch);
    const auto parse = FetchOverrideOf(key);
    assert(parse->data.size == 40 && parse->data.vertex_offset_sgpr == 2);
    assert(parse->data.attributes[0].inst_offset == 0x22);
    const auto& first = parse->data.attributes[0];
    assert(first.data_format == 13 && first.num_format == 7);
    assert(DescribeFetch(parse->data, fetch.sgpr_base) == fetch);
}

} // namespace

int main() {
    TestRoundTrips();
    TestOverrideScope();
    TestEveryFieldInTheList();
    std::puts("fetch-description-test: ok");
    return 0;
}
