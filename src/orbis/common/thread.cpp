// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <fmt/format.h>

#include "orbis/internals/kernel/threads/pthread.h"


#include "orbis/common/util.h"
#include "orbis/logging/log.h"
#include "orbis/common/thread.h"
#include "orbis/common/futex.h"
#include "ntapi.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#if defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#else
#endif
#include <sched.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __FreeBSD__
#define cpu_set_t cpuset_t
#endif

namespace Common {

namespace {
thread_local std::string current_thread_name;
}

#ifdef __APPLE__

void SetCurrentThreadRealtime(const std::chrono::nanoseconds period_ns) {
    // CPU time to grant.
    const std::chrono::nanoseconds computation_ns = period_ns / 2;

    // Determine the timebase for converting time to ticks.
    struct mach_timebase_info timebase{};
    mach_timebase_info(&timebase);
    const auto ticks_per_ns =
        static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);

    const auto period_ticks =
        static_cast<u32>(static_cast<double>(period_ns.count()) * ticks_per_ns);
    const auto computation_ticks =
        static_cast<u32>(static_cast<double>(computation_ns.count()) * ticks_per_ns);

    thread_time_constraint_policy policy = {
        .period = period_ticks,
        .computation = computation_ticks,
        // Should not matter since preemptible is false, but needs to be >= computation regardless.
        .constraint = computation_ticks,
        .preemptible = false,
    };

    int ret = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (ret != KERN_SUCCESS) {
        LOG_ERROR(Common, "Could not set thread to real-time with period {} ns: {}",
                  period_ns.count(), ret);
    }
}

#else

void SetCurrentThreadRealtime(const std::chrono::nanoseconds /*period_ns*/) {
    // Not implemented
}

#endif

#ifdef _WIN32

namespace {

// One high-resolution timer per thread. Setting it restarts it, and an APC ends the alertable wait it
// interrupts, so a sleep nested in the APC can reuse it.
struct SleepTimer {
    HANDLE handle = [] {
        auto timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        return timer ? timer : ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }();
    ~SleepTimer() {
        if (handle) {
            ::CloseHandle(handle);
        }
    }
};

} // namespace

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    auto handle = GetCurrentThread();
    int windows_priority = 0;
    switch (new_priority) {
    case ThreadPriority::Low:
        windows_priority = THREAD_PRIORITY_BELOW_NORMAL;
        break;
    case ThreadPriority::Normal:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    case ThreadPriority::High:
        windows_priority = THREAD_PRIORITY_ABOVE_NORMAL;
        break;
    case ThreadPriority::VeryHigh:
        windows_priority = THREAD_PRIORITY_HIGHEST;
        break;
    case ThreadPriority::Critical:
        windows_priority = THREAD_PRIORITY_TIME_CRITICAL;
        break;
    default:
        windows_priority = THREAD_PRIORITY_NORMAL;
        break;
    }
    SetThreadPriority(handle, windows_priority);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible, const bool spin_short) {
    if (duration <= std::chrono::nanoseconds::zero()) {
        if (remaining) {
            *remaining = std::chrono::nanoseconds::zero();
        }
        return true;
    }

    const auto begin_sleep = std::chrono::steady_clock::now();
    const auto deadline = begin_sleep + duration;

    // Leave a 150us margin to absorb Windows high-resolution waitable timer wakeup jitter,
    // finishing the tail with a low-power pause spin up to the exact deadline.
    static constexpr std::chrono::nanoseconds SpinMargin{150'000};
    if (duration > SpinMargin || !spin_short) {
        const auto timer_ns = (duration > SpinMargin ? duration - SpinMargin : duration).count();
        const auto timer_ticks = timer_ns / 100;
        if (timer_ticks > 0) {
            LARGE_INTEGER interval{
                .QuadPart = -timer_ticks,
            };
            thread_local const SleepTimer timer;
            const DWORD ret =
                timer.handle && ::SetWaitableTimer(timer.handle, &interval, 0, nullptr, nullptr, FALSE)
                    ? ::WaitForSingleObjectEx(timer.handle, INFINITE, interruptible)
                    : WAIT_FAILED;
            if (ret != WAIT_OBJECT_0) {
                if (remaining) {
                    const auto end_sleep = std::chrono::steady_clock::now();
                    const auto sleep_time = end_sleep - begin_sleep;
                    *remaining =
                        duration > sleep_time ? duration - sleep_time : std::chrono::nanoseconds(0);
                }
                return false;
            }
        }
    }

    while (std::chrono::steady_clock::now() < deadline) {
        YieldProcessor();
    }

    if (remaining) {
        *remaining = std::chrono::nanoseconds(0);
    }
    return true;
}

// Game:Main polls the GPU frame counter with short sleeps, which the timer stretches to ~0.5 ms: they end at the GPU
// thread's next guest-memory write instead (within 1 ms, never before the requested time)
#pragma comment(lib, "synchronization.lib")
static std::atomic<u32> gpu_writes{0};

void NotifyGpuWrite() {
    gpu_writes.fetch_add(1, std::memory_order_release);
    ::WakeByAddressAll(&gpu_writes);
}

bool WaitGpuWrite(const std::chrono::nanoseconds duration) {
    if (duration > std::chrono::microseconds(150) || GetCurrentThreadNameView() != "Game:Main") return false;
    const auto deadline = std::chrono::steady_clock::now() + duration;
    u32 seen = gpu_writes.load(std::memory_order_acquire);
    ::WaitOnAddress(&gpu_writes, &seen, sizeof(seen), 1);
    while (std::chrono::steady_clock::now() < deadline) YieldProcessor();
    return true;
}

void InterruptSleep(void* thread) {
    ::QueueUserAPC([](ULONG_PTR) {}, thread, 0);
}

#else

void SetCurrentThreadPriority(ThreadPriority new_priority) {
    const sched_param params{};
    pthread_setschedparam(pthread_self(), new_priority == ThreadPriority::Low ? SCHED_BATCH : SCHED_OTHER, &params);
}

bool AccurateSleep(const std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   const bool interruptible, const bool spin_short) {
    if (duration <= std::chrono::nanoseconds::zero()) {
        if (remaining) *remaining = {};
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + duration;
    if (!interruptible && spin_short && duration <= std::chrono::microseconds(60)) {
        while (std::chrono::steady_clock::now() < deadline) _mm_pause();
        if (remaining) *remaining = {};
        return true;
    }
    timespec request = {
        .tv_sec = duration.count() / 1'000'000'000,
        .tv_nsec = duration.count() % 1'000'000'000,
    };
    timespec remain{};
    int ret;
    while ((ret = nanosleep(&request, &remain)) < 0 && errno == EINTR) {
        if (interruptible) {
            break;
        }
        request = remain;
    }
    if (remaining) {
        *remaining = ret == 0 ? std::chrono::nanoseconds{} : std::chrono::nanoseconds(remain.tv_sec * 1'000'000'000 + remain.tv_nsec);
    }
    return ret == 0;
}

static std::atomic<u32> gpu_writes{0};

void NotifyGpuWrite() {
    gpu_writes.fetch_add(1, std::memory_order_release);
    FutexWakeAll(&gpu_writes);
}

bool WaitGpuWrite(const std::chrono::nanoseconds duration) {
    if (duration > std::chrono::microseconds(150) || GetCurrentThreadNameView() != "Game:Main") return false;
    const auto deadline = std::chrono::steady_clock::now() + duration;
    FutexWait(gpu_writes, gpu_writes.load(std::memory_order_acquire), std::chrono::milliseconds(1));
    while (std::chrono::steady_clock::now() < deadline) _mm_pause();
    return true;
}

#endif

bool SetCurrentThreadBackground(bool enabled) {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), enabled ? THREAD_MODE_BACKGROUND_BEGIN
                                                        : THREAD_MODE_BACKGROUND_END) != 0;
#else
    return false;
#endif
}

#ifdef _WIN32

// Sets the debugger-visible name of the current thread.
void SetCurrentThreadName(const char* name) {
    current_thread_name = name;
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    SetThreadDescription(GetCurrentThread(), UTF8ToUTF16W(name).data());
}

void SetThreadName(void* thread, const char* name) {
    SetThreadDescription(thread, UTF8ToUTF16W(name).data());
}

#else // !_WIN32, so must be POSIX threads

// MinGW with the POSIX threading model does not support pthread_setname_np
#if !defined(_WIN32) || defined(_MSC_VER)
void SetCurrentThreadName(const char* name) {
    current_thread_name = name;
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
#ifdef __APPLE__
    pthread_setname_np(name);
#elif defined(__Bitrig__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name);
#elif defined(__NetBSD__)
    pthread_setname_np(pthread_self(), "%s", (void*)name);
#elif defined(__linux__)
    // Linux limits thread names to 15 characters and will outright reject any
    // attempt to set a longer name with ERANGE.
    std::string truncated(name, std::min(strlen(name), static_cast<std::size_t>(15)));
    if (int e = pthread_setname_np(pthread_self(), truncated.c_str())) {
        errno = e;
        LOG_ERROR(Common, "Failed to set thread name to '{}': {}", truncated, GetLastErrorMsg());
    }
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

void SetThreadName(void* thread, const char* name) {
    // TODO
}
#endif

#if defined(_WIN32)
void SetCurrentThreadName(const char* name) {
    current_thread_name = name;
    if (Libraries::Kernel::g_curthread) {
        Libraries::Kernel::g_curthread->name = name;
    }
    // Do Nothing on MinGW
}

void SetThreadName(void* thread, const char* name) {
    // Do Nothing on MinGW
}
#endif

#endif

AccurateTimer::AccurateTimer(const std::chrono::nanoseconds target_interval,
                             const u32 max_catch_up_intervals,
                             const MissedTickPolicy missed_tick_policy)
    : target_interval{target_interval}, max_timing_debt{target_interval * max_catch_up_intervals},
      missed_tick_policy{missed_tick_policy} {}

void AccurateTimer::Start() {
    const auto begin_sleep = std::chrono::steady_clock::now();
    if (total_wait.count() > 0) {
        AccurateSleep(total_wait, nullptr, false);
    }
    start_time = std::chrono::steady_clock::now();
    total_wait -= std::chrono::duration_cast<std::chrono::nanoseconds>(start_time - begin_sleep);
}

void AccurateTimer::End() {
    const auto now = std::chrono::steady_clock::now();
    total_wait +=
        target_interval - std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_time);

    total_wait = Detail::NormalizePeriodicWait(total_wait, target_interval, max_timing_debt,
                                               missed_tick_policy);
}

void AccurateTimer::Reset() {
    total_wait = target_interval;
}

std::string_view GetCurrentThreadNameView() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        return g_curthread->name;
    }
    if (!current_thread_name.empty()) {
        return current_thread_name;
    }
#ifdef _WIN32
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name != nullptr) {
        current_thread_name = Common::UTF16ToUTF8(name);
        LocalFree(name);
    }
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0) {
        current_thread_name = name;
    }
#endif
    if (current_thread_name.empty()) {
        current_thread_name = "<unknown name>";
    }
    return current_thread_name;
}

std::string GetCurrentThreadName() {
    using namespace Libraries::Kernel;
    if (g_curthread && !g_curthread->name.empty()) {
        return g_curthread->name;
    }
#ifdef _WIN32
    PWSTR name{};
    if (FAILED(GetThreadDescription(GetCurrentThread(), &name)) || name == nullptr) {
        return "<unknown name>";
    }
    const auto result = Common::UTF16ToUTF8(name);
    LocalFree(name);
    return result;
#else
    char name[256];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return std::string{name};
#endif
}

} // namespace Common
