// gow3: fault-handler registry. The C loader's SIGSEGV handler calls
// gow3gpu_handle_fault() first; registered handlers run in priority order.
#pragma once
#include <functional>
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
        access_violation_handlers.emplace(handler, priority);
    }
    void RegisterIllegalInstructionHandler(const IllegalInstructionHandler& handler, u32 priority) {
        illegal_instruction_handlers.emplace(handler, priority);
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
    // gow3: different handlers may share a priority (GOW3_PRECOMPILE_REPLAY's reads run at 0
    // with the page manager, before the SRT walker patch at 1); they cover different addresses.
    template <typename T>
    struct HandlerEntry {
        T handler;
        u32 priority;
        bool operator<(const HandlerEntry& right) const {
            if (priority != right.priority) return priority < right.priority;
            return std::less<T>{}(handler, right.handler);
        }
    };
    std::set<HandlerEntry<AccessViolationHandler>> access_violation_handlers;
    std::set<HandlerEntry<IllegalInstructionHandler>> illegal_instruction_handlers;
};
using Signals = Common::Singleton<SignalDispatch>;
} // namespace Core
