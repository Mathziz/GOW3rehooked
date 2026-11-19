// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifdef __linux__
#include <atomic>
#include <chrono>
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Common {
// Linux futexes compare one aligned 32-bit word. On x86-64 the low word is first.
template <typename T>
void FutexWait(std::atomic<T>& value, T seen, std::chrono::nanoseconds timeout = std::chrono::nanoseconds::max()) {
    if (timeout <= std::chrono::nanoseconds::zero()) return;
    const timespec ts{timeout.count() / 1'000'000'000, timeout.count() % 1'000'000'000};
    syscall(SYS_futex, &value, FUTEX_WAIT_PRIVATE, static_cast<unsigned>(seen),
            timeout == std::chrono::nanoseconds::max() ? nullptr : &ts, nullptr, 0);
}

template <typename T>
void FutexWakeAll(std::atomic<T>* address) {
    syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}
} // namespace Common
#endif
