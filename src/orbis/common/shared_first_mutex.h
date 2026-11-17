// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>

#include "orbis/common/types.h"

#ifdef _WIN64
#include <windows.h>
#else
#include <condition_variable>
#include <mutex>
#endif

namespace Common {

// Like std::shared_mutex, but reader has priority over writer.
class SharedFirstMutex {
public:
    void lock() {
#ifdef _WIN64
        if (try_lock()) {
            return;
        }
        waiters.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            u32 s = 0;
            if (state.compare_exchange_strong(s, WriterBit, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                waiters.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
            WaitOnAddress(&state, &s, sizeof(state), INFINITE);
        }
#else
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !writer_active && readers == 0; });
        writer_active = true;
#endif
    }

    bool try_lock() {
#ifdef _WIN64
        u32 expected = 0;
        return state.compare_exchange_strong(expected, WriterBit, std::memory_order_acquire,
                                             std::memory_order_relaxed);
#else
        std::lock_guard<std::mutex> lock(mtx);
        if (writer_active || readers > 0) {
            return false;
        }
        writer_active = true;
        return true;
#endif
    }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
#ifdef _WIN64
        if (try_lock()) {
            return true;
        }
        waiters.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            u32 s = 0;
            if (state.compare_exchange_strong(s, WriterBit, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                waiters.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
            const auto now = Clock::now();
            if (now >= abs_time) {
                waiters.fetch_sub(1, std::memory_order_relaxed);
                return try_lock();
            }
            const auto remaining = abs_time - now;
            if (remaining < std::chrono::milliseconds(1)) {
                waiters.fetch_sub(1, std::memory_order_relaxed);
                while (Clock::now() < abs_time) {
                    YieldProcessor();
                    if (state.load(std::memory_order_relaxed) == 0 && try_lock()) {
                        return true;
                    }
                }
                return try_lock();
            }
            const auto sleep_ms = std::chrono::floor<std::chrono::milliseconds>(remaining);
            constexpr auto MaxFiniteWait = static_cast<s64>(INFINITE) - 1;
            const DWORD timeout_ms =
                static_cast<DWORD>(std::min<s64>(sleep_ms.count(), MaxFiniteWait));
            WaitOnAddress(&state, &s, sizeof(state), timeout_ms);
        }
#else
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_until(lock, abs_time, [this]() { return !writer_active && readers == 0; })) {
            return false;
        }
        writer_active = true;
        return true;
#endif
    }

    void unlock() {
#ifdef _WIN64
        state.store(0, std::memory_order_seq_cst);
        if (waiters.load(std::memory_order_seq_cst) != 0) {
            WakeByAddressAll(&state);
        }
#else
        std::lock_guard<std::mutex> lock(mtx);
        writer_active = false;
        cv.notify_all();
#endif
    }

    void lock_shared() {
#ifdef _WIN64
        if (try_lock_shared()) {
            return;
        }
        waiters.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            u32 s = state.load(std::memory_order_relaxed);
            while ((s & WriterBit) == 0) {
                if (state.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    waiters.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }
            }
            WaitOnAddress(&state, &s, sizeof(state), INFINITE);
        }
#else
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]() { return !writer_active; });
        ++readers;
#endif
    }

    bool try_lock_shared() {
#ifdef _WIN64
        u32 s = state.load(std::memory_order_relaxed);
        while ((s & WriterBit) == 0) {
            if (state.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
#else
        std::lock_guard<std::mutex> lock(mtx);
        if (writer_active) {
            return false;
        }
        ++readers;
        return true;
#endif
    }

    template <typename Clock, typename Duration>
    bool try_lock_shared_until(const std::chrono::time_point<Clock, Duration>& abs_time) {
#ifdef _WIN64
        if (try_lock_shared()) {
            return true;
        }
        waiters.fetch_add(1, std::memory_order_seq_cst);
        for (;;) {
            u32 s = state.load(std::memory_order_relaxed);
            while ((s & WriterBit) == 0) {
                if (state.compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                    waiters.fetch_sub(1, std::memory_order_relaxed);
                    return true;
                }
            }
            const auto now = Clock::now();
            if (now >= abs_time) {
                waiters.fetch_sub(1, std::memory_order_relaxed);
                return try_lock_shared();
            }
            const auto remaining = abs_time - now;
            if (remaining < std::chrono::milliseconds(1)) {
                waiters.fetch_sub(1, std::memory_order_relaxed);
                while (Clock::now() < abs_time) {
                    YieldProcessor();
                    if ((state.load(std::memory_order_relaxed) & WriterBit) == 0 &&
                        try_lock_shared()) {
                        return true;
                    }
                }
                return try_lock_shared();
            }
            const auto sleep_ms = std::chrono::floor<std::chrono::milliseconds>(remaining);
            constexpr auto MaxFiniteWait = static_cast<s64>(INFINITE) - 1;
            const DWORD timeout_ms =
                static_cast<DWORD>(std::min<s64>(sleep_ms.count(), MaxFiniteWait));
            WaitOnAddress(&state, &s, sizeof(state), timeout_ms);
        }
#else
        std::unique_lock<std::mutex> lock(mtx);
        if (!cv.wait_until(lock, abs_time, [this]() { return !writer_active; })) {
            return false;
        }
        ++readers;
        return true;
#endif
    }

    void unlock_shared() {
#ifdef _WIN64
        if (state.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            if (waiters.load(std::memory_order_seq_cst) != 0) {
                WakeByAddressAll(&state);
            }
        }
#else
        std::lock_guard<std::mutex> lock(mtx);
        if (--readers == 0) {
            cv.notify_all();
        }
#endif
    }

private:
#ifdef _WIN64
    static constexpr u32 WriterBit = 0x80000000u;
    std::atomic<u32> state{0};
    std::atomic<u32> waiters{0};
#else
    std::mutex mtx;
    std::condition_variable cv;
    int readers = 0;
    bool writer_active = false;
#endif
};

} // namespace Common
