// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void GpuProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* env = std::getenv("GOW3_GPU_PROFILE");
    if (!env || env[0] != '1' || instance_ptr) {
        return;
    }
    instance_ptr = new GpuProfiler(instance, scheduler);
}

GpuProfiler::GpuProfiler(const Instance& instance, Scheduler& scheduler_)
    : device{instance.GetDevice()}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    };
    pool = Check<"create timestamp query pool">(device.createQueryPoolUnique(info));
    device.resetQueryPool(*pool, 0, NumSlices * SliceQueries);
    period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    for (auto& k : keys) {
        k.reserve(SliceQueries);
    }
    std::printf("GPU profile: on (timestamp period %.2f ns)\n", period_ns);
}

void GpuProfiler::WriteTimestamp(u64 key) {
    if (paused || used[slice] + 1 >= SliceQueries) {
        return; // the frame's slice is full: the rest of the frame goes to the last label
    }
    // Outside render passes: radv_CmdWriteTimestamp2 crashed inside some. Marks sit where a
    // pass, dispatch or submission ends anyway.
    scheduler.EndRendering();
    const u32 query = slice * SliceQueries + used[slice]++;
    keys[slice].push_back(key);
    current = key;
    scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuProfiler::BeginFrame() {
    const auto now = std::chrono::steady_clock::now();
    if (last_frame.time_since_epoch().count() != 0) {
        wall_ms += std::chrono::duration<double, std::milli>(now - last_frame).count();
        ++wall_frames;
    }
    last_frame = now;
    // Close the frame: one more timestamp without a label.
    if (!paused && used[slice] > 0 && used[slice] < SliceQueries) {
        scheduler.EndRendering();
        const u32 query = slice * SliceQueries + used[slice]++;
        scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        });
        pending[slice] = true;
        ticks[slice] = scheduler.CurrentTick();
    }
    // The oldest slice, four frames back. While the GPU has not finished it (or its command
    // buffer is not even submitted), its queries can be neither read nor reset: skip this
    // frame's timestamps instead of waiting, which deadlocked during video playback.
    const u32 next = (slice + 1) % NumSlices;
    paused = pending[next] && !scheduler.IsFree(ticks[next]);
    if (paused) {
        ++skipped;
        Print();
        return;
    }
    slice = next;
    if (pending[slice]) {
        Collect(slice);
    }
    if (used[slice]) {
        device.resetQueryPool(*pool, slice * SliceQueries, used[slice]);
    }
    used[slice] = 0;
    keys[slice].clear();
    pending[slice] = false;
    Print();
}

void GpuProfiler::Collect(u32 which) {
    const u32 count = used[which];
    std::vector<u64> stamps(count);
    // BeginFrame only collects a slice the GPU has finished, so the results are there.
    const auto result = device.getQueryPoolResults(
        *pool, which * SliceQueries, count, count * sizeof(u64), stamps.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    for (u32 i = 0; i + 1 < count; ++i) {
        const double ms = double(stamps[i + 1] - stamps[i]) * period_ns / 1e6;
        const u64 key = keys[which][i];
        if (const auto parent = parents.find(key); parent != parents.end()) {
            auto& step = steps[key];
            step.ms += ms;
            ++step.segments;
            totals[parent->second].ms += ms; // the parent's line keeps its total
            continue;
        }
        auto& total = totals[key];
        total.ms += ms;
        ++total.segments;
    }
    ++frames;
}

void GpuProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5) || frames == 0) {
        return;
    }
    window = now;
    std::vector<std::pair<u64, Total>> sorted(totals.begin(), totals.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double sum = 0;
    for (const auto& [key, total] : sorted) {
        sum += total.ms;
    }
    // On GPU-bound frames the GPU ms per frame should come close to the time between frames.
    std::printf("GPU profile: %.2f ms/frame over %llu frames (%llu skipped, GPU behind), frames "
                "%.2f ms apart, %zu labels\n",
                sum / frames, static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(skipped),
                wall_frames ? wall_ms / double(wall_frames) : 0.0, sorted.size());
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 40); ++i) {
        const auto& [key, total] = sorted[i];
        std::printf("  %6.3f ms/frame %5.1f/frame  %s\n", total.ms / frames,
                    double(total.segments) / frames, described[key].c_str());
    }
    // Steps after the list, in another format: a reader of the list above stops before them.
    std::vector<std::pair<u64, Total>> step_list(steps.begin(), steps.end());
    std::ranges::sort(step_list, [&](const auto& a, const auto& b) {
        return described[a.first] < described[b.first];
    });
    for (const auto& [key, total] : sorted) {
        double in_steps = 0;
        for (const auto& [step, step_total] : step_list) {
            in_steps += parents[step] == key ? step_total.ms : 0.0;
        }
        if (in_steps == 0) {
            continue;
        }
        std::printf("GPU profile steps of %s: %.3f ms a frame\n", described[key].c_str(),
                    total.ms / frames);
        std::printf("    step %6.3f ms %5.1f/f  0 (the label itself, until its first step)\n",
                    (total.ms - in_steps) / frames, double(total.segments) / frames);
        for (const auto& [step, step_total] : step_list) {
            if (parents[step] == key) {
                std::printf("    step %6.3f ms %5.1f/f  %s\n", step_total.ms / frames,
                            double(step_total.segments) / frames, described[step].c_str());
            }
        }
    }
    totals.clear();
    steps.clear();
    frames = 0;
    skipped = 0;
    wall_ms = 0;
    wall_frames = 0;
}

} // namespace Vulkan
