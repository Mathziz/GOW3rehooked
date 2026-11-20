// SPDX-License-Identifier: GPL-2.0-or-later
#include "present/presentation_timing.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <immintrin.h>
#include <mutex>
#include "orbis/common/thread.h"
#include "main/host.h"

namespace Gow3::Presenter {
s64 PresentationTiming::Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
PresentationTiming::PresentationTiming(VkDevice d, SDL_Window* window, s64 period, bool supported, bool reflex)
    : device(d), period(period), display(period), pacer(period) {
    UpdateDisplay(window);
    if (supported) wait_present = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device, "vkWaitForPresentKHR"));
    if (reflex) {
        set_sleep_mode = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(vkGetDeviceProcAddr(device, "vkSetLatencySleepModeNV"));
        latency_sleep = reinterpret_cast<PFN_vkLatencySleepNV>(vkGetDeviceProcAddr(device, "vkLatencySleepNV"));
        set_marker = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(vkGetDeviceProcAddr(device, "vkSetLatencyMarkerNV"));
        const VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        const VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        if (!set_sleep_mode || !latency_sleep || !set_marker || vkCreateSemaphore(device, &info, nullptr, &sleep_semaphore) != VK_SUCCESS)
            Log("NVIDIA Reflex: initialization failed; using normal presentation pacing");
    }
    if (wait_present) feedback = std::jthread([this](const std::stop_token& token) { Feedback(token); });
}
void PresentationTiming::UpdateDisplay(SDL_Window* window) {
    const auto* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    nominal.store(mode && mode->refresh_rate > 0 ? s64(1'000'000'000. / mode->refresh_rate) : 0, std::memory_order_release);
}
PresentationTiming::~PresentationTiming() {
    feedback.request_stop(); cv.notify_all();
    if (feedback.joinable()) feedback.join();
    if (sleep_semaphore) vkDestroySemaphore(device, sleep_semaphore, nullptr);
}
void PresentationTiming::Attach(VkSwapchainKHR handle) {
    std::unique_lock lock(handle_mutex);
    swapchain = handle; ++generation; pacer.Reset(); locked = false; correction = 0;
    if (sleep_semaphore) {
        const VkLatencySleepModeInfoNV mode{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
            .lowLatencyMode = VK_TRUE, .lowLatencyBoost = VK_TRUE};
        const bool enabled = set_sleep_mode(device, swapchain, &mode) == VK_SUCCESS;
        reflex_active.store(enabled, std::memory_order_release);
        Log(enabled ? "NVIDIA Reflex enabled" : "NVIDIA Reflex unavailable on swapchain; using normal presentation pacing");
    }
}
void PresentationTiming::Detach() {
    std::unique_lock lock(handle_mutex);
    reflex_active.store(false, std::memory_order_release);
    frame_id.store(0, std::memory_order_release);
    swapchain = VK_NULL_HANDLE; ++generation; locked = false; correction = 0;
}
void PresentationTiming::Marker(u64 id, VkLatencyMarkerNV marker) {
    if (!id || !ReflexActive()) return;
    std::scoped_lock guard(handle_mutex);
    if (!swapchain || !ReflexActive()) return;
    const VkSetLatencyMarkerInfoNV info{.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = marker};
    set_marker(device, swapchain, &info);
}
void PresentationTiming::DisableReflex() {
    std::scoped_lock guard(handle_mutex);
    if (!reflex_active.exchange(false, std::memory_order_acq_rel)) return;
    frame_id.store(0, std::memory_order_release);
    if (swapchain) {
        const VkLatencySleepModeInfoNV mode{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
        set_sleep_mode(device, swapchain, &mode);
    }
    Log("NVIDIA Reflex sleep failed; using normal presentation pacing");
}
void PresentationTiming::BeginInput() {
    if (!ReflexActive()) return;
    std::scoped_lock input_guard(input_mutex);
    const u64 present = present_count.load(std::memory_order_acquire);
    if (present == slept_present) return;
    u64 epoch, value;
    {
        std::scoped_lock guard(handle_mutex);
        if (!swapchain || !ReflexActive()) return;
        epoch = generation;
        value = ++sleep_value;
        const VkLatencySleepInfoNV info{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, .signalSemaphore = sleep_semaphore, .value = value};
        if (latency_sleep(device, swapchain, &info) != VK_SUCCESS) value = 0;
        slept_present = present;
    }
    if (!value) return DisableReflex();
    const VkSemaphoreWaitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &sleep_semaphore, .pValues = &value};
    if (vkWaitSemaphores(device, &wait, 100'000'000) != VK_SUCCESS) {
        return DisableReflex();
    }
    std::scoped_lock guard(handle_mutex);
    if (!swapchain || !ReflexActive() || generation != epoch) return;
    const u64 id = ++next_frame;
    const VkSetLatencyMarkerInfoNV start{.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = VK_LATENCY_MARKER_SIMULATION_START_NV};
    const VkSetLatencyMarkerInfoNV input{.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = VK_LATENCY_MARKER_INPUT_SAMPLE_NV};
    set_marker(device, swapchain, &start);
    set_marker(device, swapchain, &input);
    frame_id.store(id, std::memory_order_release);
}
u64 PresentationTiming::EndSimulation() {
    const u64 id = frame_id.exchange(0, std::memory_order_acq_rel);
    Marker(id, VK_LATENCY_MARKER_SIMULATION_END_NV);
    return id;
}
VkResult PresentationTiming::Acquire(VkSemaphore semaphore, VkFence available, u32* index) {
    std::scoped_lock guard(handle_mutex);
    return vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, semaphore, available, index);
}
VkResult PresentationTiming::Present(VkQueue queue, const VkPresentInfoKHR& info, std::mutex& queue_mutex) {
    std::scoped_lock guard(handle_mutex);
    std::scoped_lock queue_guard(queue_mutex);
    return vkQueuePresentKHR(queue, &info);
}
VkResult PresentationTiming::SubmitAndPresent(VkQueue queue, const VkSubmitInfo& submit, VkFence fence, const VkPresentInfoKHR& present,
                                               u64 id, s64 latch, std::mutex& queue_mutex) {
    VkResult result;
    { std::scoped_lock guard(queue_mutex); result = vkQueueSubmit(queue, 1, &submit, fence); }
    if (result != VK_SUCCESS) return result;
    const s64 ready = ReflexActive() ? 0 : Pace(fence, latch);
    Marker(id, VK_LATENCY_MARKER_PRESENT_START_NV);
    result = Present(queue, present, queue_mutex);
    Marker(id, VK_LATENCY_MARKER_PRESENT_END_NV);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) present_count.fetch_add(1, std::memory_order_release);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) Presented(id, latch, ready);
    return result;
}
s64 PresentationTiming::Pace(VkFence fence, s64 latch) {
    if (!latch) { pacer.Reset(); return 0; }
    if (locked.load(std::memory_order_acquire)) {
        pacer.Reset();
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, u64(period)) == VK_SUCCESS) return Now();
        locked = false; return 0;
    }
    const auto plan = pacer.Plan(latch);
    if (plan.free) return 0;
    const auto timeout = u64(std::max<s64>(plan.wait_limit_ns - Now(), 0));
    const auto result = vkWaitForFences(device, 1, &fence, VK_TRUE, timeout);
    const s64 ready = Now();
    pacer.AddSample(result == VK_SUCCESS ? ready : 0);
    if (result != VK_SUCCESS) return 0;
    constexpr s64 spin = 400'000;
    if (plan.present_ns - ready > spin) Common::AccurateSleep(std::chrono::nanoseconds(plan.present_ns - ready - spin), nullptr, false);
    while (Now() < plan.present_ns) _mm_pause();
    return ready;
}
void PresentationTiming::Presented(u64 id, s64 latch, s64 ready) {
    if (!wait_present || !id || !latch || !ready || ReflexActive()) return;
    std::scoped_lock guard(handle_mutex);
    std::scoped_lock lock(mutex);
    if (samples.size() == 128) samples.pop_front();
    samples.push_back({generation, id, latch, ready}); cv.notify_one();
}
void PresentationTiming::Feedback(const std::stop_token& token) {
    Common::SetCurrentThreadName("GOW3 display timing");
    u64 epoch{}; s64 last{};
    while (!token.stop_requested()) {
        Sample sample;
        { std::unique_lock lock(mutex); if (!cv.wait(lock, token, [&] { return !samples.empty(); })) return;
          sample = samples.front(); samples.pop_front(); }
        const s64 timeout = Now() + 50'000'000;
        while (!token.stop_requested()) {
            {
                std::scoped_lock guard(handle_mutex);
                if (!swapchain || sample.generation != generation || ReflexActive()) break;
                if (epoch != generation) { display.Reset(); epoch = generation; last = 0; }
                display.SetNominalDisplayPeriod(nominal.load(std::memory_order_acquire));
                const auto result = wait_present(device, swapchain, sample.id, 0);
                const s64 shown = Now();
                if (result != VK_TIMEOUT || shown >= timeout) {
                    if (result != VK_SUCCESS || shown - sample.latch > 250'000'000) {
                        display.Reset(); locked = false; correction = 0; last = 0;
                    } else {
                        if (last && shown - last > 250'000'000) display.Reset();
                        last = shown;
                        const auto adjustment = display.AddSample(sample.latch, sample.ready, shown);
                        locked.store(adjustment.locked, std::memory_order_release);
                        correction.fetch_add(adjustment.correction_ns, std::memory_order_acq_rel);
                    }
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
}
}
