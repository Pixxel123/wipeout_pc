// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 diagnostic GOW3_PRECOMPILE_RECORD=<file>: records the guest inputs of each pipeline new in
// this launch into a precompile list (vk_precompile_list.h), which a later launch's start-up
// rebuild (GOW3_PRECOMPILE_BOOT, vk_precompile_boot.cpp) translates back into the shader store.
//
// Capture: when a draw selects a graphics pipeline key that is new in this launch, copy the
// registers, the draw-indirect parameters and per stage the program hash, user data and code,
// plus every guest word the stage's SRT walker and fetch-shader parse read, with the pointer
// each was read through. The walker is JIT code: it runs once more with fake pointers into
// reserved, inaccessible memory, so each of its reads faults into the fault handler
// (vk_precompile_guard.cpp), which reads the guest word, records it and hands back the value
// (or the fake pointer for the next table). The fetch shader and vertex buffer descriptors are
// recorded from the parse the draw just did. Each capture is written as it is made, after the
// code of each program it is the first to use.
//
// Linux only (the fault handler edits the walker's registers in the signal context). While it
// is on, the page manager's read-fault count also includes the faults of recorded walks.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <fmt/format.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_precompile_guard.h"
#include "video_core/renderer_vulkan/vk_precompile_list.h"
#include "video_core/renderer_vulkan/vk_precompile_record.h"
#include "video_core/renderer_vulkan/vk_precompile_shadow.h"

namespace Vulkan {

namespace {

using namespace Precompile;
using Shader::HwStage;
using Shader::SwStage;

constexpr u32 MaxCaptures = 2000;
constexpr u32 MaxDetails = 8; ///< capture failure lines
constexpr auto ReportInterval = std::chrono::seconds(5);

static_assert(ListRegsWords == AmdGpu::Regs::NumRegs);
static_assert(ListUserDataWords == AmdGpu::NUM_USER_DATA);
static_assert(ListStages == MaxShaderStages);
// num_interp is the word between ps_input_addr's padding and barycentric_control's.
static_assert(offsetof(AmdGpu::Regs, ps_input_addr) / sizeof(u32) + 2 == ListPsInControlReg);
static_assert(offsetof(AmdGpu::Regs, barycentric_control) / sizeof(u32) ==
              ListPsInControlReg + 2);
static_assert(ListMaxPsInputs ==
              std::tuple_size_v<decltype(Shader::HwFragmentRuntimeInfo::inputs)>);

struct StageCapture {
    SwStage sw_stage;
    HwStage hw_stage;
    u64 hash;
    VAddr code;
    u64 walker, walker_size; ///< the SRT walker's code
    AmdGpu::UserData user_data;
    std::vector<std::pair<u32, u64>> roots; ///< user-data pointers followed: first sgpr, target
};

/// One new pipeline key: everything its rebuild may read.
struct KeyCapture {
    std::unique_ptr<AmdGpu::Regs> regs;
    DrawIndirectParams draw_indirect_params;
    u64 key_hash;
    std::vector<StageCapture> stages;
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
};

/// Notes this thread's faults; reads of the stages' walkers are served (vk_precompile_guard.h).
void WatchStages(std::span<const StageCapture> stages) {
    std::array<ThreadState::Range, MaxShaderStages> walkers{};
    for (size_t i = 0; i < stages.size() && i < walkers.size(); ++i) {
        walkers[i] = {stages[i].walker, stages[i].walker + stages[i].walker_size};
    }
    FaultState().Watch(walkers);
}

void AddRoot(StageCapture& stage, u32 sgpr, u64 target) {
    for (const auto& root : stage.roots) {
        if (root.first == sgpr) {
            return;
        }
    }
    stage.roots.emplace_back(sgpr, target);
}

/// New keys captured or not, captures written to the list file, already in it or not written,
/// programs whose code was written.
struct RecordTally {
    u64 captured, capture_failed, skipped, words;
    u64 written, bytes, already, not_written, code_written, code_missing;
    bool operator==(const RecordTally&) const = default;
};

/// A program's code from its start token through its OrbShdr header (vk_precompile_list.h),
std::vector<u32> ReadProgramCode(VAddr address) {
    std::array<u32, 2> start;
    if (!ReadGuest(address, start.data(), sizeof(start)) || start[0] != CodeStartToken) {
        return {};
    }
    const u64 header = CodeHeaderAt(start);
    std::array<u32, CodeHeaderWords> info;
    if ((header + CodeHeaderWords) * 4 > MaxCodeBytes ||
        !ReadGuest(address + header * 4, info.data(), sizeof(info))) {
        return {};
    }
    u32 length;
    std::memcpy(&length, reinterpret_cast<const u8*>(info.data()) + CodeHeaderLengthAt,
                sizeof(length));
    const u64 words = std::max<u64>(header + CodeHeaderWords, ((length >> 8) + 3) / 4);
    if (words * 4 > MaxCodeBytes) {
        return {};
    }
    std::vector<u32> code(words);
    if (!ReadGuest(address, code.data(), words * 4)) {
        return {};
    }
    return code;
}

} // namespace

struct PrecompileRecord {
    PrecompileRecord(std::FILE* record_file_, ListAppend&& kept)
        : record_file{record_file_}, list_bytes{kept.keep ? kept.bytes - EncodeListHeader().size()
                                                          : 0},
          keys{std::move(kept.keys)}, code_seen{kept.code.begin(), kept.code.end()},
          slots{g_reserved.record, RecordSlotSize, NumRecordSlots} {}
    ~PrecompileRecord() {
        if (record_file) {
            std::fclose(record_file);
        }
    }

    void Capture(PipelineCache& cache);
    void Report(bool final);

    [[nodiscard]] bool Capturing() const {
        return record_file;
    }

private:
    void Record(const KeyCapture& c);
    void RecordCode(const StageCapture& stage);
    bool WriteRecord(const std::vector<u8>& bytes);
    const char* RecordStage(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordWalker(const Shader::Info& info, StageCapture& stage, KeyCapture& c);
    const char* RecordFetch(const Shader::Info& info, StageCapture& stage, KeyCapture& c);

    std::FILE* record_file; ///< null once a write failed
    size_t list_bytes;      ///< the file's entries, kept and written: what the size cap counts
    RecordTally record{}, record_reported{};
    bool record_capped_logged = false;
    std::set<u64> keys;                ///< the file's keys, by KeyContentHash
    std::unordered_set<u64> code_seen; ///< programs whose code is in the file or not found
    RecordSlots slots;
    std::unordered_set<u64> seen; ///< key hashes already captured or skipped
    u32 failure_lines = 0;
    std::chrono::steady_clock::time_point last_report{};
    bool cap_logged = false, read_logged = false;
};

namespace {
PrecompileRecord* g_active; ///< reported at exit

void ReportAtExit() {
    if (g_active) {
        g_active->Report(true);
    }
}
} // namespace

void PrecompileRecord::Report(bool final) {
    const auto now = std::chrono::steady_clock::now();
    if (!final && (now - last_report < ReportInterval || record == record_reported)) {
        return;
    }
    last_report = now;
    record_reported = record;
    const auto& t = record;
    std::printf("%s\n",
                fmt::format("Precompile record: {} captured, {} capture failed, {} skipped "
                            "(cap/hs/motion), {} guest words, {} written ({} KiB), {} already "
                            "in the list, {} not written (geometry/size/write), code of {} "
                            "programs, {} without code{}",
                            t.captured, t.capture_failed, t.skipped, t.words, t.written,
                            t.bytes >> 10, t.already, t.not_written, t.code_written,
                            t.code_missing, final ? " (exit)" : "")
                    .c_str());
}

void PrecompileRecord::Capture(PipelineCache& cache) {
    const auto& sel = cache.sel;
    const u64 key_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
    if (!seen.insert(key_hash).second) {
        return;
    }
    // Tessellation reads constants at selection time (not prepared either); the motion choice
    // reads a global that may have moved on by the rebuild.
    if (sel.regs->stage_enable.hs_en || sel.graphics_key.motion_vectors) {
        ++record.skipped;
        return;
    }
    if (record.captured + record.capture_failed >= MaxCaptures) {
        ++record.skipped;
        if (!cap_logged) {
            cap_logged = true;
            std::printf("Precompile record: %u keys captured, later ones skipped\n", MaxCaptures);
        }
        return;
    }
    KeyCapture c{};
    c.regs = std::make_unique<AmdGpu::Regs>(*sel.regs);
    c.draw_indirect_params = sel.draw_indirect_params;
    c.key_hash = key_hash;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        const auto* info = sel.infos[i];
        if (!info) {
            continue;
        }
        auto& stage = c.stages.emplace_back();
        stage.sw_stage = SwStage(i);
        stage.hw_stage = info->hw_stage;
        stage.hash = info->pgm_hash;
        stage.code = sel.regs->ProgramForStage(u32(info->hw_stage))->Address<VAddr>();
        stage.walker = u64(info->srt_info.walker_func);
        stage.walker_size = info->srt_info.walker_func_size;
        if (const char* error = RecordStage(*info, stage, c)) {
            ++record.capture_failed;
            if (failure_lines++ < MaxDetails) {
                std::printf("Precompile record: capture of key %016llx failed, stage %u "
                            "%016llx: %s\n",
                            static_cast<unsigned long long>(key_hash), i,
                            static_cast<unsigned long long>(stage.hash), error);
            }
            if (FirstReadError() && !read_logged) {
                read_logged = true;
                std::printf("Precompile record: first failed guest read: %s\n",
                            std::strerror(FirstReadError()));
            }
            return;
        }
    }
    ++record.captured;
    record.words += c.words.size();
    Record(c);
}

/// Appends a capture to the list file, after the code of programs it uses first.
void PrecompileRecord::Record(const KeyCapture& c) {
    // The rebuild takes vertex + fragment keys only: a geometry key's selection also reads
    // the copy shader through vs_program, which is not among the recorded stages.
    if (c.regs->stage_enable.raw != AmdGpu::ShaderStageEnable::Vs) {
        ++record.not_written;
        return;
    }
    RecordedKey key{};
    key.key_hash = c.key_hash;
    key.vertex_sgpr_offset = c.draw_indirect_params.vertex_sgpr_offset;
    key.instance_sgpr_offset = c.draw_indirect_params.instance_sgpr_offset;
    for (u32 i = 0; i < AmdGpu::Regs::NumRegs; ++i) {
        if (const u32 value = c.regs->reg_array[i]) {
            key.regs.emplace_back(i, value);
        }
    }
    for (const auto& stage : c.stages) {
        key.stages.push_back({u32(stage.sw_stage), u32(stage.hw_stage), stage.hash,
                              stage.user_data, stage.roots});
    }
    key.words = c.words;
    key.pointers = c.pointers;
    // An earlier launch recorded it (and its programs' code) already.
    const u64 content = KeyContentHash(key);
    if (keys.contains(content)) {
        ++record.already;
        return;
    }
    for (const auto& stage : c.stages) {
        RecordCode(stage);
    }
    if (record_file && WriteRecord(EncodeListEntry(key))) {
        ++record.written;
        keys.insert(content);
    } else {
        ++record.not_written;
    }
}

/// Writes a program's code before the first entry that uses it, once per program.
void PrecompileRecord::RecordCode(const StageCapture& stage) {
    if (!record_file || !code_seen.insert(stage.hash).second) {
        return;
    }
    const auto code = ReadProgramCode(stage.code);
    if (code.empty() || CheckCode(stage.hash, code) ||
        !WriteRecord(EncodeListCode(stage.hash, code))) {
        ++record.code_missing;
        return;
    }
    ++record.code_written;
}

/// Appends one entry to the list file, flushed at once: the game may end without exiting.
bool PrecompileRecord::WriteRecord(const std::vector<u8>& bytes) {
    if (!ListHasRoom(list_bytes, bytes.size())) {
        if (!record_capped_logged) {
            record_capped_logged = true;
            std::printf("Precompile record: list full (%zu MiB), later captures not written\n",
                        MaxListBytes >> 20);
        }
        return false;
    }
    if (std::fwrite(bytes.data(), 1, bytes.size(), record_file) != bytes.size() ||
        std::fflush(record_file) != 0) {
        std::printf("Precompile record: write failed (%s), recording stopped\n",
                    std::strerror(errno));
        std::fclose(record_file);
        record_file = nullptr;
        return false;
    }
    record.bytes += bytes.size();
    list_bytes += bytes.size();
    return true;
}

const char* PrecompileRecord::RecordStage(const Shader::Info& info, StageCapture& stage,
                                          KeyCapture& c) {
    if (info.user_data.size() != stage.user_data.size()) {
        return "unexpected user data size";
    }
    std::copy(info.user_data.begin(), info.user_data.end(), stage.user_data.begin());
    stage.roots.reserve(stage.user_data.size());
    const char* error = nullptr;
    const bool completed = Guarded([&] {
        error = RecordWalker(info, stage, c);
        if (!error) {
            error = RecordFetch(info, stage, c);
        }
    });
    FaultState().Stop();
    return completed ? error : "fault while recording";
}

const char* PrecompileRecord::RecordWalker(const Shader::Info& info, StageCapture& stage,
                                           KeyCapture& c) {
    if (!info.srt_info.walker_func) {
        return nullptr;
    }
    std::vector<u32> flat(info.flattened_ud_buf.size());
    if (flat.size() < stage.user_data.size()) {
        return "flattened user data smaller than the user data";
    }
    std::copy(stage.user_data.begin(), stage.user_data.end(), flat.begin());
    std::vector<GuestWord> words;
    std::vector<GuestPointer> pointers;
    words.reserve(MaxRecordedWords);
    pointers.reserve(MaxRecordedPointers);
    slots.Reset();
    WalkRecorder recorder{slots, stage.user_data, ReadGuest, words, pointers, stage.roots};
    auto& state = FaultState();
    WatchStages({&stage, 1});
    state.recorder = &recorder;
    info.srt_info.walker_func(reinterpret_cast<const u32*>(slots.Address(0)), flat.data());
    const u32 faults = state.faults;
    state.Stop();
    if (recorder.error) {
        return recorder.error;
    }
    if (faults != 0) {
        return "a read outside the record slots faulted";
    }
    // The recorded walk must give exactly what the draw's own walk gave.
    if (flat != info.flattened_ud_buf) {
        return "recorded walk differs from the draw's";
    }
    c.words.insert(c.words.end(), words.begin(), words.end());
    c.pointers.insert(c.pointers.end(), pointers.begin(), pointers.end());
    return nullptr;
}

const char* PrecompileRecord::RecordFetch(const Shader::Info& info, StageCapture& stage,
                                          KeyCapture& c) {
    if (!info.has_fetch_shader) {
        return nullptr;
    }
    // The draw just parsed it on this thread: the parse cache answers.
    const auto fetch = Shader::Gcn::ParseFetchShader(info);
    if (!fetch) {
        return nullptr;
    }
    const auto pointer_at = [&](u32 sgpr) -> std::optional<u64> {
        if (sgpr + 1 >= stage.user_data.size()) {
            return std::nullopt;
        }
        u64 pointer;
        std::memcpy(&pointer, &stage.user_data[sgpr], sizeof(pointer));
        return pointer & GuestAddressMask;
    };
    const auto code = pointer_at(info.fetch_shader_sgpr_base);
    if (!code) {
        return "fetch shader pointer out of range";
    }
    AddRoot(stage, info.fetch_shader_sgpr_base, *code);
    const auto* code_words = reinterpret_cast<const u32*>(*code);
    for (u32 i = 0; i < fetch->size / sizeof(u32); ++i) {
        c.words.push_back({*code, i * u32(sizeof(u32)), code_words[i]});
    }
    for (const auto& attrib : fetch->attributes) {
        const auto table = pointer_at(attrib.sgpr_base);
        if (!table) {
            return "vertex buffer table pointer out of range";
        }
        AddRoot(stage, attrib.sgpr_base, *table);
        const auto* sharp = reinterpret_cast<const u32*>(*table) + attrib.dword_offset;
        for (u32 w = 0; w < sizeof(AmdGpu::Buffer) / sizeof(u32); ++w) {
            c.words.push_back({*table, (attrib.dword_offset + w) * u32(sizeof(u32)), sharp[w]});
        }
    }
    return nullptr;
}

void PrecompileRecordDeleter::operator()(PrecompileRecord* record) const {
    if (g_active == record) {
        g_active = nullptr;
    }
    delete record;
}

namespace {
/// The most a list file can hold.
size_t MaxListFileBytes() {
    return MaxListBytes + EncodeListHeader().size();
}

/// Reads a whole file, but not much more than `limit` bytes: a larger file shows by its size.
/// False when it cannot be read (errno says why).
bool ReadFileBytes(const char* path, size_t limit, std::vector<u8>& bytes) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    bytes.clear();
    u8 chunk[1 << 16];
    for (size_t read; bytes.size() <= limit && (read = std::fread(chunk, 1, sizeof(chunk), file));) {
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    const bool failed = std::ferror(file);
    std::fclose(file);
    if (failed) {
        errno = EIO;
    }
    return !failed;
}
} // namespace

namespace Precompile {

std::optional<ParsedList> LoadListFile(const char* path, u32 max_keys) {
    std::vector<u8> bytes;
    if (!ReadFileBytes(path, MaxListFileBytes(), bytes)) {
        std::printf("Precompile list: cannot read %s (%s), off\n", path, std::strerror(errno));
        return std::nullopt;
    }
    if (bytes.size() > MaxListFileBytes()) {
        std::printf("Precompile list: %s is larger than %zu MiB, rejected\n", path,
                    MaxListBytes >> 20);
        return std::nullopt;
    }
    auto parsed = ParseList(bytes, max_keys);
    switch (parsed.status) {
    case ParsedList::Status::Ok:
        std::printf("Precompile list: %s: %zu entries, %u rejected\n", path, parsed.keys.size(),
                    parsed.rejected);
        return parsed;
    case ParsedList::Status::NotAList:
        std::printf("Precompile list: %s is not a precompile list, rejected\n", path);
        break;
    case ParsedList::Status::OtherVersion:
        std::printf("Precompile list: %s has version %u, this build reads %u, rejected\n", path,
                    parsed.version, ListVersion);
        break;
    case ParsedList::Status::OtherLayout:
        std::printf("Precompile list: %s has another register layout, rejected\n", path);
        break;
    }
    return std::nullopt;
}

} // namespace Precompile

namespace {
/// The value of an environment switch that names a file; null when unset or empty.
const char* PathSetting(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] ? value : nullptr;
}

/// Whether two paths name one file; for a path that does not exist yet, whether they are equal.
bool SameFile(const char* a, const char* b) {
    std::error_code error;
    const bool same = std::filesystem::equivalent(a, b, error);
    return error ? std::string_view{a} == b : same;
}

/// The list file to record into, null when it cannot be written (logged). A list of this
/// version is kept and appended to, without a cut-off last entry (the game may end inside a
/// write); anything else is written anew. `kept` says what the file holds.
std::FILE* OpenRecord(const char* path, ListAppend& kept) {
    std::vector<u8> existing;
    if (!ReadFileBytes(path, MaxListFileBytes(), existing) && errno != ENOENT) {
        std::printf("Precompile record: cannot read %s (%s), off\n", path, std::strerror(errno));
        return nullptr;
    }
    kept = PlanListAppend(existing);
    if (kept.keep) {
        std::error_code error;
        if (existing.size() > kept.bytes) {
            std::filesystem::resize_file(path, kept.bytes, error);
        }
        std::FILE* file = error ? nullptr : std::fopen(path, "ab");
        if (!file) {
            std::printf("Precompile record: cannot write %s (%s), off\n", path,
                        error ? error.message().c_str() : std::strerror(errno));
            return nullptr;
        }
        std::printf("Precompile record: appending to %s (%zu keys, code of %zu programs%s)\n",
                    path, kept.keys.size(), kept.code.size(),
                    existing.size() > kept.bytes ? ", a cut-off last entry dropped" : "");
        return file;
    }
    if (kept.why) {
        std::printf("Precompile record: %s %s, written anew\n", path, kept.why);
    }
    std::FILE* file = std::fopen(path, "wb");
    const auto header = EncodeListHeader();
    if (!file || std::fwrite(header.data(), 1, header.size(), file) != header.size() ||
        std::fflush(file) != 0) {
        std::printf("Precompile record: cannot write %s (%s), off\n", path, std::strerror(errno));
        if (file) {
            std::fclose(file);
        }
        return nullptr;
    }
    std::printf("Precompile record: writing %s\n", path);
    return file;
}
} // namespace

PrecompileRecordPtr MakePrecompileRecord() {
    const char* record_path = PathSetting("GOW3_PRECOMPILE_RECORD");
    const char* list_path = PathSetting("GOW3_PRECOMPILE_LIST");
    const char* boot = std::getenv("GOW3_PRECOMPILE_BOOT");
    const bool boot_on = boot && boot[0] == '1';
    if (list_path && !boot_on) {
        std::printf("Precompile list: %s is read only by the start-up rebuild "
                    "(GOW3_PRECOMPILE_BOOT=1), not used\n",
                    list_path);
    }
    // The start-up rebuild reads the list (vk_precompile_boot.cpp), after this: a record file
    // of the same name would empty it first.
    if (record_path && list_path && boot_on && SameFile(record_path, list_path)) {
        std::printf("Precompile record: %s is the list of the start-up rebuild, not recording\n",
                    record_path);
        record_path = nullptr;
    }
    if (!record_path) {
        return nullptr;
    }
#ifndef __linux__
    std::printf("Precompile record: not supported on this platform\n");
    return nullptr;
#else
    ListAppend kept;
    std::FILE* record_file = OpenRecord(record_path, kept);
    if (!record_file) {
        return nullptr;
    }
    if (!SetUpFaultGuard()) {
        std::fclose(record_file);
        return nullptr;
    }
    static const bool report_at_exit = std::atexit(ReportAtExit) == 0;
    (void)report_at_exit;
    auto* precompile = new PrecompileRecord(record_file, std::move(kept));
    g_active = precompile;
    return PrecompileRecordPtr{precompile};
#endif
}

void PipelineCache::CapturePrecompile() {
    if (precompile->Capturing()) {
        precompile->Capture(*this);
    }
}

void PipelineCache::ReportPrecompile() {
    if (precompile) {
        precompile->Report(false);
    }
}

} // namespace Vulkan
