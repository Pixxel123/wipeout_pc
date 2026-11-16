// gow3: fault-handler registry. The C loader's SIGSEGV handler calls
// gow3gpu_handle_fault() first; registered handlers run in priority order.
#pragma once
#include <set>
#include <signal.h>
#include "common/singleton.h"
#include "common/types.h"

namespace Core {
using AccessViolationHandler = bool (*)(void* context, void* fault_address);
using IllegalInstructionHandler = bool (*)(void* context);

class SignalDispatch {
public:
    void RemoveHandlers() { access_violation_handlers.clear(); illegal_instruction_handlers.clear(); }
    void RegisterAccessViolationHandler(const AccessViolationHandler& handler, u32 priority) {
        Add(access_violation_handlers, handler, priority);
    }
    void RegisterIllegalInstructionHandler(const IllegalInstructionHandler& handler, u32 priority) {
        Add(illegal_instruction_handlers, handler, priority);
    }
    bool DispatchAccessViolation(void* context, void* fault_address) const {
        for (const auto& entry : access_violation_handlers)
            if (entry.handler(context, fault_address)) return true;
        return false;
    }
    bool DispatchIllegalInstruction(void* context) const {
        for (const auto& entry : illegal_instruction_handlers)
            if (entry.handler(context)) return true;
        return false;
    }

private:
    // gow3: different handlers may share a priority, in registration order
    // (the GOW3_PRECOMPILE_* handler runs at 0 after the page manager, before the SRT walker
    // patch at 1). A handler is registered once.
    template <typename T>
    struct HandlerEntry {
        T handler;
        u32 priority;
        u64 sequence;
        bool operator<(const HandlerEntry& right) const {
            if (priority != right.priority) return priority < right.priority;
            return sequence < right.sequence;
        }
    };
    template <typename T>
    void Add(std::set<HandlerEntry<T>>& handlers, T handler, u32 priority) {
        for (const auto& entry : handlers)
            if (entry.handler == handler) return;
        handlers.insert({handler, priority, next_sequence++});
    }
    u64 next_sequence = 0;
    std::set<HandlerEntry<AccessViolationHandler>> access_violation_handlers;
    std::set<HandlerEntry<IllegalInstructionHandler>> illegal_instruction_handlers;
};
using Signals = Common::Singleton<SignalDispatch>;
} // namespace Core
