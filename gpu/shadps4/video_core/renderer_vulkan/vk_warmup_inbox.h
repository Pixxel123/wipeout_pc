// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: helpers of the background pipeline cache warm-up (vk_pipeline_serialization.cpp).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace Vulkan {

inline bool UseOnDemandPipelines(bool binary_cache, bool supported, bool capture_only,
                                 bool background_warmup) {
    return background_warmup && binary_cache && supported && !capture_only;
}

/// gow3 (GOW3_PRECOMPILE_BOOT): whether the start-up rebuild from a precompile list runs before
/// the warm-up when on-demand pipelines are off (pipeline binaries off, as on NVIDIA).
constexpr bool RunsPrecompileBoot(bool boot_requested, bool list_present, bool capture_only) {
    return boot_requested && list_present && !capture_only;
}

/// gow3 (GOW3_PRECOMPILE_BOOT): whether the start-up rebuild runs once the list's code is read.
/// Not when the list needed the code of `wanted` programs from the game files and fewer than 9
/// in 10 were `read` (no game folder, an archive not found or not read): the start then goes on
/// as without a list. A few programs not in the game files (1 of 1,283 for the shipped list)
/// do not stop it.
constexpr bool GameCodeSuffices(std::uint32_t wanted, std::uint32_t read) {
    return std::uint64_t{read} * 10 >= std::uint64_t{wanted} * 9;
}

/// gow3: what a start does with the saved shader store (BeginDiskShaders).
enum class DiskShaderStart {
    IndexOnly,         ///< the store is read as it is
    Rebuild,           ///< the saved shaders are dropped, the list rebuilt and its pipelines built
    RenewWithoutBuild, ///< the saved shaders are dropped and the marker saved, no list to rebuild
};

/// gow3 (GOW3_PRECOMPILE_BOOT): a store marker that differs from this build's rebuilds with a
/// ready list (PreparePrecompileList) and is renewed without one. With an equal marker, a
/// driver pipeline-binary key recorded by the last start-up rebuild that differs from the
/// driver's (a driver update: no saved pipeline binary is usable) rebuilds when the list is
/// ready; without a ready list, or without a recorded key (a store from before the key), the
/// store is indexed as it is.
constexpr DiskShaderStart DiskShaderStartOf(bool marker_equal, bool key_recorded,
                                            bool key_equal, bool list_ready) {
    if (!marker_equal) {
        return list_ready ? DiskShaderStart::Rebuild : DiskShaderStart::RenewWithoutBuild;
    }
    return key_recorded && !key_equal && list_ready ? DiskShaderStart::Rebuild
                                                    : DiskShaderStart::IndexOnly;
}

/// gow3 (GOW3_PRECOMPILE_BOOT): whether a rebuild start skips the pipeline pass, as
/// GOW3_PRECOMPILE_BOOT_PIPELINES=0 does. `unfinished`: the passes in a row that started and
/// did not end (ShaderProfile/pass_attempts, 0 when absent). One unfinished pass (a quit) builds
/// again; at two, a pass that crashes the game would crash every start, so the start skips it.
constexpr bool SkipsPipelinePass(std::uint32_t unfinished) {
    return unfinished >= 2;
}

/// gow3: whether the background warm-up leaves a saved stage alone because the game is
/// translating a permutation of its program (`pending`). With first-use numbering that
/// translation takes the next index, which may be the stage's. With the store's numbering it
/// takes an index the store does not hold, so a stage whose spec is already at its saved index
/// is used as it is; nothing is inserted while the translation is pending.
constexpr bool WarmStageWaitsForTranslation(bool pending, bool store_numbering,
                                            bool at_saved_index) {
    return pending && !(store_numbering && at_saved_index);
}

/// #149: set when the driver refuses a cached pipeline for lack of memory while preloading.
/// The bulk warm-up then stops reading and building: each further build would only fight the
/// game for what is left, and the rest compile when the game needs them. Results are VkResult
/// values (this header does not include Vulkan).
class WarmupMemoryStop {
public:
    static constexpr int OutOfHostMemory = -1;   ///< VK_ERROR_OUT_OF_HOST_MEMORY
    static constexpr int OutOfDeviceMemory = -2; ///< VK_ERROR_OUT_OF_DEVICE_MEMORY

    void Report(int result) {
        if (result == OutOfHostMemory || result == OutOfDeviceMemory) {
            stopped.store(true, std::memory_order_relaxed);
        }
    }
    bool Stopped() const {
        return stopped.load(std::memory_order_relaxed);
    }
    void Reset() {
        stopped.store(false, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> stopped{false};
};

/// The one shared by the pipeline constructors (preloading) and the warm-ups.
inline WarmupMemoryStop& PreloadMemoryStop() {
    static WarmupMemoryStop stop;
    return stop;
}

/// Warm-up threads: one on four cores or fewer, otherwise all but two, at most six.
inline unsigned WarmupThreadCount(unsigned cores) {
    return cores <= 4 ? 1u : std::min(cores - 2, 6u);
}

/// Items made on worker threads, handed to the GPU thread a few at a time so that a frame
/// never pays for the whole cache at once.
template <typename T>
class WarmupInbox {
public:
    void Push(T item) {
        std::scoped_lock lock{mutex};
        items.push_back(std::move(item));
    }

    /// At most `max` items, oldest first.
    std::vector<T> Take(std::size_t max) {
        std::scoped_lock lock{mutex};
        const std::size_t count = std::min(max, items.size());
        std::vector<T> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(std::move(items.front()));
            items.pop_front();
        }
        return out;
    }

    bool Empty() {
        std::scoped_lock lock{mutex};
        return items.empty();
    }

private:
    std::mutex mutex;
    std::deque<T> items;
};

/// The first pipeline ready for a key is kept: a warm-up pipeline that arrives after the
/// game compiled the same one (or the reverse) is dropped. True when `value` was stored.
template <typename Map, typename Key, typename Value>
bool PublishFirst(Map& map, const Key& key, Value&& value) {
    return map.try_emplace(key, std::forward<Value>(value)).second;
}

} // namespace Vulkan
