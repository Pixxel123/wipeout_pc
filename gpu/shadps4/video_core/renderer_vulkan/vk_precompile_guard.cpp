// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cerrno>
#include <cstdio>
#ifdef __linux__
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "common/decoder.h"
#include "core/signals.h"
#include "gow3_tls.h"
#include "video_core/renderer_vulkan/vk_precompile_guard.h"

namespace Vulkan::Precompile {

Reserved g_reserved;

namespace {

int g_read_errno; ///< of the first failed ReadGuest
constinit thread_local ThreadState t_state GOW3_TLS_INITIAL_EXEC{};

#ifdef __linux__
u64 Reserve(u64 size) {
    void* at = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return at == MAP_FAILED ? 0 : u64(at);
}
#else
u64 Reserve(u64) {
    return 0;
}
#endif

/// Access-violation handler (priority 0, before the SRT walker patch). Reads of a watched
/// walker are served here: recorded guest words while recording, else zero (and counted), so
/// the SRT walker patch never rewrites a walker the game still uses. Other faults in our
/// ranges are noted and go on to the recovery point (runtime_fault_recover); the rest pass
/// through untouched (a guest page the page manager resolves is not a stray read).
bool OnFault(void* context, void* fault_address) {
#ifdef __linux__
    auto& t = t_state;
    const u64 address = u64(fault_address);
    if (!t.watching && !g_reserved.InRecord(address) && !g_reserved.InShadow(address)) {
        return false; // the game's own faults
    }
    auto& gregs = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
    const u64 rip = u64(gregs[REG_RIP]);
    ZydisDecodedInstruction inst;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const bool walker_read =
        t.InWalker(rip) &&
        ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
            inst, operands, reinterpret_cast<void*>(rip))) &&
        inst.mnemonic == ZYDIS_MNEMONIC_MOV && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
        (operands[0].reg.value == ZYDIS_REGISTER_RDI ||
         operands[0].reg.value == ZYDIS_REGISTER_R10D);
    const bool ours = g_reserved.InRecord(address) || g_reserved.InShadow(address);
    if (!walker_read) {
        if (ours) {
            t.Note(address); // a descriptor or fetch-shader read: back to the recovery point
        }
        return false;
    }
    const bool is_pointer = operands[0].reg.value == ZYDIS_REGISTER_RDI;
    u64 value;
    if (t.recorder && g_reserved.InRecord(address)) {
        value = t.recorder->Load(u64(gregs[REG_RDI]), address, is_pointer);
    } else {
        // Outside the recorded words (or the record slots in a replay): a zero, or for a
        // pointer the dead page (not 0, whose next read would fault again outside the shadow).
        t.Note(address);
        value = is_pointer ? g_reserved.shadow : 0;
    }
    gregs[is_pointer ? REG_RDI : REG_R10] = greg_t(value);
    gregs[REG_RIP] += inst.length;
    return true;
#else
    return false;
#endif
}

} // namespace

bool SetUpFaultGuard() {
    if (g_reserved.record) {
        return true;
    }
    g_reserved.record = Reserve(RecordSlotSize * NumRecordSlots);
    g_reserved.shadow = Reserve(ShadowArenaSize);
    if (!g_reserved.record || !g_reserved.shadow) {
        std::printf("Precompile: could not reserve address space, off\n");
        g_reserved = {};
        return false;
    }
    (void)Common::Decoder::Instance(); // built now: the fault handler must not allocate
    Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
    return true;
}

#ifdef __linux__
bool Protect(u64 address, size_t size, bool access) {
    return mprotect(reinterpret_cast<void*>(address), size,
                    access ? PROT_READ | PROT_WRITE : PROT_NONE) == 0;
}
void Discard(u64 address, size_t size) {
    madvise(reinterpret_cast<void*>(address), size, MADV_DONTNEED);
    Protect(address, size, false);
}

bool ReadGuest(u64 address, void* out, size_t size) {
    const int saved = errno;
    iovec local{out, size}, remote{reinterpret_cast<void*>(address), size};
    const bool read = process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
    if (!read && !g_read_errno) {
        g_read_errno = errno;
    }
    errno = saved;
    return read;
}
#else
bool Protect(u64, size_t, bool) {
    return false;
}
void Discard(u64, size_t) {}
bool ReadGuest(u64, void*, size_t) {
    return false;
}
#endif

int FirstReadError() {
    return g_read_errno;
}

ThreadState& FaultState() {
    return t_state;
}

std::optional<u64> ShadowArena::Map(const ShadowLayout& layout) {
    const size_t size = layout.Size();
    if (size > MaxShadowBytes || next + size > g_reserved.shadow + ShadowArenaSize ||
        !Protect(next, size, true)) {
        return std::nullopt;
    }
    const u64 address = next;
    next += size;
    for (const size_t guard : layout.GuardPages()) {
        if (!Protect(address + guard, PageSize, false)) {
            Discard(address, size); // without its guard a stray read would go unnoticed
            return std::nullopt;
        }
    }
    return address;
}

void ShadowArena::Unmap(u64 address, const ShadowLayout& layout) {
    Discard(address, layout.Size());
}

const char* FaultPlace(u64 fault) {
    return g_reserved.InShadow(fault)   ? "shadow guard or dead page"
           : g_reserved.InRecord(fault) ? "record slots"
           : fault                      ? "guest or host memory"
                                        : "unknown address";
}

} // namespace Vulkan::Precompile
