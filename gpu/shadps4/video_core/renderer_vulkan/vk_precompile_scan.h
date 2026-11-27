// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 (GOW3_PRECOMPILE_SCAN, vk_precompile_scan.cpp): where each PS4 program's code is in the
// game's memory. A scan reads guest memory in buffers and finds the code by its OrbShdr header
// (AmdGpu::BinaryInfo), which holds the program hash; the code starts before it with the start
// token (vk_precompile_list.h). The search is here; the memory reads are in the .cpp.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"

namespace Vulkan::Precompile {

/// A program's code: its hash and the guest address of its start token.
struct FoundCode {
    u64 hash;
    u64 address;
    bool operator==(const FoundCode&) const = default;
};

/// A guest read that fails instead of faulting.
using ReadFn = std::function<bool(u64 address, void* out, size_t size)>;

constexpr size_t ScanHeaderBytes = CodeHeaderWords * 4;
/// A scan's buffers overlap by the header size: a header split across two is whole in the
/// second.
constexpr size_t ScanOverlap = ScanHeaderBytes;
/// Below its buffer the walk back to a start reads this much at a time.
constexpr u64 ScanReadBlock = 4096;

namespace ScanDetail {

/// Where the code whose OrbShdr header is at `header`, inside `bytes` at `base`, starts: the
/// nearest start token below it whose second word puts the header there (CodeHeaderAt), within
/// MaxCodeBytes, and whose code passes the recorder's check (ReadProgramCode, CheckCode).
/// Candidates below `bytes` are read through `read`, a block at a time, down to the first block
/// that cannot be read.
inline std::optional<u64> CodeStart(std::span<const u8> bytes, u64 base, u64 header, u64 hash,
                                    const ReadFn& read) {
    const u64 reach = std::min<u64>(header, MaxCodeBytes - ScanHeaderBytes);
    // The bytes candidates are read from, at block_lo; a block read below `bytes` keeps the 8
    // bytes above it, which a candidate may reach into.
    std::span<const u8> block = bytes;
    u64 block_lo = base;
    std::vector<u8> below; ///< the block read below `bytes`, reused
    for (u64 back = 8; back <= reach; back += 8) {
        const u64 start = header - back;
        while (start < block_lo) {
            const u64 hi = block_lo;
            const u64 lo = std::max(header - reach, (hi - 1) & ~(ScanReadBlock - 1));
            std::array<u8, 8> above;
            std::memcpy(above.data(), block.data(), above.size());
            below.resize(hi - lo + above.size());
            if (!read(lo, below.data(), hi - lo)) {
                return std::nullopt;
            }
            std::memcpy(below.data() + (hi - lo), above.data(), above.size());
            block = below;
            block_lo = lo;
        }
        std::array<u32, 2> words;
        std::memcpy(words.data(), block.data() + (start - block_lo), sizeof(words));
        if (words[0] != CodeStartToken || CodeHeaderAt(words) * 4 != back) {
            continue;
        }
        const auto code = ReadProgramCode(start, read);
        if (!code.empty() && !CheckCode(hash, code)) {
            return start;
        }
    }
    return std::nullopt;
}

} // namespace ScanDetail

/// Appends each program `bytes` (guest memory at `base`) holds the OrbShdr header of: its hash
/// and where its code starts (ScanDetail::CodeStart). Buffers read one after another overlap by
/// ScanOverlap; a header whole in both is appended once when `out` is shared.
inline void FindShaderCode(std::span<const u8> bytes, u64 base, const ReadFn& read,
                           std::vector<FoundCode>& out) {
    for (size_t at = 0; at + ScanHeaderBytes <= bytes.size(); ++at) {
        const void* hit = std::memchr(bytes.data() + at, CodeSignature[0],
                                      bytes.size() - ScanHeaderBytes + 1 - at);
        if (!hit) {
            return;
        }
        at = size_t(static_cast<const u8*>(hit) - bytes.data());
        if (std::memcmp(bytes.data() + at, CodeSignature.data(), CodeSignature.size()) != 0) {
            continue;
        }
        u64 hash;
        std::memcpy(&hash, bytes.data() + at + CodeHeaderHashAt, sizeof(hash));
        const auto start = ScanDetail::CodeStart(bytes, base, base + at, hash, read);
        if (start && (out.empty() || out.back() != FoundCode{hash, *start})) {
            out.push_back({hash, *start});
        }
    }
}

/// A thread at nice 19 that, while loading, scans the resident readable memory of the guest
/// range for program code, sleeping 250 ms between passes (Linux only: elsewhere Start does
/// nothing). It never faults: pages are checked with mincore and read with
/// process_vm_readv.
class ShaderScan {
public:
    ~ShaderScan();

    void Start();
    void Stop();
    /// Passes run only while this is true.
    void SetLoading(bool loading);
    /// Ends the sleep between passes now.
    void Wake();
    /// Lets a later pass report `hash` again (TakeNew), at the address it sees first then.
    void Forget(u64 hash);

    /// Passes run, those cut short when loading ended included. What they found is in TakeNew
    /// by then.
    [[nodiscard]] u64 Passes() const {
        return passes.load(std::memory_order_acquire);
    }
    /// Passes begun, the running one included: a pass not counted in it reads the memory after
    /// the call.
    [[nodiscard]] u64 Started() const {
        return started.load(std::memory_order_relaxed);
    }
    /// The last pass that was not cut short.
    [[nodiscard]] u64 LastPassMs() const {
        return last_pass_ms.load(std::memory_order_relaxed);
    }
    /// The programs found since the last call whose hash the scan had not found before: one
    /// address each, the first seen.
    [[nodiscard]] std::vector<FoundCode> TakeNew();

private:
    void Run();
    void Pass(std::vector<u8>& buf);

    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> loading{false}, stop{false};
    bool woken = false;            ///< under mutex
    std::vector<FoundCode> fresh;  ///< under mutex: TakeNew
    std::vector<u64> forget;       ///< under mutex: Forget, taken out of `known` by each pass
    std::unordered_set<u64> known; ///< the scan thread's: every hash found
    std::atomic<u64> passes{0}, started{0}, last_pass_ms{0};
};

/// Whether GOW3_PRECOMPILE_SCAN=1 asks for the scan diagnostic.
[[nodiscard]] inline bool ScanRequested() {
    const char* scan = std::getenv("GOW3_PRECOMPILE_SCAN");
    return scan && scan[0] == '1';
}

/// gow3 diagnostic GOW3_PRECOMPILE_SCAN=1: runs a ShaderScan while loading and logs it. Each
/// does nothing while it is off.
void StartScanDiagnostic();                     ///< the PipelineCache constructor
void ScanProgramUsed(u64 hash);                 ///< GetProgram: a program's first use
void ScanFlipped(u64 now_ns, bool first_scene); ///< each flip, after CompilePolicy::Flipped

} // namespace Vulkan::Precompile
