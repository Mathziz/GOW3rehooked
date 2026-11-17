// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

#include "orbis/common/types.h"

namespace Common {

enum class MissedTickPolicy : u32 {
    CatchUp,
    SkipMissed,
};

namespace Detail {

[[nodiscard]] constexpr std::chrono::nanoseconds NormalizePeriodicWait(
    std::chrono::nanoseconds wait, const std::chrono::nanoseconds interval,
    const std::chrono::nanoseconds max_timing_debt, const MissedTickPolicy policy) {
    if (policy == MissedTickPolicy::CatchUp) {
        return std::clamp(wait, -max_timing_debt, interval);
    }

    // A periodic notification is an edge, not replayable work. Once its deadline is missed,
    // schedule a fresh full interval instead of preserving phase with a shortened catch-up wait.
    return wait <= std::chrono::nanoseconds::zero() ? interval : std::min(wait, interval);
}

} // namespace Detail

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

void SetCurrentThreadRealtime(std::chrono::nanoseconds period_ns);

void SetCurrentThreadPriority(ThreadPriority new_priority);

bool SetCurrentThreadBackground(bool enabled);

void SetCurrentThreadName(const char* name);

void SetThreadName(void* thread, const char* name);

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible, bool spin_short = true);

// Ends Game:Main's short guest sleeps at the GPU thread's next guest-memory write (NotifyGpuWrite); false if not one
bool WaitGpuWrite(std::chrono::nanoseconds duration);
void NotifyGpuWrite();
// Ends an interruptible AccurateSleep of the thread with that native handle (now or at its next one)
void InterruptSleep(void* thread);

class AccurateTimer {
    std::chrono::nanoseconds target_interval{};
    std::chrono::nanoseconds max_timing_debt{};
    std::chrono::nanoseconds total_wait{};
    MissedTickPolicy missed_tick_policy{};

    std::chrono::steady_clock::time_point start_time;

public:
    explicit AccurateTimer(std::chrono::nanoseconds target_interval, u32 max_catch_up_intervals = 2,
                           MissedTickPolicy missed_tick_policy = MissedTickPolicy::CatchUp);

    void Start();

    void End();

    /// Drops accumulated timing debt after an external clock discontinuity.
    void Reset();

    std::chrono::nanoseconds GetTotalWait() const {
        return total_wait;
    }
};

std::string GetCurrentThreadName();
std::string_view GetCurrentThreadNameView();

} // namespace Common
