// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void GpuProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* env = std::getenv("GOW3_GPU_PROFILE");
    if (!env || (env[0] != '1' && env[0] != '2') || instance_ptr) {
        return;
    }
    instance_ptr = new GpuProfiler(instance, scheduler);
    instance_ptr->steps = env[0] == '2';
#ifdef __linux__
    instance_ptr->calibrated = instance.GpuProfileCalibrated();
#endif
    std::printf("GPU profile: idle gaps %s\n",
                instance_ptr->calibrated ? "are split by the time of the next submission"
                                         : "are not split (no calibrated timestamps)");
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
    base = key;
    scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

namespace {
s64 MonotonicNs() {
#ifdef __linux__
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return s64{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
#else
    return 0;
#endif
}
} // namespace

void GpuProfiler::NoteIdleMark() {
    idle_cur.valid = false;
    if (!calibrated || paused || keys[slice].empty() || keys[slice].back() != IdleKey) {
        return;
    }
    idle_events[slice].push_back({used[slice] - 1, submit_ordinal++});
    idle_cur = {slice, u32(idle_events[slice].size() - 1), true};
}

void GpuProfiler::NoteSubmitted() {
    if (!calibrated) {
        return;
    }
    // This submission ends the gap that the previous one's idle label opened.
    if (idle_prev.valid && idle_prev.pos < idle_events[idle_prev.slice].size()) {
        idle_events[idle_prev.slice][idle_prev.pos].next_submit_ns = MonotonicNs();
    }
    idle_prev = idle_cur;
    idle_cur.valid = false;
}

void GpuProfiler::Step(u32 kind) {
    if (!steps || paused || base == 0 || used[slice] + 2 >= SliceQueries) {
        return;
    }
    const u64 key = base ^ (0x9E3779B97F4A7C15ull * kind);
    if (!described.contains(key)) {
        described.emplace(key, described[base] + (kind == 1 ? " | draws" : " | after"));
    }
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
    idle_events[slice].clear();
    if (idle_prev.valid && idle_prev.slice == slice) {
        idle_prev.valid = false;
    }
    submit_ordinal = 0;
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
        auto& total = totals[keys[which][i]];
        total.ms += ms;
        ++total.segments;
    }
#ifdef __linux__
    if (calibrated && !idle_events[which].empty()) {
        // Device time and CLOCK_MONOTONIC read together: the offset between the two clocks.
        const VkCalibratedTimestampInfoEXT infos[2] = {
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr, VK_TIME_DOMAIN_DEVICE_EXT},
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, nullptr,
             VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT},
        };
        u64 now[2]{}, deviation{};
        if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsEXT &&
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsEXT(
                static_cast<VkDevice>(device), 2, infos, now, &deviation) == VK_SUCCESS) {
            const double offset_ns = double(now[1]) - double(now[0]) * period_ns;
            for (const IdleEvent& event : idle_events[which]) {
                if (event.next_submit_ns == 0 || event.index + 1 >= count) {
                    continue;
                }
                const double start = double(stamps[event.index]) * period_ns + offset_ns;
                const double gap = double(stamps[event.index + 1] - stamps[event.index]) * period_ns;
                const double wait = std::clamp(double(event.next_submit_ns) - start, 0.0, gap);
                auto& total = idle_totals[std::min<u32>(event.ordinal, 3)];
                total.gap_ms += gap / 1e6;
                total.wait_ms += wait / 1e6;
                ++total.gaps;
                total.queued += double(event.next_submit_ns) <= start;
            }
        }
    }
#endif
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
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), steps ? 1500 : 40); ++i) {
        const auto& [key, total] = sorted[i];
        std::printf("  %6.3f ms/frame %5.1f/frame  %s\n", total.ms / frames,
                    double(total.segments) / frames, described[key].c_str());
    }
    for (u32 i = 0; i < idle_totals.size(); ++i) {
        const IdleTotal& t = idle_totals[i];
        if (t.gaps == 0) {
            continue;
        }
        std::printf("  idle gap after submission %u%s of a frame: %.3f ms/frame in %.2f gaps/frame; "
                    "%.3f ms/frame before the next submission reached the driver, %.3f after; "
                    "next one already there in %.0f %% of gaps\n",
                    i + 1, i == 3 ? "+" : "", t.gap_ms / frames, double(t.gaps) / frames,
                    t.wait_ms / frames, (t.gap_ms - t.wait_ms) / frames,
                    100.0 * double(t.queued) / double(t.gaps));
    }
    idle_totals = {};
    totals.clear();
    frames = 0;
    skipped = 0;
    wall_ms = 0;
    wall_frames = 0;
}

} // namespace Vulkan
