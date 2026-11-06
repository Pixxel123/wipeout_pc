// SPDX-License-Identifier: GPL-2.0-or-later
// Descriptor reuse shadow and GOW3_DESCRIPTOR_REUSE_STATS accounting (vk_descriptor_reuse.h).
// No device needed.
#include <cassert>
#include <cstdio>
#include <vector>

#include "video_core/renderer_vulkan/vk_descriptor_reuse.h"

using namespace DescriptorReuse;

namespace {
constexpr std::uint32_t StorageBuffer = 7, SampledImage = 2, SamplerType = 0, StorageImage = 3;
constexpr std::uint32_t Graphics = 0, Compute = 1;

/// A push: writes plus owned element storage (rebuilt per push so addresses differ).
struct Push {
    std::vector<std::vector<Element>> storage;
    std::vector<Write> writes;
    std::vector<std::uint32_t> bytes;
    Push& Add(std::uint32_t binding, std::uint32_t type, Kind kind, std::vector<Element> els,
              std::uint32_t first = 0, bool supported = true) {
        storage.push_back(std::move(els));
        writes.push_back({binding, first, std::uint32_t(storage.back().size()), type, kind,
                          supported, nullptr});
        bytes.push_back(64 + 24 * std::uint32_t(storage.back().size()));
        for (std::size_t i = 0; i < writes.size(); ++i) {
            writes[i].elements = storage[i].data();
        }
        return *this;
    }
};

Push Typical(std::uint64_t cb_offset, std::uint64_t view = 0x100, std::uint64_t sampler = 0x200,
             std::uint64_t layout = 5, std::uint64_t range = 256) {
    Push p;
    p.Add(0, StorageBuffer, Kind::Buffer, {{0x10, cb_offset, range}});
    p.Add(1, SampledImage, Kind::Image, {{view, layout, 0}});
    p.Add(2, SamplerType, Kind::Sampler, {{sampler, 0, 0}});
    return p;
}

struct Rig {
    Counters counters;
    Epochs epochs;
    std::array<Shadow, 2> shadow;
    std::array<LastPush, 2> last;
    void Run(std::uint32_t point, const Push& p, std::uint64_t layout, std::uint64_t sig) {
        counters.Add(point, Binds);
        Account(counters, point, shadow[point], last[point], epochs, p.writes.data(),
                p.writes.size(), p.bytes.data(), layout, sig);
    }
    std::uint64_t Get(std::uint32_t point, Counter c) const {
        return counters.value[point][c].load();
    }
    void Clear() {
        for (auto& point : counters.value) {
            for (auto& v : point) {
                v = 0;
            }
        }
    }
};
} // namespace

int main() {
    // 1. First use: nothing avoidable.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, Elements) == 3 && r.Get(Graphics, ExactElems) == 0);
        assert(r.Get(Graphics, ExactWrites) == 0 && r.Get(Graphics, ValidBinds) == 0);
    }
    // 2. Identical payloads at other host addresses: everything avoidable, set equal.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, ExactElems) == 3 && r.Get(Graphics, ExactWrites) == 3);
        assert(r.Get(Graphics, ExactBinds) == 1 && r.Get(Graphics, SetEqual) == 1);
        assert(r.Get(Graphics, ExactBytes) == 3 * 88);
    }
    // 3. Buffer offset only: the image and sampler are avoidable, the buffer is offset-only.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.Clear();
        r.Run(Graphics, Typical(256), 1, 11);
        assert(r.Get(Graphics, ExactWrites) == 2 && r.Get(Graphics, ExactBinds) == 0);
        assert(r.Get(Graphics, BufOffsetOnly) == 1 && r.Get(Graphics, BufUnchanged) == 0);
        assert(r.Get(Graphics, ImgUnchanged) == 1 && r.Get(Graphics, SampUnchanged) == 1);
        assert(r.Get(Graphics, SetEqual) == 0);
    }
    // 4. View, sampler, range and image layout changes are changes.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.Clear();
        r.Run(Graphics, Typical(0, 0x101), 1, 11);
        assert(r.Get(Graphics, ImgUnchanged) == 0 && r.Get(Graphics, ExactWrites) == 2);
        r.Clear();
        r.Run(Graphics, Typical(0, 0x101, 0x201), 1, 11);
        assert(r.Get(Graphics, SampUnchanged) == 0);
        r.Clear();
        r.Run(Graphics, Typical(0, 0x101, 0x201, 5, 512), 1, 11);
        assert(r.Get(Graphics, BufUnchanged) == 0 && r.Get(Graphics, BufOffsetOnly) == 0);
        r.Clear();
        r.Run(Graphics, Typical(0, 0x101, 0x201, 1, 512), 1, 11);
        assert(r.Get(Graphics, ImgUnchanged) == 0 && r.Get(Graphics, ExactWrites) == 2);
    }
    // 5. An array with one changed element: element-level reuse, the write is kept whole.
    {
        Rig r;
        Push a;
        a.Add(0, StorageImage, Kind::Image, {{1, 1, 0}, {2, 1, 0}, {3, 1, 0}});
        r.Run(Graphics, a, 1, 11);
        r.Clear();
        Push b;
        b.Add(0, StorageImage, Kind::Image, {{1, 1, 0}, {9, 1, 0}, {3, 1, 0}});
        r.Run(Graphics, b, 1, 11);
        assert(r.Get(Graphics, ExactElems) == 2 && r.Get(Graphics, ExactWrites) == 0);
        assert(r.Get(Graphics, ExactBinds) == 0);
        // A partial write at an offset updates only its elements.
        r.Clear();
        Push c;
        c.Add(0, StorageImage, Kind::Image, {{9, 1, 0}}, 1);
        r.Run(Graphics, c, 1, 11);
        assert(r.Get(Graphics, ExactWrites) == 1);
    }
    // 6. Layout change: not exact, structural if identically defined, any always.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.Clear();
        r.Run(Graphics, Typical(0), 2, 11);
        assert(r.Get(Graphics, ExactElems) == 0 && r.Get(Graphics, StructElems) == 3);
        assert(r.Get(Graphics, AnyElems) == 3 && r.Get(Graphics, SameLayout) == 0);
        r.Clear();
        r.Run(Graphics, Typical(0), 3, 12);
        assert(r.Get(Graphics, StructElems) == 0 && r.Get(Graphics, AnyElems) == 3);
        // The other layout's push established the state: back on layout 3 it is reusable.
        r.Clear();
        r.Run(Graphics, Typical(0), 3, 12);
        assert(r.Get(Graphics, ExactBinds) == 1);
    }
    // 7. A new command buffer forces a full push and is charged as lost reuse.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.epochs.Invalidate(Cause::NewCommandBuffer, true, true);
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, ExactElems) == 0 && r.Get(Graphics, AnyElems) == 0);
        assert(r.Get(Graphics, ElemUnchanged) == 3 && r.Get(Graphics, LostNewCb) == 3);
        assert(r.epochs.invalidations[Graphics][std::size_t(Cause::NewCommandBuffer)] == 1);
        // The full push revalidates.
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, ExactElems) == 3);
    }
    // 8. External binding invalidation (raw command buffer, scene pass) forces a full push.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.epochs.Invalidate(Cause::ScenePass, true, false);
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, ExactElems) == 0 && r.Get(Graphics, LostScene) == 3);
        r.epochs.Invalidate(Cause::RawCommandBuffer, true, true);
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, LostRaw) == 3);
    }
    // 9. Unsupported writes never count as unchanged and make the shadow forget.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        Push p = Typical(0);
        p.Add(3, 99, Kind::Other, {{7, 0, 0}}, 0, false);
        r.Clear();
        r.Run(Graphics, p, 1, 11);
        assert(r.Get(Graphics, UnsupportedWrites) == 1 && r.Get(Graphics, OtherElems) == 1);
        assert(r.Get(Graphics, ExactWrites) == 3 && r.Get(Graphics, ExactBinds) == 0);
        // Forgotten: the next identical push reuses nothing.
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        assert(r.Get(Graphics, ExactElems) == 0);
    }
    // 10. Graphics and compute are independent.
    {
        Rig r;
        r.Run(Graphics, Typical(0), 1, 11);
        r.Run(Compute, Typical(64), 2, 22);
        r.epochs.Invalidate(Cause::ComputePass, false, true);
        r.Clear();
        r.Run(Graphics, Typical(0), 1, 11);
        r.Run(Compute, Typical(64), 2, 22);
        assert(r.Get(Graphics, ExactBinds) == 1 && r.Get(Compute, ExactBinds) == 0);
        assert(r.Get(Compute, LostCPass) == 3);
    }
    // Type change on a binding: not unchanged even with equal handles.
    {
        Shadow s;
        Element e{5, 0, 0};
        s.Commit({0, 0, 1, SampledImage, Kind::Image, true, &e});
        assert(s.Compare({0, 0, 1, StorageImage, Kind::Image, true, &e}).unchanged == 0);
        assert(s.Compare({0, 0, 1, SampledImage, Kind::Image, true, &e}).unchanged == 1);
    }
    // The per-flip log: the header names every column of a line.
    {
        std::FILE* file = std::tmpfile();
        assert(file);
        WriteHeader(file);
        WriteLine(file, 123);
        std::rewind(file);
        char header[8192], line[8192];
        assert(std::fgets(header, sizeof(header), file) && std::fgets(line, sizeof(line), file));
        const auto words = [](const char* text) {
            int n = 0;
            for (bool in = false; *text; ++text) {
                const bool space = *text == ' ' || *text == '\n';
                n += !space && !in;
                in = !space;
            }
            return n;
        };
        assert(words(header) == words(line) + 1); // "#" heads the header
        assert(words(line) == 1 + 2 * int(CounterCount));
        std::fclose(file);
    }
    std::puts("descriptor reuse: ok");
    return 0;
}
