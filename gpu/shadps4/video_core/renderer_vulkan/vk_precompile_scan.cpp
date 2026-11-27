// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (vk_precompile_scan.h): the memory reads of the shader scan, and the diagnostic
// GOW3_PRECOMPILE_SCAN=1, which runs it while loading (CompilePolicy::loading) and logs each
// pass. With GOW3_PRECOMPILE_LIST=<file> it also logs how many of the list's programs the game
// used since the load began (their first use in this launch) and how many of those the scan had
// found before the load's first scene frame: every 5 s outside a load, and once more as the
// next load begins.
//
// Linux only (mincore, process_vm_readv); elsewhere the switch logs that it is not supported.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#ifdef __linux__
#include <fstream>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_compile_policy.h"
#include "video_core/renderer_vulkan/vk_precompile_scan.h"

namespace Vulkan::Precompile {

namespace {

constexpr u64 ReportIntervalNs = 5'000'000'000;

#ifdef __linux__
constexpr auto PassInterval = std::chrono::milliseconds(250);
constexpr size_t PassChunk = 1u << 20; ///< checked with mincore and read at a time
/// The guest range: the game's memory, not the host's.
constexpr u64 GuestLo = 0x0800000000ULL, GuestHi = 0xfc00000000ULL;

u64 HostPageSize() {
    static const u64 page = u64(sysconf(_SC_PAGESIZE));
    return page;
}

/// A guest read only where every page is resident, so it never pages anything in.
bool ReadResident(u64 address, void* out, size_t size, std::vector<unsigned char>& resident) {
    const u64 page = HostPageSize();
    const u64 lo = address & ~(page - 1);
    const u64 hi = address + size;
    resident.resize((hi - lo + page - 1) / page);
    if (size == 0 || mincore(reinterpret_cast<void*>(lo), hi - lo, resident.data()) != 0 ||
        !std::ranges::all_of(resident, [](unsigned char r) { return (r & 1) != 0; })) {
        return false;
    }
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
}

/// The readable mappings of this process inside the guest range.
std::vector<std::pair<u64, u64>> GuestRanges() {
    std::vector<std::pair<u64, u64>> ranges;
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        unsigned long lo = 0, hi = 0;
        char perms[5] = {};
        if (std::sscanf(line.c_str(), "%lx-%lx %4s", &lo, &hi, perms) != 3 || perms[0] != 'r') {
            continue;
        }
        const u64 from = std::max<u64>(lo, GuestLo), to = std::min<u64>(hi, GuestHi);
        if (from < to) {
            ranges.emplace_back(from, to);
        }
    }
    return ranges;
}
#endif

} // namespace

ShaderScan::~ShaderScan() {
    Stop();
}

void ShaderScan::Start() {
#ifdef __linux__
    if (!thread.joinable()) {
        stop = false;
        thread = std::thread([this] { Run(); });
    }
#endif
}

void ShaderScan::Stop() {
    {
        std::scoped_lock lk{mutex};
        stop = true;
    }
    wake.notify_one();
    if (thread.joinable()) {
        thread.join();
    }
}

void ShaderScan::SetLoading(bool value) {
    if (loading.load(std::memory_order_relaxed) == value) {
        return;
    }
    {
        std::scoped_lock lk{mutex};
        loading = value;
    }
    wake.notify_one();
}

void ShaderScan::Wake() {
    {
        std::scoped_lock lk{mutex};
        woken = true;
    }
    wake.notify_one();
}

void ShaderScan::Forget(u64 hash) {
    std::scoped_lock lk{mutex};
    forget.push_back(hash);
}

std::vector<FoundCode> ShaderScan::TakeNew() {
    std::scoped_lock lk{mutex};
    return std::exchange(fresh, {});
}

#ifdef __linux__
void ShaderScan::Run() {
    Common::SetCurrentThreadName("gow3:ShaderScan");
    setpriority(PRIO_PROCESS, 0, 19); // this thread
    // Each read lands after ScanOverlap bytes, which hold the last bytes of the read before
    // when that one ended where this one starts.
    std::vector<u8> buf(ScanOverlap + PassChunk);
    std::unique_lock lk{mutex};
    while (!stop) {
        wake.wait(lk, [this] { return stop || loading; });
        if (stop) {
            break;
        }
        woken = false;
        lk.unlock();
        Pass(buf);
        lk.lock();
        wake.wait_for(lk, PassInterval, [this] { return stop || woken; });
    }
}

void ShaderScan::Pass(std::vector<u8>& buf) {
    started.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lk{mutex};
        for (const u64 hash : forget) {
            known.erase(hash);
        }
        forget.clear();
    }
    const auto t0 = std::chrono::steady_clock::now();
    const u64 page = HostPageSize();
    const u64 own_lo = reinterpret_cast<u64>(buf.data()), own_hi = own_lo + buf.size();
    std::vector<unsigned char> resident, read_resident;
    const ReadFn read = [&read_resident](u64 address, void* out, size_t size) {
        return ReadResident(address, out, size, read_resident);
    };
    std::vector<FoundCode> found; // the whole pass's: FindShaderCode drops overlap repeats
    size_t counted = 0;
    std::unordered_set<u64> pass_hashes;
    u64 bytes_read = 0, new_hashes = 0, carried_to = 0;
    bool cut = false; ///< loading ended or Stop came first
    for (const auto& [lo, hi] : GuestRanges()) {
        for (u64 chunk = lo; chunk < hi && !cut; chunk += PassChunk) {
            cut = !loading || stop;
            if (cut) {
                break;
            }
            const u64 n = std::min<u64>(PassChunk, hi - chunk);
            resident.resize((n + page - 1) / page);
            if (mincore(reinterpret_cast<void*>(chunk), n, resident.data()) != 0) {
                continue;
            }
            for (size_t p = 0; p < resident.size();) {
                if (!(resident[p] & 1)) {
                    ++p;
                    continue;
                }
                size_t q = p;
                while (q < resident.size() && (resident[q] & 1)) {
                    ++q;
                }
                const u64 start = chunk + p * page;
                const size_t len = size_t(std::min<u64>((q - p) * page, hi - start));
                p = q;
                if (start < own_hi && start + len > own_lo) {
                    continue; // this scan's own buffer
                }
                iovec local{buf.data() + ScanOverlap, len},
                    remote{reinterpret_cast<void*>(start), len};
                const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
                if (got <= 0) {
                    continue; // unreadable now (protection changed): skipped
                }
                bytes_read += u64(got);
                const size_t carried = carried_to == start ? ScanOverlap : 0;
                FindShaderCode(std::span<const u8>{buf}.subspan(ScanOverlap - carried,
                                                                carried + size_t(got)),
                               start - carried, read, found);
                if (size_t(got) >= ScanOverlap) {
                    std::memcpy(buf.data(), buf.data() + size_t(got), ScanOverlap);
                    carried_to = start + u64(got);
                } else {
                    carried_to = 0;
                }
                for (; counted < found.size(); ++counted) {
                    const FoundCode& code = found[counted];
                    pass_hashes.insert(code.hash);
                    if (known.insert(code.hash).second) {
                        ++new_hashes;
                        std::scoped_lock lk{mutex};
                        fresh.push_back(code);
                    }
                }
            }
        }
    }
    const u64 ms = u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count());
    if (!cut) {
        last_pass_ms.store(ms, std::memory_order_relaxed);
    }
    const u64 pass = passes.fetch_add(1, std::memory_order_release) + 1;
    std::printf("Precompile scan: pass %llu, %llu MiB in %llu ms, %zu shaders (%llu new)%s\n",
                static_cast<unsigned long long>(pass),
                static_cast<unsigned long long>(bytes_read >> 20),
                static_cast<unsigned long long>(ms), pass_hashes.size(),
                static_cast<unsigned long long>(new_hashes), cut ? " (cut short)" : "");
}
#endif

namespace {

struct ScanDiagnostic {
    ShaderScan scan;
    std::unordered_set<u64> list_programs; ///< GOW3_PRECOMPILE_LIST's; set before it starts
    std::mutex mutex;                      ///< guards the rest
    bool loading = false;
    std::unordered_set<u64> found;          ///< every program the scan found (TakeNew)
    std::unordered_set<u64> found_at_scene; ///< list programs found before this load's scene
    std::unordered_set<u64> used;           ///< list programs first used since the load began
    u64 next_report_ns = 0;
};

/// Null while off. Never freed: a running thread must not be destroyed at exit.
std::atomic<ScanDiagnostic*> g_scan{nullptr};

} // namespace

void StartScanDiagnostic() {
    if (!ScanRequested() || g_scan.load(std::memory_order_acquire)) {
        return;
    }
    if (LoadRequested()) {
        // Never two scans at once, and the loader takes every program the scan finds.
        std::printf("Precompile scan: off, GOW3_PRECOMPILE_LOAD=1 runs its own scan\n");
        return;
    }
#ifndef __linux__
    std::printf("Precompile scan: not supported on this platform\n");
#else
    auto* diagnostic = new ScanDiagnostic;
    const char* list_path = std::getenv("GOW3_PRECOMPILE_LIST");
    if (list_path && list_path[0]) {
        if (const auto list = LoadListFile(list_path, MaxListEntries)) {
            for (const auto& key : list->keys) {
                for (const auto& stage : key.stages) {
                    diagnostic->list_programs.insert(stage.hash);
                }
            }
        }
    }
    diagnostic->scan.Start();
    g_scan.store(diagnostic, std::memory_order_release);
#endif
}

void ScanProgramUsed(u64 hash) {
    ScanDiagnostic* const diagnostic = g_scan.load(std::memory_order_acquire);
    if (!diagnostic || !diagnostic->list_programs.contains(hash)) {
        return;
    }
    std::scoped_lock lk{diagnostic->mutex};
    diagnostic->used.insert(hash);
}

void ScanFlipped(u64 now_ns, bool first_scene) {
    ScanDiagnostic* const diagnostic = g_scan.load(std::memory_order_acquire);
    if (!diagnostic) {
        return;
    }
    auto& d = *diagnostic;
    const bool loading = g_compile_policy.loading.load(std::memory_order_relaxed);
    d.scan.SetLoading(loading);
    if (d.list_programs.empty()) {
        return;
    }
    // The line is counted under the lock and printed after it. It comes every 5 s outside a
    // load and once more as the next load begins, before that load clears the counts.
    std::optional<std::pair<size_t, size_t>> line; ///< used, of them found before the scene
    {
        std::scoped_lock lk{d.mutex};
        const auto count = [&d] {
            size_t found_before = 0;
            for (const u64 hash : d.used) {
                found_before += d.found_at_scene.contains(hash);
            }
            return std::pair{d.used.size(), found_before};
        };
        if (loading && !d.loading) { // a load begins
            line = count();
            d.used.clear();
            d.found_at_scene.clear();
        }
        d.loading = loading;
        if (first_scene) {
            for (const auto& code : d.scan.TakeNew()) {
                d.found.insert(code.hash);
            }
            for (const u64 hash : d.list_programs) {
                if (d.found.contains(hash)) {
                    d.found_at_scene.insert(hash);
                }
            }
        }
        if (!loading && now_ns >= d.next_report_ns) {
            d.next_report_ns = now_ns + ReportIntervalNs;
            line = count();
        }
    }
    if (line) {
        std::printf("Precompile scan: %zu list programs used since the load, %zu of them found "
                    "before the first scene frame, last pass %llu ms\n",
                    line->first, line->second,
                    static_cast<unsigned long long>(d.scan.LastPassMs()));
    }
}

} // namespace Vulkan::Precompile
