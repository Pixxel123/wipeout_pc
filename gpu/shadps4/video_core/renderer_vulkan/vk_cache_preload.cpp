// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_cache_preload.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Vulkan::CachePreload {
namespace {
struct State {
    std::filesystem::path root;
    std::mutex mutex;
    Store store;
};
// gow3: created once and never destroyed: the thread runs until the process ends, and nothing
// at exit has to wait for it or tear down what it uses.
std::atomic<State*> g_state{nullptr};

/// The started state and `file` as a list entry, or nothing for a file outside the cache.
std::pair<State*, std::string> Active(const std::filesystem::path& file) {
    State* state = g_state.load(std::memory_order_acquire);
    if (!state) {
        return {};
    }
    auto entry = file.lexically_relative(state->root).generic_string();
    if (!ValidEntry(entry)) {
        return {};
    }
    return {state, std::move(entry)};
}

std::optional<std::vector<u8>> ReadBytes(const std::filesystem::path& file, size_t limit) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    const auto size = stream ? std::streamoff(stream.tellg()) : -1;
    if (size <= 0 || size > std::streamoff(limit)) {
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return std::nullopt;
    }
    return bytes;
}

/// Reads the listed files into the store when `load` is set, rewrites the list without the
/// oldest and missing entries and prints what it did.
void Preload(State& state, bool read_list, bool load) {
    const auto started = std::chrono::steady_clock::now();
    const auto list = state.root / ListName;
    std::string text;
    if (auto bytes = read_list ? ReadBytes(list, MaxListBytes) : std::nullopt) {
        text.assign(bytes->begin(), bytes->end());
    }
    // Newest first, so the byte cap leaves out the oldest files.
    const auto entries = Recent(text, MaxFiles);
    std::vector<std::string> kept;
    size_t loaded = 0, loaded_bytes = 0;
    for (const auto& entry : entries) {
        const auto file = state.root / entry;
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error) {
            continue; // gone: dropped from the list
        }
        kept.push_back(entry);
        if (!load || loaded_bytes + size > MaxBytes) {
            continue;
        }
        auto bytes = ReadBytes(file, MaxBytes - loaded_bytes);
        if (!bytes) {
            continue;
        }
        const size_t read = bytes->size();
        std::scoped_lock lock{state.mutex};
        if (state.store.Keep(entry, std::move(*bytes))) {
            ++loaded;
            loaded_bytes += read;
        }
    }
    // Rewritten oldest first, so the list stays at most MaxFiles entries plus one session's.
    std::string trimmed;
    for (auto entry = kept.rbegin(); entry != kept.rend(); ++entry) {
        trimmed += *entry + '\n';
    }
    std::error_code error;
    if (trimmed.empty()) {
        std::filesystem::remove(list, error);
    } else {
        PipelineBinaryCache::Save(list, std::span(reinterpret_cast<const u8*>(trimmed.data()),
                                                  trimmed.size()));
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - started).count();
    if (load) {
        std::printf("Cache preload: %zu of %zu recent files (%.1f MB) in %.0f ms\n", loaded,
                    kept.size(), loaded_bytes / 1048576.0, ms);
    } else {
        std::printf("Cache preload: off (GOW3_CACHE_PRELOAD=0), %zu recent files listed\n",
                    kept.size());
    }
}

/// gow3: the files the game uses are appended here every few seconds rather than by the thread
/// that builds the pipeline, which must not wait for the disk. The writes land in the OS file
/// cache; the few files of the last seconds before a forced exit are only missing from the next
/// preload.
[[noreturn]] void AppendLoop(State& state) {
    const auto list = state.root / ListName;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::vector<std::string> used;
        {
            std::scoped_lock lock{state.mutex};
            used = state.store.TakePending();
        }
        if (used.empty()) {
            continue;
        }
        std::ofstream out(list, std::ios::binary | std::ios::app);
        for (const auto& entry : used) {
            out << entry << '\n';
        }
    }
}
} // namespace

void Start(const std::filesystem::path& root, bool read_list) {
    if (g_state.load(std::memory_order_acquire)) {
        return;
    }
    auto* state = new State;
    state->root = root;
    const bool load = EmulatorSettingsImpl::Flag("GOW3_CACHE_PRELOAD", true);
    g_state.store(state, std::memory_order_release);
    std::thread([state, read_list, load] {
        Common::SetCurrentThreadName("GoW3:CachePreload");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        Preload(*state, read_list, load);
        AppendLoop(*state);
    }).detach();
}

std::optional<std::vector<u8>> Find(const std::filesystem::path& file) {
    const auto [state, entry] = Active(file);
    if (!state) {
        return std::nullopt;
    }
    std::scoped_lock lock{state->mutex};
    const auto* bytes = state->store.Find(entry);
    return bytes ? std::optional{*bytes} : std::nullopt;
}

void Used(const std::filesystem::path& file) {
    if (const auto [state, entry] = Active(file); state) {
        std::scoped_lock lock{state->mutex};
        state->store.Used(entry);
    }
}

void Saved(const std::filesystem::path& file) {
    if (const auto [state, entry] = Active(file); state) {
        std::scoped_lock lock{state->mutex};
        state->store.Saved(entry);
    }
}

} // namespace Vulkan::CachePreload
