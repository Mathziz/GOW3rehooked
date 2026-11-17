// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <shared_mutex>
#include <thread>
#include <vulkan/vulkan.h>
#include "present/display_pacer.h"

struct SDL_Window;
namespace Gow3::Presenter {
class PresentationTiming {
public:
    PresentationTiming(VkDevice device, SDL_Window* window, s64 period, bool present_wait, bool reflex);
    ~PresentationTiming();
    void Attach(VkSwapchainKHR swapchain);
    void UpdateDisplay(SDL_Window* window);
    void Detach();
    VkResult Acquire(VkSemaphore semaphore, u32* index);
    VkResult Present(VkQueue queue, const VkPresentInfoKHR& info, std::mutex& queue_mutex);
    VkResult SubmitAndPresent(VkQueue queue, const VkSubmitInfo& submit, VkFence fence, const VkPresentInfoKHR& present,
                              u64 id, s64 latch, std::mutex& queue_mutex);
    s64 Pace(VkFence fence, s64 latch);
    void Presented(u64 id, s64 latch, s64 ready);
    bool ReflexActive() const { return reflex_active.load(std::memory_order_acquire); }
    void BeginInput();
    u64 EndSimulation();
    void Marker(u64 id, VkLatencyMarkerNV marker);
    s64 TakeCorrection() { return correction.exchange(0, std::memory_order_acq_rel); }
    static s64 Now();
private:
    void Feedback(const std::stop_token& token);
    void DisableReflex();
    struct Sample { u64 generation, id; s64 latch, ready; };
    VkDevice device;
    s64 period;
    PFN_vkWaitForPresentKHR wait_present{};
    PFN_vkSetLatencySleepModeNV set_sleep_mode{};
    PFN_vkLatencySleepNV latency_sleep{};
    PFN_vkSetLatencyMarkerNV set_marker{};
    VkSemaphore sleep_semaphore{};
    std::atomic<bool> reflex_active{};
    std::atomic<u64> frame_id{}, present_count{};
    u64 sleep_value{}, next_frame{}, slept_present = ~u64{};
    std::mutex input_mutex;
    ::Vulkan::DisplayPacer display;
    ::Vulkan::VrrPacer pacer;
    std::atomic<bool> locked{};
    std::atomic<s64> correction{}, nominal{};
    std::mutex handle_mutex;
    VkSwapchainKHR swapchain{};
    u64 generation{};
    std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Sample> samples;
    std::jthread feedback;
};
s64 TakeVblankCorrection();
}
