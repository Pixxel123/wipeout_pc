// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_pipeline_binary_preload.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Vulkan::PipelineBinaryPreload {
namespace {
struct State {
    std::filesystem::path directory;
    bool preload = true;
    std::mutex mutex;
    std::unordered_map<std::string, std::vector<u8>> bytes; ///< preloaded files by name
    std::unordered_set<std::string> used;                   ///< names listed this session
    std::vector<std::string> pending;                       ///< not yet appended to the list
};
// gow3: created once and never destroyed: the thread runs until the process ends, and nothing
// at exit has to wait for it or tear down what it uses.
std::atomic<State*> g_state{nullptr};

State* Active(const std::filesystem::path& file) {
    State* state = g_state.load(std::memory_order_acquire);
    return state && file.parent_path() == state->directory ? state : nullptr;
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

void Record(State& state, const std::string& name) {
    if (state.used.insert(name).second) {
        state.pending.push_back(name);
    }
}

void Run(State& state) {
    Common::SetCurrentThreadName("GoW3:BinaryPreload");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    const auto started = std::chrono::steady_clock::now();
    const auto list = state.directory / ListName;
    std::string text;
    // A list grown past 16 MB (no start ever got to trim it) is dropped and begins again.
    if (auto bytes = ReadBytes(list, size_t{16} << 20)) {
        text.assign(bytes->begin(), bytes->end());
    }
    // Newest first, so the byte cap leaves out the oldest files.
    const auto names = Recent(text, MaxFiles);
    std::vector<std::string> kept;
    size_t loaded = 0, loaded_bytes = 0;
    for (const auto& name : names) {
        const auto file = state.directory / (name + ".bin");
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (error) {
            continue; // gone: dropped from the list
        }
        kept.push_back(name);
        if (!state.preload || loaded_bytes + size > MaxBytes) {
            continue;
        }
        auto bytes = ReadBytes(file, MaxBytes - loaded_bytes);
        if (!bytes) {
            continue;
        }
        std::scoped_lock lock{state.mutex};
        // A file the game already asked for was read from the disk then, and may have been
        // written again since this thread read it: its bytes here are not kept.
        if (!state.used.contains(name)) {
            ++loaded;
            loaded_bytes += bytes->size();
            state.bytes.emplace(name, std::move(*bytes));
        }
    }
    // Rewritten oldest first, so the list stays at most MaxFiles names plus one session's.
    std::string trimmed;
    for (auto name = kept.rbegin(); name != kept.rend(); ++name) {
        trimmed += *name + '\n';
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
    if (state.preload) {
        std::printf("Pipeline binaries: preloaded %zu of %zu recent files (%.1f MB) in %.0f ms\n",
                    loaded, kept.size(), loaded_bytes / 1048576.0, ms);
    } else {
        std::printf("Pipeline binaries: preload off (GOW3_PIPELINE_PRELOAD=0), %zu recent "
                    "files listed\n",
                    kept.size());
    }
    // gow3: the names the game uses are appended here every few seconds rather than by the
    // thread that builds the pipeline, which must not wait for the disk. The writes land in the
    // OS file cache; the few names of the last seconds before a forced exit are only missing
    // from the next preload.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::vector<std::string> used;
        {
            std::scoped_lock lock{state.mutex};
            used.swap(state.pending);
        }
        if (used.empty()) {
            continue;
        }
        std::ofstream out(list, std::ios::binary | std::ios::app);
        for (const auto& name : used) {
            out << name << '\n';
        }
    }
}
} // namespace

void Start(const std::filesystem::path& directory) {
    if (g_state.load(std::memory_order_acquire)) {
        return;
    }
    auto* state = new State;
    state->directory = directory;
    state->preload = EmulatorSettingsImpl::Flag("GOW3_PIPELINE_PRELOAD", true);
    g_state.store(state, std::memory_order_release);
    std::thread(Run, std::ref(*state)).detach();
}

std::optional<std::vector<u8>> Find(const std::filesystem::path& file) {
    State* state = Active(file);
    if (!state || !state->preload) {
        return std::nullopt;
    }
    std::scoped_lock lock{state->mutex};
    const auto it = state->bytes.find(file.stem().string());
    return it != state->bytes.end() ? std::optional{it->second} : std::nullopt;
}

void Used(const std::filesystem::path& file) {
    if (State* state = Active(file)) {
        std::scoped_lock lock{state->mutex};
        Record(*state, file.stem().string());
    }
}

void Saved(const std::filesystem::path& file) {
    if (State* state = Active(file)) {
        const auto name = file.stem().string();
        std::scoped_lock lock{state->mutex};
        Record(*state, name);
        state->bytes.erase(name);
    }
}

} // namespace Vulkan::PipelineBinaryPreload
