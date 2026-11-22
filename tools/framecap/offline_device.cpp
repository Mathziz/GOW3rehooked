// SPDX-License-Identifier: GPL-2.0-or-later
// What only the offline harness needs of the renderer: its own Vulkan device (the game borrows the window's, Renderer::Init(External)).
#include "gpu/device.h"
#include "gpu/renderer_impl.h"

// The host.cpp pieces the shared renderer code links against (the harness does not build host.cpp).
namespace Gow3 {
std::filesystem::path ExecutableDirectory() {
    wchar_t path[MAX_PATH];
    return std::filesystem::path(std::wstring(path, GetModuleFileNameW(nullptr, path, MAX_PATH))).parent_path();
}
bool AotBuildAllowed() { return true; }  // the launch breaker is the game's
void BeginAotBuild() {}
void EndAotBuild() {}
namespace CpuProbe {  // TEMP: the game's sampler
void Register(u32) {}
void BlocksBegin() {}
void BlocksEnd() {}
}
} // namespace Gow3

namespace Gpu {
bool Renderer::Init(bool validation, std::string& error) { return d->Init(validation, error); }

bool Renderer::Impl::Init(bool validation, std::string& error) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "gow3", 1, "gow3", 1, VK_API_VERSION_1_3};
    const char* layers[]{"VK_LAYER_KHRONOS_validation"};
    const char* inst_ext[]{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, validation ? 1u : 0u, layers, validation ? 1u : 0u, inst_ext};
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return error = "vkCreateInstance failed", false;
    if (validation) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mi.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d,
                                void* user) -> VkBool32 {
            auto* self = (Impl*)user;
            ++self->stats.validation;
            self->Note(std::string("validation: ") + (d->pMessageIdName ? d->pMessageIdName : "?") + ": " +
                       std::string(d->pMessage ? d->pMessage : "").substr(0, 400));
            return VK_FALSE;
        };
        mi.pUserData = this;
        create(inst, &mi, nullptr, &messenger);
    }
    u32 n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    for (auto p : pds) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(p, &pp);
        if (!pd || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = p;
    }
    if (!pd) return error = "no Vulkan device", false;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf.data());
    for (family = 0; family < n && !QueueFamilySuitable(qf[family].queueFlags); ++family) {}
    if (family == n) return error = "no graphics queue with sparse binding", false;
    if (const auto m = MissingExtensions(pd); !m.empty()) return error = "missing Vulkan extensions:" + m, false;
    if (CreateDevice(pd, family, {}, &dev) != VK_SUCCESS) return error = "vkCreateDevice failed (features)", false;
    vkGetDeviceQueue(dev, family, 0, &queue);
    return Setup(error);
}
} // namespace Gpu
