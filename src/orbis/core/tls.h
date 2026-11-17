// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include "orbis/common/types.h"
#ifdef _WIN32
#include <malloc.h>
#endif

namespace Xbyak {
class CodeGenerator;
}

namespace Core {

union DtvEntry {
    std::size_t counter;
    u8* pointer;
};

struct Tcb {
    Tcb* tcb_self;
    DtvEntry* tcb_dtv;
    void* tcb_thread;
    void* tcb_spare[2];
    u64 tcb_canary;
    void* tcb_fiber;
};

#ifdef _WIN32
/// Gets the thread local storage key for the TCB block.
u32 GetTcbKey();
#endif

/// Sets the data pointer to the TCB block.
void SetTcbBase(void* image_address);

/// Retrieves Tcb structure for the calling thread.
Tcb* GetTcbBase();

/// Makes sure TLS is initialized for the thread before entering guest.
void InitializeTLS();

/// Debug: host calls of threads that set this (the game's submitting thread) are timed and report long blocking.
inline thread_local bool timed_host_calls{};
/// Debug: the time this thread spent inside outermost host calls since the last Take, and its longest call.
struct HostCallTotals {
    s64 ns, longest_ns;
    const char* longest;
    u32 depth;
};
inline thread_local HostCallTotals host_call_totals{};
inline HostCallTotals TakeHostCallTotals() {
    const HostCallTotals t = host_call_totals;
    host_call_totals = {.depth = t.depth};
    return t;
}
struct HostCallTimer {
    explicit HostCallTimer(const char* name);
    ~HostCallTimer();
    const char* name;
    s64 start;
};

template <auto f>
struct HostCallWrapperImpl;

template <class ReturnType, class... Args, PS4_SYSV_ABI ReturnType (*func)(Args...)>
struct HostCallWrapperImpl<func> {
    // Kept out of line so the common path stays a flag test and a tail call, without the timer's frame and unwind state.
    [[clang::noinline, gnu::cold]] static ReturnType PS4_SYSV_ABI Timed(Args... args) {
        const HostCallTimer timer(__FUNCSIG__);
        return func(args...);
    }
    static ReturnType PS4_SYSV_ABI wrap(Args... args) {
        if (timed_host_calls) [[unlikely]] return Timed(args...);
        return func(args...);
    }
};

#define HOST_CALL(func) (Core::HostCallWrapperImpl<func>::wrap)

} // namespace Core
