// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "mutex.h"

namespace Libraries::Kernel {

TimedMutex::TimedMutex() = default;

void TimedMutex::lock() {
#ifdef _WIN64
    u32 c = 0;
    if (state.compare_exchange_strong(c, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
    }
    if (c != 2) {
        c = state.exchange(2, std::memory_order_acquire);
    }
    while (c != 0) {
        u32 expected = 2;
        WaitOnAddress(&state, &expected, sizeof(state), INFINITE);
        c = state.exchange(2, std::memory_order_acquire);
    }
#else
    mtx.lock();
#endif
}

bool TimedMutex::try_lock() {
#ifdef _WIN64
    u32 expected = 0;
    return state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                         std::memory_order_relaxed);
#else
    return mtx.try_lock();
#endif
}

void TimedMutex::unlock() {
#ifdef _WIN64
    if (state.exchange(0, std::memory_order_release) == 2) {
        WakeByAddressSingle(&state);
    }
#else
    mtx.unlock();
#endif
}

} // namespace Libraries::Kernel
