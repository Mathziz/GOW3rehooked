// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unordered_map>
#include "orbis/common/util.h"
#include "orbis/common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Kernel {

class EqueueInternal;
struct EqueueEvent;

struct OrbisKernelBintime {
    s64 sec;
    s64 frac;
};

using OrbisKernelUseconds = u32;
using OrbisKernelEqueue = s64;

struct OrbisKernelEvent {
    enum Filter : s16 {
        None = 0,
        Read = -1,
        Write = -2,
        Aio = -3,
        Vnode = -4,
        Proc = -5,
        Signal = -6,
        Timer = -7,
        Fs = -9,
        Lio = -10,
        User = -11,
        Polling = -12,
        VideoOut = -13,
        GraphicsCore = -14,
        HrTimer = -15,
    };

    enum Flags : u16 {
        Add = 1u,
        Delete = 2u,
        Enable = 4u,
        Disable = 8u,
        OneShot = 0x10u,
        Clear = 0x20u,
        Receipt = 0x40u,
        Dispatch = 0x80u,
        Flag1 = 0x2000u,
        System = 0xf000u,
    };

    u64 ident = 0;                /* identifier for this event */
    Filter filter = Filter::None; /* filter for event */
    u16 flags = 0;
    u32 fflags = 0;
    u64 data = 0;
    void* udata = nullptr; /* opaque user data identifier */
};

struct OrbisVideoOutEventHint {
    u64 event_id : 8;
    u64 video_id : 8;
    u64 flip_arg : 48;
};

struct OrbisVideoOutEventData {
    u64 time : 12;
    u64 count : 4;
    u64 flip_arg : 48;
};

struct KernelTimer {
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    struct WaitToken {
        std::atomic<bool> cancelled{false};
    };

    explicit KernelTimer(Duration interval) : m_expiry(Clock::now() + interval) {}

    ~KernelTimer() {
        Cancel();
    }

    KernelTimer(const KernelTimer&) = delete;
    KernelTimer& operator=(const KernelTimer&) = delete;

    [[nodiscard]] TimePoint Expiry() const noexcept {
        return m_expiry;
    }

    void ExpiresAt(TimePoint expiry) noexcept {
        Cancel();
        m_expiry = expiry;
    }

    void Cancel() noexcept {
        if (m_token) {
            m_token->cancelled.store(true, std::memory_order_release);
            m_token.reset();
        }
    }

    void AsyncWait(std::move_only_function<void()> callback);

private:
    TimePoint m_expiry{};
    std::shared_ptr<WaitToken> m_token;
};

struct EqueueEvent {
    OrbisKernelEvent event;
    void* data = nullptr;
    std::chrono::steady_clock::time_point time_added;
    std::chrono::nanoseconds timer_interval;
    std::unique_ptr<KernelTimer> timer;

    void Clear() {
        is_triggered = false;
        event.fflags = 0;
        event.data = 0;
    }

    void Trigger(void* data) {
        is_triggered = true;
        event.data = reinterpret_cast<uintptr_t>(data);
    }

    void TriggerUser(void* data) {
        is_triggered = true;
        event.udata = data;
    }

    void TriggerTimer() {
        is_triggered = true;
        event.data++;
    }

    void TriggerDisplay(void* data) {
        is_triggered = true;
        if (data != nullptr) {
            auto event_data = std::bit_cast<OrbisVideoOutEventData>(event.data);
            auto event_hint_raw = reinterpret_cast<u64>(data);
            auto event_hint = static_cast<OrbisVideoOutEventHint>(event_hint_raw);
            if (event_hint.event_id == event.ident && event.ident != 0xfe) {
                auto time = Common::FencedRDTSC();
                auto counter = event_data.count;
                if (counter != 0xf) {
                    counter++;
                }
                event.data =
                    (time & 0xfff) | (counter << 0xc) | (event_hint_raw & 0xffffffffffff0000);
            }
        }
    }

    bool IsTriggered() const {
        return is_triggered;
    }

    bool operator==(const EqueueEvent& ev) const {
        return ev.event.ident == event.ident && ev.event.filter == event.filter;
    }

private:
    bool is_triggered = false;
};

class EqueueInternal {
    struct SmallTimer {
        OrbisKernelEvent event;
        std::chrono::steady_clock::time_point added;
        std::chrono::nanoseconds interval;
    };

public:
    explicit EqueueInternal(OrbisKernelEqueue handle, std::string_view name)
        : m_handle(handle), m_name(name) {}

    std::string_view GetName() const {
        return m_name;
    }

    bool AddEvent(EqueueEvent& event);
    bool ScheduleEvent(u64 id, s16 filter,
                       void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&));
    bool RemoveEvent(u64 id, s16 filter);
    int WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo);
    bool TriggerEvent(u64 ident, s16 filter, void* trigger_data);
    int GetTriggeredEvents(OrbisKernelEvent* ev, int num);

    bool AddSmallTimer(EqueueEvent& event);
    bool HasSmallTimer() {
        std::scoped_lock lock{m_mutex};
        return !m_small_timers.empty();
    }
    bool RemoveSmallTimer(u64 id) {
        if (HasSmallTimer()) {
            std::scoped_lock lock{m_mutex};
            return m_small_timers.erase(id) > 0;
        }
        return false;
    }

    int WaitForSmallTimer(OrbisKernelEvent* ev, int num, u32 micros);

    bool EventExists(u64 id, s16 filter);

private:
    OrbisKernelEqueue m_handle;
    std::string m_name;
    std::mutex m_mutex;
    std::vector<EqueueEvent> m_events;
    std::condition_variable m_cond;
    std::unordered_map<u64, SmallTimer> m_small_timers;
};

EqueueInternal* GetEqueue(OrbisKernelEqueue eq);
u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev);

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Kernel
