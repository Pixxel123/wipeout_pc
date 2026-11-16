// SPDX-License-Identifier: GPL-2.0-or-later
// The permutation probe (vk_spec_probe.h) against the specialization it stands in for: over
// random sharps, vertex buffers, fetch shaders, runtime infos and start bindings, it must answer
// exactly `stored == StageSpecialization(info, runtime_info, profile, start)`.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <vector>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_spec_probe.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
}
void assert_fail_impl() {
    std::fprintf(stderr, "ASSERT failed in the code under test\n");
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    std::fprintf(stderr, "UNREACHABLE reached in the code under test\n");
    std::abort();
}
namespace Common::Log {
bool ShouldLog(Level) {
    return false;
}
void Write(std::string_view, Level, const char*, int, const char*, std::string&&) {}
} // namespace Common::Log

namespace {

using Shader::SharpLocation;
using Shader::StageSpecialization;
using Rng = std::mt19937_64;

u32 Pick(Rng& rng, u32 n) {
    return static_cast<u32>(rng() % n);
}
bool Chance(Rng& rng, u32 percent) {
    return Pick(rng, 100) < percent;
}

// Number formats that every format helper accepts (SnormNz reaches UNREACHABLE for most data
// formats in MapNumberConversion).
constexpr u32 kBufferNfmts[] = {0, 1, 2, 3, 4, 5, 7};
constexpr u32 kImageNfmts[] = {0, 1, 2, 3, 4, 5, 7, 9, 10, 12, 13};

constexpr u32 kFloat = 7, kUint = 4, kFormat32x4 = 14;

AmdGpu::Buffer RandomBuffer(Rng& rng, bool allow_invalid = true) {
    AmdGpu::Buffer buffer{};
    buffer.base_address = rng() & 0xFFFFFFFFFFull;
    buffer.stride = Pick(rng, 4) == 0 ? 0 : Pick(rng, 1u << 14);
    buffer.swizzle_enable = Chance(rng, 30);
    buffer.num_records = allow_invalid && Chance(rng, 20) ? 0 : 1 + Pick(rng, 1000);
    buffer.dst_sel_x = Pick(rng, 8);
    buffer.dst_sel_y = Pick(rng, 8);
    buffer.dst_sel_z = Pick(rng, 8);
    buffer.dst_sel_w = Pick(rng, 8);
    buffer.num_format = kBufferNfmts[Pick(rng, std::size(kBufferNfmts))];
    buffer.data_format = Pick(rng, 16);
    buffer.element_size = Pick(rng, 4);
    buffer.index_stride = Pick(rng, 4);
    buffer.type = allow_invalid && Chance(rng, 5) ? 1 + Pick(rng, 3) : 0;
    return buffer;
}

AmdGpu::Image RandomImage(Rng& rng, bool allow_invalid = true) {
    AmdGpu::Image image{};
    std::array<u64, 4> words{rng(), rng(), rng(), rng()};
    std::memcpy(&image, words.data(), sizeof(image));
    image.base_address = allow_invalid && Chance(rng, 15) ? 0 : 1 + Pick(rng, 1u << 20);
    image.type = allow_invalid && Chance(rng, 10) ? Pick(rng, 8) : 8 + Pick(rng, 8);
    image.num_format = kImageNfmts[Pick(rng, std::size(kImageNfmts))];
    image.data_format = Pick(rng, 64);
    return image;
}

AmdGpu::Sampler RandomSampler(Rng& rng) {
    AmdGpu::Sampler sampler{};
    if (!Chance(rng, 15)) {
        sampler.raw0 = rng();
        sampler.raw1 = rng() | 1;
    }
    return sampler;
}

/// One shader stage's inputs, as GetProgram sees them: the Info with its resource lists and
/// flattened user data, the user data with the fetch shader and vertex buffer pointers, and
/// the guest memory those point at. Not movable: the Info points into it.
struct Stage {
    static constexpr u32 FlatBase = 16;
    static constexpr u32 MaxAttributes = 4;

    std::vector<u32> code = std::vector<u32>(64, 0);
    std::array<AmdGpu::Buffer, MaxAttributes> vsharps{};
    std::array<u32, 16> ud{};
    Shader::Info info;
    Shader::RuntimeInfo rt{};
    Shader::Backend::Bindings start{};

    struct Load {
        u32 dfmt, nfmt, offset, vaddr;
    };
    std::vector<Load> loads;

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    explicit Stage(Shader::SwStage stage) {
        const auto hw = stage == Shader::SwStage::Vertex     ? Shader::HwStage::Vertex
                        : stage == Shader::SwStage::Fragment ? Shader::HwStage::Fragment
                                                             : Shader::HwStage::Compute;
        info.hw_stage = hw;
        info.sw_stage = stage;
        rt.Initialize(hw, stage);
        const u32* code_pointer = code.data();
        const AmdGpu::Buffer* vsharps_pointer = vsharps.data();
        std::memcpy(ud.data(), &code_pointer, sizeof(code_pointer));
        std::memcpy(ud.data() + 2, &vsharps_pointer, sizeof(vsharps_pointer));
        info.user_data = ud;
        info.fetch_shader_sgpr_base = 0;
    }

    /// One s_load_dwordx4 per attribute (V# k at dword 4k of the table in s[2:3]), the typed
    /// loads, then s_setpc_b64.
    void WriteCode() {
        std::fill(code.begin(), code.end(), 0);
        u32 at = 0;
        for (u32 k = 0; k < loads.size(); ++k) {
            const u32 sdst = 8 + 4 * k;
            code[at++] = 0xC0000000u | (2u << 22) | (sdst << 15) | (1u << 9) | (1u << 8) | (4 * k);
        }
        for (u32 k = 0; k < loads.size(); ++k) {
            const auto& load = loads[k];
            code[at++] = 0xE8000000u | (3u << 16) | (load.dfmt << 19) | (load.nfmt << 23) |
                         (load.offset & 0xFFF);
            code[at++] = load.vaddr | ((4 + 4 * k) << 8) | ((2 + k) << 16);
        }
        code[at++] = 0xBE802000u;
    }

    void SetFlat(u32 offset, const void* data, size_t size) {
        std::memcpy(info.flattened_ud_buf.data() + offset, data, size);
    }

    // Resource layout in the flattened buffer: buffers, images, fmasks, samplers, each sharp at
    // its own offset.
    u32 BufferOffset(u32 i) const {
        return FlatBase + 4 * i;
    }
    u32 ImageOffset(u32 i) const {
        return FlatBase + 4 * u32(info.buffers.size()) + 8 * i;
    }
    u32 FMaskOffset(u32 i) const {
        return ImageOffset(u32(info.images.size())) + 8 * i;
    }
    u32 SamplerOffset(u32 i) const {
        return FMaskOffset(u32(info.fmasks.size())) + 4 * i;
    }

    void Layout(u32 buffers, u32 images, u32 fmasks, u32 samplers, Rng& rng) {
        info.buffers.clear();
        info.images.clear();
        info.fmasks.clear();
        info.samplers.clear();
        for (u32 i = 0; i < buffers; ++i) {
            auto& desc = info.buffers.emplace_back();
            desc.is_formatted = Chance(rng, 50);
            desc.sharp_fetch.load_mask = 0xF;
            for (u32 w = 0; w < 4; ++w) {
                desc.sharp_fetch.offsets[w] = SharpLocation(FlatBase + 4 * i + w);
            }
            if (Chance(rng, 5)) {
                desc.sharp_fetch.offsets[0] = Shader::UNKNOWN_LOCATION; // never bound
            } else if (Chance(rng, 10)) {
                desc.post_op = Shader::SharpFetchPostOp::OffsetByProgramBase;
            } else if (Chance(rng, 10)) {
                desc.post_op = Shader::SharpFetchPostOp::BitwiseOrDw1WithImm;
                desc.post_op_dw1_mask = u32(rng()) & 0x7FFF0000u;
            }
        }
        for (u32 i = 0; i < images; ++i) {
            auto& desc = info.images.emplace_back();
            desc.is_array = Chance(rng, 30);
            desc.is_written = Chance(rng, 30);
            desc.is_depth = Chance(rng, 10);
            desc.mip_fallback_mode = Chance(rng, 20) ? Shader::MipStorageFallbackMode::DynamicIndex
                                                     : Shader::MipStorageFallbackMode::None;
            if (Chance(rng, 10)) {
                desc.post_op = Shader::SharpFetchPostOp::ConvertCubeTo2DArray;
            }
            desc.sharp_fetch.load_mask = 0xFF;
            for (u32 w = 0; w < 8; ++w) {
                desc.sharp_fetch.offsets[w] = SharpLocation(ImageOffset(i) + w);
            }
        }
        for (u32 i = 0; i < fmasks; ++i) {
            info.fmasks.push_back({SharpLocation(FMaskOffset(i))});
        }
        for (u32 i = 0; i < samplers; ++i) {
            auto& desc = info.samplers.emplace_back();
            desc.sharp_fetch.load_mask = 0xF;
            for (u32 w = 0; w < 4; ++w) {
                desc.sharp_fetch.offsets[w] = SharpLocation(SamplerOffset(i) + w);
            }
        }
        info.flattened_ud_buf.assign(SamplerOffset(samplers) + 4, 0);
    }

    void RandomizeBuffer(u32 i, Rng& rng) {
        const auto sharp = RandomBuffer(rng);
        SetFlat(BufferOffset(i), &sharp, sizeof(sharp));
    }
    void RandomizeImage(u32 i, Rng& rng) {
        const auto sharp = RandomImage(rng);
        SetFlat(ImageOffset(i), &sharp, sizeof(sharp));
    }
    void RandomizeFMask(u32 i, Rng& rng) {
        const auto sharp = RandomImage(rng);
        SetFlat(FMaskOffset(i), &sharp, sizeof(sharp));
    }
    void RandomizeSampler(u32 i, Rng& rng) {
        const auto sharp = RandomSampler(rng);
        SetFlat(SamplerOffset(i), &sharp, sizeof(sharp));
    }
    Load RandomLoad(Rng& rng) {
        const bool typed = Chance(rng, 60);
        return {typed ? Pick(rng, 16) : 0u, typed ? kBufferNfmts[Pick(rng, 7)] : 0u,
                Chance(rng, 50) ? 0u : Pick(rng, 256), Pick(rng, 4)};
    }

    void Randomize(Rng& rng) {
        Layout(Pick(rng, 5), Pick(rng, 5), Pick(rng, 3), Pick(rng, 4), rng);
        for (u32 i = 0; i < info.buffers.size(); ++i) {
            RandomizeBuffer(i, rng);
        }
        for (u32 i = 0; i < info.images.size(); ++i) {
            RandomizeImage(i, rng);
        }
        for (u32 i = 0; i < info.fmasks.size(); ++i) {
            RandomizeFMask(i, rng);
        }
        for (u32 i = 0; i < info.samplers.size(); ++i) {
            RandomizeSampler(i, rng);
        }
        info.has_fetch_shader = info.sw_stage == Shader::SwStage::Vertex ? !Chance(rng, 10)
                                                                         : Chance(rng, 5);
        loads.clear();
        for (u32 k = 0, n = 1 + Pick(rng, MaxAttributes); k < n; ++k) {
            loads.push_back(RandomLoad(rng));
        }
        WriteCode();
        for (auto& sharp : vsharps) {
            sharp = RandomBuffer(rng);
        }
        info.pgm_base = rng() & 0xFFFFFFFF00ull;
        if (info.sw_stage == Shader::SwStage::Vertex) {
            rt.sw.vs.step_rate_0 = Pick(rng, 4);
            rt.sw.vs.step_rate_1 = Pick(rng, 4);
        }
        rt.props.num_user_data = Pick(rng, 16);
        start = {Pick(rng, 8), Pick(rng, 8), Pick(rng, 8)};
    }

    /// Keeps flipped bits out of SnormNz (6), which the format helpers do not accept for most
    /// data formats (the guest does not use it there either).
    void Sanitize() {
        for (u32 i = 0; i < info.buffers.size(); ++i) {
            AmdGpu::Buffer sharp;
            std::memcpy(&sharp, info.flattened_ud_buf.data() + BufferOffset(i), sizeof(sharp));
            if (sharp.num_format == 6) {
                sharp.num_format = kFloat;
                SetFlat(BufferOffset(i), &sharp, sizeof(sharp));
            }
        }
        for (u32 i = 0; i < info.images.size(); ++i) {
            AmdGpu::Image sharp;
            std::memcpy(&sharp, info.flattened_ud_buf.data() + ImageOffset(i), sizeof(sharp));
            if (sharp.num_format == 6) {
                sharp.num_format = kFloat;
                SetFlat(ImageOffset(i), &sharp, sizeof(sharp));
            }
        }
    }

    /// Changes one input the way a later draw can: a sharp word, a vertex buffer, a runtime
    /// info field, the start bindings, the fetch code (same address), or a sharp made invalid.
    void Mutate(Rng& rng) {
        switch (Pick(rng, 9)) {
        case 0: // one bit of the flattened buffer
            if (const auto words = info.flattened_ud_buf.size() - FlatBase; words) {
                info.flattened_ud_buf[FlatBase + Pick(rng, u32(words))] ^= 1u << Pick(rng, 32);
                Sanitize();
            }
            break;
        case 1: { // a resource sharp replaced
            const u32 kind = Pick(rng, 4);
            if (kind == 0 && !info.buffers.empty()) {
                RandomizeBuffer(Pick(rng, u32(info.buffers.size())), rng);
            } else if (kind == 1 && !info.images.empty()) {
                RandomizeImage(Pick(rng, u32(info.images.size())), rng);
            } else if (kind == 2 && !info.fmasks.empty()) {
                RandomizeFMask(Pick(rng, u32(info.fmasks.size())), rng);
            } else if (kind == 3 && !info.samplers.empty()) {
                RandomizeSampler(Pick(rng, u32(info.samplers.size())), rng);
            }
            break;
        }
        case 2: { // a resource made unbound
            const u32 kind = Pick(rng, 4);
            if (kind == 0 && !info.buffers.empty()) {
                info.flattened_ud_buf[BufferOffset(Pick(rng, u32(info.buffers.size()))) + 2] = 0;
            } else if (kind == 1 && !info.images.empty()) {
                const u32 at = ImageOffset(Pick(rng, u32(info.images.size())));
                info.flattened_ud_buf[at] = 0;
                info.flattened_ud_buf[at + 1] &= ~0xFFu;
            } else if (kind == 2 && !info.fmasks.empty()) {
                const u32 at = FMaskOffset(Pick(rng, u32(info.fmasks.size())));
                info.flattened_ud_buf[at] = 0;
                info.flattened_ud_buf[at + 1] &= ~0xFFu;
            } else if (kind == 3 && !info.samplers.empty()) {
                const u32 at = SamplerOffset(Pick(rng, u32(info.samplers.size())));
                std::fill_n(info.flattened_ud_buf.begin() + at, 4, 0u);
            }
            break;
        }
        case 3: // a vertex buffer V# behind the same pointer
            vsharps[Pick(rng, MaxAttributes)] = Chance(rng, 50) ? RandomBuffer(rng) : [&] {
                auto sharp = vsharps[0];
                sharp.num_format = kBufferNfmts[Pick(rng, 7)];
                sharp.data_format = Pick(rng, 16);
                return sharp;
            }();
            break;
        case 4: // a runtime info field
            if (Chance(rng, 50)) {
                rt.sw.vs.step_rate_0 ^= 1;
            } else {
                rt.props.num_user_data ^= 1;
            }
            break;
        case 5: // the start bindings
            (Chance(rng, 50) ? start.buffer : start.unified) += 1;
            break;
        case 6: { // one field of a load in the fetch code
            auto& load = loads[Pick(rng, u32(loads.size()))];
            switch (Pick(rng, 4)) {
            case 0:
                load.dfmt = Pick(rng, 16);
                break;
            case 1:
                load.nfmt = kBufferNfmts[Pick(rng, 7)];
                break;
            case 2:
                load.offset = Pick(rng, 256);
                break;
            default:
                load.vaddr = Pick(rng, 4);
                break;
            }
            WriteCode();
            break;
        }
        case 7: // fetch code with another attribute count
            if (loads.size() > 1 && Chance(rng, 50)) {
                loads.pop_back();
            } else if (loads.size() < MaxAttributes) {
                loads.push_back(RandomLoad(rng));
            }
            WriteCode();
            break;
        default: // the program base (OffsetByProgramBase buffers)
            info.pgm_base += 0x100;
            break;
        }
    }
};

const Shader::Profile kProfile{};

StageSpecialization Build(const Stage& stage) {
    return StageSpecialization(stage.info, stage.rt, kProfile, stage.start);
}

/// The probe's answer, checked against the full build in both call orders (the parse cache is
/// then warm or cold for the probe).
bool CheckAgrees(const StageSpecialization& stored, const Stage& stage, bool probe_first) {
    bool probe, full;
    if (probe_first) {
        probe = Vulkan::SpecProbe::Matches(stored, stage.info, stage.rt, stage.start);
        full = stored == Build(stage);
    } else {
        full = stored == Build(stage);
        probe = Vulkan::SpecProbe::Matches(stored, stage.info, stage.rt, stage.start);
    }
    if (probe != full) {
        std::fprintf(stderr, "probe %d, full build %d\n", probe, full);
        std::abort();
    }
    return full;
}

void Fuzz() {
    Rng rng{0x5bec'1a1e};
    constexpr u32 Iterations = 100000;
    u32 hits = 0, shortcut_hits = 0, mutated_hits = 0;
    for (u32 n = 0; n < Iterations; ++n) {
        static constexpr Shader::SwStage stages[] = {
            Shader::SwStage::Vertex, Shader::SwStage::Vertex, Shader::SwStage::Fragment,
            Shader::SwStage::Compute};
        auto stage = std::make_unique<Stage>(stages[Pick(rng, 4)]);
        stage->Randomize(rng);
        const auto stored = Build(*stage);
        assert(CheckAgrees(stored, *stage, Chance(rng, 50))); // unchanged inputs always match
        const u32 mutations = Pick(rng, 4);
        for (u32 m = 0; m < mutations; ++m) {
            stage->Mutate(rng);
        }
        if (CheckAgrees(stored, *stage, Chance(rng, 50))) {
            ++hits;
            mutated_hits += mutations != 0;
            shortcut_hits += stored.bitset.none() && stored.start != stage->start;
        }
        // A hole left by InsertPermut's resize never matches.
        const StageSpecialization hole{};
        assert(!Vulkan::SpecProbe::Matches(hole, stage->info, stage->rt, stage->start));
    }
    std::printf("fuzz: %u iterations, %u matched (%u after a change, %u through the "
                "no-bindings shortcut with other start bindings)\n",
                Iterations, hits, mutated_hits, shortcut_hits);
    // Both answers must be common, or the comparison above proves little.
    assert(hits > Iterations / 4 && hits < Iterations * 3 / 4);
    assert(mutated_hits > Iterations / 20 && shortcut_hits > 100);
}

/// A vertex shader with one attribute and one buffer, all sharps valid.
std::unique_ptr<Stage> DirectedStage() {
    Rng rng{7};
    auto stage = std::make_unique<Stage>(Shader::SwStage::Vertex);
    stage->info.has_fetch_shader = true;
    stage->Layout(1, 1, 0, 0, rng);
    stage->info.buffers[0].post_op = Shader::SharpFetchPostOp::None;
    stage->info.buffers[0].sharp_fetch.offsets[0] = SharpLocation(Stage::FlatBase);
    auto buffer = RandomBuffer(rng, false);
    buffer.stride = 16;
    stage->SetFlat(stage->BufferOffset(0), &buffer, sizeof(buffer));
    auto image = RandomImage(rng, false);
    stage->info.images[0].is_depth = false;
    stage->SetFlat(stage->ImageOffset(0), &image, sizeof(image));
    stage->loads = {{0, 0, 0, 0}};
    stage->WriteCode();
    auto vsharp = RandomBuffer(rng, false);
    vsharp.num_format = kFloat;
    vsharp.data_format = kFormat32x4;
    stage->vsharps[0] = vsharp;
    return stage;
}

void Asymmetry() {
    const auto unbind_buffer = [](Stage& stage) {
        stage.info.flattened_ud_buf[stage.BufferOffset(0) + 2] = 0; // num_records
    };
    // Stored has the buffer bound, the new draw does not: operator== compares only where the
    // new draw's bit is set, so this matches...
    {
        auto stage = DirectedStage();
        const auto stored = Build(*stage);
        assert(stored.bitset[0]);
        unbind_buffer(*stage);
        assert(CheckAgrees(stored, *stage, true));
        // ...unless the start bindings differ: the stored bits still rule out the shortcut.
        stage->start.buffer += 1;
        assert(!CheckAgrees(stored, *stage, true));
    }
    // Stored has the buffer unbound, the new draw binds it: the new bit is set and the stored
    // default element differs from the new one, so this must not match. A probe gated by the
    // stored bits would wrongly say it does.
    {
        auto stage = DirectedStage();
        const auto bound = Build(*stage);
        unbind_buffer(*stage);
        const auto stored = Build(*stage);
        assert(!stored.bitset[0] && stored.bitset[1]);
        stage->info.flattened_ud_buf[stage->BufferOffset(0) + 2] = 64; // bound again
        assert(!CheckAgrees(stored, *stage, true));
        assert(CheckAgrees(bound, *stage, false)); // as before it was unbound
    }
    // Both bit sets empty: a match whatever the start bindings (the shortcut).
    {
        auto stage = DirectedStage();
        unbind_buffer(*stage);
        stage->info.flattened_ud_buf[stage->ImageOffset(0)] = 0;
        stage->info.flattened_ud_buf[stage->ImageOffset(0) + 1] &= ~0xFFu;
        const auto stored = Build(*stage);
        assert(stored.bitset.none() && !stored.vs_attribs.empty());
        stage->start = {9, 9, 9};
        assert(CheckAgrees(stored, *stage, true));
    }
    // An FMask sets a bit too: bound in the new draw (with an element equal to the stored
    // default), it rules out the shortcut, so other start bindings do not match.
    {
        Rng rng{11};
        auto stage = std::make_unique<Stage>(Shader::SwStage::Fragment);
        stage->Layout(0, 0, 1, 0, rng);
        auto fmask = RandomImage(rng, false);
        fmask.width = 0;
        fmask.height = 0;
        auto unbound = fmask;
        unbound.base_address = 0;
        stage->SetFlat(stage->FMaskOffset(0), &unbound, sizeof(unbound));
        const auto stored = Build(*stage);
        assert(stored.bitset.none() && stored.fmasks.size() == 1);
        stage->SetFlat(stage->FMaskOffset(0), &fmask, sizeof(fmask));
        assert(CheckAgrees(stored, *stage, true));
        stage->start.unified += 1;
        assert(!CheckAgrees(stored, *stage, false));
    }
    std::printf("asymmetry: ok\n");
}

void FetchFormatOverrides() {
    // A typed load whose number format changes the attribute's number class: the fetch data
    // still compares equal (VertexAttribute::operator== ignores the formats), but the fresh
    // attribute's V# does not, so this must not match.
    {
        auto stage = DirectedStage();
        const auto stored = Build(*stage);
        stage->loads[0].dfmt = kFormat32x4;
        stage->loads[0].nfmt = kUint;
        stage->WriteCode();
        const auto fresh = Shader::Gcn::ParseFetchShader(stage->info);
        assert(fresh && fresh->attributes[0].num_format == kUint);
        assert(*stored.fetch_shader_data == *fresh);
        // The stored attribute would still give the stored element: a probe that walked it
        // instead of the fresh parse would wrongly match.
        const auto& old_attribute = stored.fetch_shader_data->attributes[0];
        assert(Vulkan::SpecProbe::MakeVsAttrib(old_attribute, old_attribute.GetSharp(stage->info),
                                               stage->rt) == stored.vs_attribs[0]);
        assert(!CheckAgrees(stored, *stage, true));
    }
    // The data format alone (num_format unchanged): a different remapped number format.
    {
        auto stage = DirectedStage();
        stage->vsharps[0].num_format = kFloat;
        const auto stored = Build(*stage);
        stage->loads[0].dfmt = 1; // Format8: Float is remapped to Uint
        stage->loads[0].nfmt = kFloat;
        stage->WriteCode();
        assert(stored.fetch_shader_data == Shader::Gcn::ParseFetchShader(stage->info));
        assert(!CheckAgrees(stored, *stage, false));
    }
    // inst_offset only moves the V# base address, which no specialization field reads: probe
    // and full build agree that this matches (the permutation identity ignores it today).
    {
        auto stage = DirectedStage();
        const auto stored = Build(*stage);
        stage->loads[0].offset = 48;
        stage->WriteCode();
        assert(Shader::Gcn::ParseFetchShader(stage->info)->attributes[0].inst_offset == 48);
        assert(CheckAgrees(stored, *stage, true));
    }
    std::printf("fetch format overrides: ok\n");
}

} // namespace

int main() {
    Asymmetry();
    FetchFormatOverrides();
    Fuzz();
    std::printf("spec-match-test: passed\n");
}
