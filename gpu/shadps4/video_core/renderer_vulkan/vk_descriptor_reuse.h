// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: which descriptors of a push the command stream already holds. A Shadow keeps the
// descriptors the last tracked pushes established for one bind point, by content (handles,
// offsets, ranges, image layouts), never by pointer. Plain types only, so it is testable without a
// device (tests/test_descriptor_reuse.cpp).
//
// GOW3_DESCRIPTOR_REUSE_STATS=1 (with GOW3_FRAME_STATS=1) counts, per bind of
// Pipeline::BindResources, how many descriptors a push could leave out under three rules:
//   exact:      same VkPipelineLayout handle as the previous push (the conservative rule);
//   structural: an identically defined set layout (Vulkan-compatible, other handle);
//   any:        any layout (an upper bound: incompatible layouts disturb the set).
// All three also need the stream unchanged since that push (the bind point's epoch).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace DescriptorReuse {

enum class Kind : std::uint8_t { Buffer, Image, Sampler, Other };

/// One descriptor's defining fields. Buffer: buffer, offset, range. Image: view, layout.
/// Sampler: sampler. Fields a kind does not use stay 0.
struct Element {
    std::uint64_t handle = 0;
    std::uint64_t offset = 0;
    std::uint64_t range = 0;
    bool operator==(const Element&) const = default;
};

/// One VkWriteDescriptorSet reduced to what defines its descriptors.
struct Write {
    std::uint32_t binding = 0;
    std::uint32_t first = 0; ///< dstArrayElement
    std::uint32_t count = 0;
    std::uint32_t type = 0;  ///< VkDescriptorType
    Kind kind = Kind::Other;
    /// False for anything this file does not model (pNext, texel buffers, unknown types): such a
    /// write never counts as unchanged and makes the shadow forget everything.
    bool supported = true;
    const Element* elements = nullptr; ///< `count` entries
};

struct Compared {
    std::uint32_t unchanged = 0;   ///< elements equal to the shadow
    std::uint32_t offset_only = 0; ///< buffers whose only difference is the offset
    [[nodiscard]] bool AllUnchanged(const Write& w) const {
        return w.supported && w.count != 0 && unchanged == w.count;
    }
};

/// What one bind point's last tracked pushes established. Owned copies only.
class Shadow {
public:
    /// Compares by content; the caller decides separately whether the stream still holds it
    /// (Valid) and whether the layout allows reuse.
    [[nodiscard]] Compared Compare(const Write& w) const {
        Compared out{};
        if (!w.supported || w.binding >= slots.size()) {
            return out;
        }
        const Slot& slot = slots[w.binding];
        if (slot.type != w.type) {
            return out;
        }
        for (std::uint32_t i = 0; i < w.count; ++i) {
            const std::size_t index = std::size_t(w.first) + i;
            if (index >= slot.known.size() || !slot.known[index]) {
                continue;
            }
            const Element& old = slot.elements[index];
            const Element& now = w.elements[i];
            if (old == now) {
                ++out.unchanged;
            } else if (w.kind == Kind::Buffer && old.handle == now.handle &&
                       old.range == now.range) {
                ++out.offset_only;
            }
        }
        return out;
    }

    /// Records the descriptors `w` establishes.
    void Commit(const Write& w) {
        if (!w.supported) {
            Forget();
            return;
        }
        if (w.binding >= slots.size()) {
            slots.resize(std::size_t(w.binding) + 1);
        }
        Slot& slot = slots[w.binding];
        if (slot.type != w.type) {
            slot.type = w.type;
            slot.elements.clear();
            slot.known.clear();
        }
        const std::size_t end = std::size_t(w.first) + w.count;
        if (slot.elements.size() < end) {
            slot.elements.resize(end);
            slot.known.resize(end, false);
        }
        for (std::uint32_t i = 0; i < w.count; ++i) {
            slot.elements[w.first + i] = w.elements[i];
            slot.known[w.first + i] = true;
        }
    }

    /// After the writes of one push: the stream holds this state as of `epoch_`.
    void Seal(std::uint32_t epoch_) {
        epoch = epoch_;
        sealed = true;
    }

    [[nodiscard]] bool Valid(std::uint32_t current_epoch) const {
        return sealed && epoch == current_epoch;
    }

    void Forget() {
        for (Slot& slot : slots) {
            slot.type = ~0u;
            slot.elements.clear();
            slot.known.clear();
        }
        sealed = false;
    }

private:
    struct Slot {
        std::uint32_t type = ~0u;
        std::vector<Element> elements;
        std::vector<bool> known;
    };
    std::vector<Slot> slots;
    std::uint32_t epoch = 0;
    bool sealed = false;
};

/// Why a bind point's stream state stopped matching its shadow.
enum class Cause : std::uint8_t {
    RawCommandBuffer, ///< Scheduler::CommandBuffer(): untracked commands may follow
    NewCommandBuffer, ///< a command buffer began
    SetFallback,      ///< BindResources bound a descriptor set (layout too large to push)
    ScenePass,        ///< the scene resample pass pushed graphics descriptors in Record
    ComputePass,      ///< a host compute pass pushed compute descriptors in Record
    Count,
};
inline constexpr std::size_t CauseCount = std::size_t(Cause::Count);
inline constexpr const char* CauseNames[CauseCount] = {"raw", "newcb", "setfb", "scene",
                                                         "cpass"};

// ---- GOW3_DESCRIPTOR_REUSE_STATS counters (cumulative; the flip writes them per frame) ----

enum Counter : std::uint32_t {
    Binds,          ///< BindResources calls with descriptors
    PushBinds,      ///< ... that push
    SetBinds,       ///< ... that update and bind a set (not modelled)
    Writes,         ///< writes pushed
    Elements,       ///< descriptors pushed (array elements counted)
    Bytes,          ///< bytes a deferred push copies: writes plus their info arrays
    SameLayout,     ///< same layout handle as the previous push (= same pipeline)
    SameSignature,  ///< identically defined layout as the previous push
    ValidBinds,     ///< the stream still held the previous push's state
    SetEqual,       ///< same layout and every write equal to the previous push (old 0 % metric)
    ElemUnchanged,  ///< descriptors equal to the shadow, ignoring layout and epoch
    BufElems,
    BufUnchanged,
    BufOffsetOnly,  ///< buffers that differ only in offset
    ImgElems,
    ImgUnchanged,
    SampElems,
    SampUnchanged,
    OtherElems,     ///< unsupported writes' descriptors
    UnsupportedWrites,
    // Avoidable under each rule: element level (array elements individually), write level (a
    // write left out whole), bytes (of the left-out writes), binds with nothing left to push.
    ExactElems,
    ExactWrites,
    ExactBytes,
    ExactBinds,
    StructElems,
    StructWrites,
    StructBytes,
    StructBinds,
    AnyElems,
    AnyWrites,
    AnyBytes,
    AnyBinds,
    // Descriptors the exact rule would have left out had the epoch not changed, by last cause.
    LostRaw,
    LostNewCb,
    LostSetFb,
    LostScene,
    LostCPass,
    // Invalidations by cause, of the scheduler that pushes tracked descriptors.
    InvRaw,
    InvNewCb,
    InvSetFb,
    InvScene,
    InvCPass,
    CounterCount,
};
static_assert(LostCPass - LostRaw + 1 == CauseCount && InvCPass - InvRaw + 1 == CauseCount);

inline constexpr const char* CounterNames[CounterCount] = {
    "binds",       "push_binds",    "set_binds",     "writes",       "elems",
    "bytes",       "same_layout",   "same_sig",      "valid",        "set_equal",
    "elem_same",   "buf",           "buf_same",      "buf_offset",   "img",
    "img_same",    "samp",          "samp_same",     "other",        "unsupported",
    "exact_elems", "exact_writes",  "exact_bytes",   "exact_binds",  "struct_elems",
    "struct_writes", "struct_bytes", "struct_binds", "any_elems",    "any_writes",
    "any_bytes",   "any_binds",     "lost_raw",      "lost_newcb",   "lost_setfb",
    "lost_scene",  "lost_cpass",    "inv_raw",       "inv_newcb",    "inv_setfb",
    "inv_scene",   "inv_cpass",
};

struct Counters {
    std::array<std::array<std::atomic<std::uint64_t>, CounterCount>, 2> value{};
    void Add(std::uint32_t point, Counter c, std::uint64_t n = 1) {
        value[point][c].fetch_add(n, std::memory_order_relaxed);
    }
};

/// GOW3_DESCRIPTOR_REUSE_STATS=1: count (vk_pipeline_common.cpp) and log them per flip
/// (videoout driver, next to GOW3_FRAME_LOG).
inline const bool stats_enabled = [] {
    const char* env = std::getenv("GOW3_DESCRIPTOR_REUSE_STATS");
    return env && env[0] == '1';
}();
inline Counters stats;

/// The per-flip log: a header naming the columns, then one line per flip of cumulative counters.
inline void WriteHeader(std::FILE* file) {
    std::fputs("# ms_of_day", file);
    for (const char* point : {"g_", "c_"}) {
        for (const char* name : CounterNames) {
            std::fprintf(file, " %s%s", point, name);
        }
    }
    std::fputc('\n', file);
}
inline void WriteLine(std::FILE* file, std::uint32_t ms_of_day) {
    std::fprintf(file, "%u", ms_of_day);
    for (const auto& point : stats.value) {
        for (const auto& v : point) {
            std::fprintf(file, " %llu",
                         static_cast<unsigned long long>(v.load(std::memory_order_relaxed)));
        }
    }
    std::fputc('\n', file);
}

/// Per bind point, a stream epoch: bumped by anything that may disturb tracked descriptors.
/// Relaxed atomics: bumps come from stage A, stage B, the presenter and helper threads, while
/// the order that matters is the producer's own (commands enter the stream in call order).
struct Epochs {
    std::array<std::atomic<std::uint32_t>, 2> epoch{};
    std::array<std::atomic<std::uint8_t>, 2> last_cause{};
    std::array<std::array<std::atomic<std::uint64_t>, CauseCount>, 2> invalidations{};
    /// Set by the first tracked push: only this scheduler's invalidations go into `stats`.
    std::atomic<bool> tracked{false};

    void Invalidate(Cause cause, bool graphics, bool compute) {
        for (std::uint32_t point = 0; point < 2; ++point) {
            if (point == 0 ? !graphics : !compute) {
                continue;
            }
            epoch[point].fetch_add(1, std::memory_order_relaxed);
            last_cause[point].store(std::uint8_t(cause), std::memory_order_relaxed);
            invalidations[point][std::size_t(cause)].fetch_add(1, std::memory_order_relaxed);
            if (stats_enabled && tracked.load(std::memory_order_relaxed)) {
                stats.Add(point, Counter(InvRaw + std::uint32_t(cause)));
            }
        }
    }
    [[nodiscard]] std::uint32_t Current(std::uint32_t point) const {
        return epoch[point].load(std::memory_order_relaxed);
    }
};


/// Per bind point, what the previous tracked push used.
struct LastPush {
    std::uint64_t layout = 0;
    std::uint64_t signature = 0;
    std::uint32_t write_count = 0;
    bool any = false;
};

/// Counts one push of `writes` against `shadow`, then commits it. `write_bytes[i]` is what
/// write i costs to copy. Single-threaded per bind point (the producing thread).
inline void Account(Counters& counters, std::uint32_t point, Shadow& shadow, LastPush& last,
                    Epochs& epochs, const Write* writes, std::size_t count,
                    const std::uint32_t* write_bytes, std::uint64_t layout,
                    std::uint64_t signature) {
    epochs.tracked.store(true, std::memory_order_relaxed);
    const std::uint32_t epoch = epochs.Current(point);
    const bool valid = shadow.Valid(epoch);
    const bool same_layout = last.any && last.layout == layout;
    const bool same_signature = last.any && last.signature == signature;
    const bool exact = valid && same_layout;
    const bool structural = valid && same_signature;
    counters.Add(point, PushBinds);
    counters.Add(point, Writes, count);
    if (same_layout) {
        counters.Add(point, SameLayout);
    }
    if (same_signature) {
        counters.Add(point, SameSignature);
    }
    if (valid) {
        counters.Add(point, ValidBinds);
    }
    bool all_exact = true, all_struct = true, all_any = true, all_equal = true;
    std::uint64_t lost = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Write& w = writes[i];
        const Compared c = shadow.Compare(w);
        const bool whole = c.AllUnchanged(w);
        all_equal &= whole;
        counters.Add(point, Elements, w.count);
        counters.Add(point, Bytes, write_bytes[i]);
        counters.Add(point, ElemUnchanged, c.unchanged);
        if (!w.supported) {
            counters.Add(point, UnsupportedWrites);
            counters.Add(point, OtherElems, w.count);
        } else if (w.kind == Kind::Buffer) {
            counters.Add(point, BufElems, w.count);
            counters.Add(point, BufUnchanged, c.unchanged);
            counters.Add(point, BufOffsetOnly, c.offset_only);
        } else if (w.kind == Kind::Image) {
            counters.Add(point, ImgElems, w.count);
            counters.Add(point, ImgUnchanged, c.unchanged);
        } else if (w.kind == Kind::Sampler) {
            counters.Add(point, SampElems, w.count);
            counters.Add(point, SampUnchanged, c.unchanged);
        } else {
            counters.Add(point, OtherElems, w.count);
        }
        const auto avoid = [&](bool rule, Counter elems, Counter writes_c, Counter bytes,
                               bool& all) {
            if (rule) {
                counters.Add(point, elems, c.unchanged);
            }
            if (rule && whole) {
                counters.Add(point, writes_c);
                counters.Add(point, bytes, write_bytes[i]);
            } else {
                all = false;
            }
        };
        avoid(exact, ExactElems, ExactWrites, ExactBytes, all_exact);
        avoid(structural, StructElems, StructWrites, StructBytes, all_struct);
        avoid(valid, AnyElems, AnyWrites, AnyBytes, all_any);
        if (!valid && same_layout) {
            lost += c.unchanged;
        }
    }
    if (count != 0) {
        if (all_exact) {
            counters.Add(point, ExactBinds);
        }
        if (all_struct) {
            counters.Add(point, StructBinds);
        }
        if (all_any) {
            counters.Add(point, AnyBinds);
        }
    }
    if (same_layout && all_equal && count == last.write_count) {
        counters.Add(point, SetEqual);
    }
    if (lost != 0) {
        const auto cause = epochs.last_cause[point].load(std::memory_order_relaxed);
        counters.Add(point, Counter(LostRaw + cause), lost);
    }
    for (std::size_t i = 0; i < count; ++i) {
        shadow.Commit(writes[i]);
    }
    shadow.Seal(epoch);
    last = {layout, signature, std::uint32_t(count), true};
}

} // namespace DescriptorReuse
