// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include "orbis/common/types.h"
struct SDL_Window;

namespace Gpu {
// ---- Physical device queries and Vulkan 1.3 device creation ----
// The renderer's queue family needs graphics, compute and sparse binding.
bool QueueFamilySuitable(VkQueueFlags flags);
std::vector<VkQueueFamilyProperties> QueueFamilies(VkPhysicalDevice pd);
// The copy engine: a family that only transfers (~0u: none).
std::uint32_t CopyFamily(std::span<const VkQueueFamilyProperties> fp);
bool SupportsBlendInterlock(VkPhysicalDevice pd);
// VK_KHR_maintenance5 is there: the mirror is bound as index buffer with a size (vkCmdBindIndexBuffer2KHR).
bool SupportsMaintenance5(VkPhysicalDevice pd);
// The extensions the renderer cannot run without that `pd` lacks, each after a space (empty: none).
std::string MissingExtensions(VkPhysicalDevice pd);
// Optional features that the device actually enabled. CreateDevice asks the GPU (vkGetPhysicalDeviceFeatures2) what it supports and
// switches off whatever it lacks instead of failing with VK_ERROR_FEATURE_NOT_PRESENT (-8); these are the ones with a fallback
// at the point of use. Every flag is true until CreateDevice has run.
struct DeviceFeatures {
    bool sampler_filter_minmax = true;      // S# MIN/MAX reduction filters; without it the sampler averages (weighted average)
    bool sampler_mirror_clamp = true;       // MIRROR_CLAMP_TO_EDGE addressing; without it MIRRORED_REPEAT is used
    bool occlusion_query_precise = true;    // exact sample counts; without it the query only tells whether anything passed
    bool depth_bounds = true;               // depth bounds test; without it the test stays off
    bool pipeline_statistics_query = true;  // trace probes only
};
const DeviceFeatures& Features();
// Vulkan 1.3 device with every feature and extension the renderer uses (minus the features `pd` lacks, which are logged), plus
// `extensions` (e.g. the swapchain); `requested` receives each extension asked for, " (MISSING)" after those the device does not list.
VkResult CreateDevice(VkPhysicalDevice pd, std::uint32_t family, std::span<const char* const> extensions, VkDevice* device, bool* present_wait = nullptr, bool* reflex = nullptr, bool* unified_layouts = nullptr, std::string* requested = nullptr);
// TEMP fault (debug runs): a VK_EXT_device_address_binding_report message, searched when the device is lost.
void TempAddressBinding(const VkDebugUtilsMessengerCallbackDataEXT* data);
} // namespace Gpu

// ---- Shared Vulkan instance, surface and device (live emulator) ----
// The one Vulkan instance and device of the process, shared by everything that draws.
namespace Gow3::Vulkan {
struct Device {
    VkInstance instance;
    VkSurfaceKHR surface; // the game window
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue; // graphics queue that also presents to the window
    u32 family;
    bool present_wait;
    bool reflex;
    bool full_screen_exclusive;  // VK_EXT_full_screen_exclusive: the presenter decides when the window is exclusive
};
// Creates the instance, the window surface and the renderer's Vulkan 1.3 device (Gpu::CreateDevice) with the swapchain.
void Initialize(SDL_Window* window);
const Device& Get();
std::mutex& QueueMutex();  // every use of the shared queue (renderer and presentation threads)
void Check(VkResult result, const char* what);
} // namespace Gow3::Vulkan
