// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>

#include "orbis/common/types.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <mutex>
#endif

namespace Libraries::Kernel {

class TimedMutex {
public:
    TimedMutex();
    ~TimedMutex() = default;

    void lock();
    bool try_lock();

    void unlock();

    template <class Rep, class Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period>& rel_time) {
#ifdef _WIN64
        if (try_lock()) {
            return true;
        }
        constexpr auto zero = std::chrono::duration<Rep, Period>::zero();
        if (rel_time <= zero) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto clock_duration =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(rel_time);
        constexpr auto max = std::chrono::steady_clock::time_point::max();
        const auto abs_time = (now < max - clock_duration) ? (now + clock_duration) : max;
        return try_lock_until(abs_time);
#else
        return mtx.try_lock_for(rel_time);
#endif
    }

    template <class Clock, class Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
#ifdef _WIN64
        if (try_lock()) {
            return true;
        }

        static constexpr auto SpinThreshold = std::chrono::microseconds(200);
        bool waited = false;

        for (;;) {
            u32 c = state.load(std::memory_order_relaxed);
            if (c == 0) {
                if (state.compare_exchange_weak(c, 2, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    return true;
                }
                continue;
            }

            const auto now = Clock::now();
            if (now >= abs_time) {
                if (waited) {
                    return state.exchange(2, std::memory_order_acquire) == 0;
                }
                u32 expected = 0;
                return state.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                                     std::memory_order_relaxed);
            }

            const auto remaining = abs_time - now;
            if (remaining < std::chrono::milliseconds(1)) {
                for (u32 spin = 0;; ++spin) {
                    YieldProcessor();
                    if (state.load(std::memory_order_relaxed) == 0) {
                        u32 expected = 0;
                        if (state.compare_exchange_weak(expected, 2, std::memory_order_acquire,
                                                        std::memory_order_relaxed)) {
                            return true;
                        }
                    }
                    const auto now_spin = Clock::now();
                    if (now_spin >= abs_time) {
                        if (waited) {
                            return state.exchange(2, std::memory_order_acquire) == 0;
                        }
                        u32 expected = 0;
                        return state.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                                             std::memory_order_relaxed);
                    }
                    if ((abs_time - now_spin) > SpinThreshold && (spin & 31u) == 31u) {
                        SwitchToThread();
                    }
                }
            }

            if (c != 2 && state.exchange(2, std::memory_order_acquire) == 0) {
                return true;
            }
            waited = true;

            const auto sleep_ms = std::chrono::floor<std::chrono::milliseconds>(remaining);
            constexpr auto MaxFiniteWait = static_cast<s64>(INFINITE) - 1;
            const DWORD timeout_ms =
                static_cast<DWORD>(std::min<s64>(sleep_ms.count(), MaxFiniteWait));
            u32 expected = 2;
            WaitOnAddress(&state, &expected, sizeof(state), timeout_ms);
        }
#else
        return mtx.try_lock_until(abs_time);
#endif
    }

private:
#ifdef _WIN64
    // 0 = unlocked, 1 = locked (no waiters), 2 = locked (maybe waiters)
    std::atomic<u32> state{0};
#else
    std::timed_mutex mtx;
#endif
};

} // namespace Libraries::Kernel
