// SPDX-License-Identifier: GPL-2.0-or-later
// The Vulkan instance, window surface and device shared by the renderer and the presenter, and the renderer's own device.
#include "gpu/device.h"
#include "gpu/renderer_impl.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <execinfo.h>
#endif
#ifndef GOW3_OFFLINE  // the window's instance needs SDL and the emulator's runtime (framecap builds without them)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "main/host.h"

// ---- Shared live Vulkan instance, window surface and device ----

namespace Gow3::Vulkan {
namespace {
Device context;
std::mutex queue_mutex;
} // namespace

void Check(VkResult result, const char* what) {
    if (result < 0) throw std::runtime_error(std::format("{} failed: {}", what, int(result)));
}

const Device& Get() { return context; }
std::mutex& QueueMutex() { return queue_mutex; }

void Initialize(SDL_Window* window) {
    u32 extension_count{};
    const auto* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!extensions) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions: ") + SDL_GetError());
    const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "God of War III",
                                .apiVersion = VK_API_VERSION_1_3};
    // GOW3_VK_VALIDATE=1 or GOW3_DEBUG=2: Khronos validation, each message ID logged a few times (skipped if layer is absent).
    bool validate = DebugDeep("GOW3_VK_VALIDATE");
    std::vector<const char*> instance_extensions(extensions, extensions + extension_count);
    const bool temp_fault = DebugAuto("GOW3_TEMP_FAULT");  // fault diagnostics: GPU address bindings
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validate) {
        u32 layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
        if (std::none_of(layers.begin(), layers.end(), [layer](const VkLayerProperties& p) { return std::strcmp(p.layerName, layer) == 0; })) {
            Log("Vulkan validation requested, but VK_LAYER_KHRONOS_validation is not available");
            validate = false;
        }
    }
    if (validate || temp_fault || DebugAuto("GOW3_TEMP_LABELS")) instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);  // labels
    u32 count = 0;
#ifdef _WIN32
    auto has = [](std::span<const VkExtensionProperties> list, const char* name) {
        return std::ranges::any_of(list, [&](const auto& e) { return !std::strcmp(e.extensionName, name); });
    };
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> instance_available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_available.data());
    const bool caps2 = has(instance_available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);  // needed by full_screen_exclusive
    if (caps2) instance_extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
#endif
    const VkInstanceCreateInfo instance_info{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledLayerCount = validate ? 1u : 0u, .ppEnabledLayerNames = &layer,
        .enabledExtensionCount = u32(instance_extensions.size()), .ppEnabledExtensionNames = instance_extensions.data()};
    Check(vkCreateInstance(&instance_info, nullptr, &context.instance), "Create Vulkan instance");

    auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(context.instance, "vkCreateDebugUtilsMessengerEXT"));
    auto add_messenger = [&](const VkDebugUtilsMessengerCreateInfoEXT& info) {
        VkDebugUtilsMessengerEXT handle{};
        if (create_messenger) create_messenger(context.instance, &info, nullptr, &handle);
    };
    if (validate) {
        add_messenger({.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 {
                static std::mutex lock;
                static std::unordered_map<s32, u32> seen;
                std::scoped_lock guard(lock);
                if (const u32 n = ++seen[data->messageIdNumber]; n <= 4)
                    Log(std::format("VK {} #{} {}", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR" : "WARN", n,
                                    data->pMessage ? data->pMessage : ""));
                return VK_FALSE;
            }});
    }
    if (temp_fault) {
        add_messenger({.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
            .pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void*) -> VkBool32 { return Gpu::TempAddressBinding(data), VK_FALSE; }});
    }
    if (!SDL_Vulkan_CreateSurface(window, context.instance, nullptr, &context.surface))
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());

    // Prefer a discrete GPU with a queue the renderer can use that presents to the window.
    vkEnumeratePhysicalDevices(context.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(context.instance, &count, devices.data());
    int best = -1;
    std::string lacking;  // presenting devices skipped for a required extension
#ifdef __linux__
    constexpr u32 amd_vendor_id = 0x1002;
    bool best_radeon = false, best_radv = false, missing_radeon_maintenance5 = false;
#endif
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties info{};
        vkGetPhysicalDeviceProperties(candidate, &info);
        const int score = info.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
        if (info.apiVersion < VK_API_VERSION_1_3 || score < best) continue;

        const auto properties = Gpu::QueueFamilies(candidate);
        for (u32 i = 0; i < properties.size(); ++i) {
            if (!Gpu::QueueFamilySuitable(properties[i].queueFlags)) continue;
            VkBool32 presents{};
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, context.surface, &presents);
            if (!presents) continue;
            if (const auto missing = Gpu::MissingExtensions(candidate); !missing.empty()) {
                lacking += std::format("{}{} lacks{}", lacking.empty() ? "" : "; ", info.deviceName, missing);
                break;
            }

            bool preferred = score > best;
#ifdef __linux__
            const bool radeon = info.vendorID == amd_vendor_id;
            if (radeon && !Gpu::SupportsMaintenance5(candidate)) {
                missing_radeon_maintenance5 = true;
                break;
            }
            bool radv = false;
            if (radeon) {
                VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
                vkGetPhysicalDeviceProperties2(candidate, &properties2);
                radv = driver.driverID == VK_DRIVER_ID_MESA_RADV;
                preferred = preferred || (radv && best_radeon && !best_radv);
            }
            if (preferred) best_radeon = radeon, best_radv = radv;
#endif
            if (preferred) best = score, context.physical = candidate, context.family = i;
            break;
        }
    }
    if (best < 0 && !lacking.empty())
        throw std::runtime_error(lacking + ". Update the GPU's Vulkan driver (on Linux, Mesa 24.1 or newer).");
#ifdef __linux__
    if (best < 0 && missing_radeon_maintenance5)
        throw std::runtime_error("Radeon requires VK_KHR_maintenance5. Install or update the Mesa Vulkan driver (RADV).");
#endif
    if (best < 0) throw std::runtime_error("No Vulkan 1.3 device presents to the window");

    bool unified_layouts = false;
    std::vector<const char*> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#ifdef _WIN32
    vkEnumerateDeviceExtensionProperties(context.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> device_available(count);
    vkEnumerateDeviceExtensionProperties(context.physical, nullptr, &count, device_available.data());
    context.full_screen_exclusive = caps2 && has(device_available, "VK_EXT_full_screen_exclusive");
    if (context.full_screen_exclusive) device_extensions.push_back("VK_EXT_full_screen_exclusive");
#endif
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(context.physical, &properties);
    std::string requested;
    const VkResult created = Gpu::CreateDevice(context.physical, context.family, device_extensions, &context.device, &context.present_wait,
                                               &context.reflex, &unified_layouts, &requested);
    Log(std::format("Vulkan device: {} ({} {}); extensions:{}", properties.properties.deviceName, driver.driverName, driver.driverInfo, requested));
    Check(created, "Create Vulkan device");
    Log(std::format("Vulkan Unified Image Layouts: {}", unified_layouts ? "supported and enabled" : "not supported (disabled)"));
    vkGetDeviceQueue(context.device, context.family, 0, &context.queue);
}
} // namespace Gow3::Vulkan
#endif

namespace Gpu {
namespace {
std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice pd) {
    u32 n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, available.data());
    return available;
}

bool HasExtension(std::span<const VkExtensionProperties> available, const char* name) {
    return std::any_of(available.begin(), available.end(), [&](const auto& e) { return !std::strcmp(e.extensionName, name); });
}

bool SupportsBlendInterlock(VkPhysicalDevice pd, std::span<const VkExtensionProperties> available) {
    if (!HasExtension(available, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) ||
        !HasExtension(available, VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME))
        return false;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &interlock};
    vkGetPhysicalDeviceFeatures2(pd, &query);
    return interlock.fragmentShaderPixelInterlock && query.features.shaderStorageImageMultisample && query.features.shaderStorageImageArrayDynamicIndexing;
}

bool SupportsMaintenance5(VkPhysicalDevice pd, std::span<const VkExtensionProperties> available) {
    if (!HasExtension(available, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) return false;
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m5};
    vkGetPhysicalDeviceFeatures2(pd, &query);
    return m5.maintenance5;
}

constexpr const char* kRequiredExtensions[]{VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
                                            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME};

DeviceFeatures enabled_features;  // written once by CreateDevice, before any renderer thread starts

void LogFeatures(const std::string& message) {
#ifndef GOW3_OFFLINE
    Gow3::Log(message);
#else
    std::fprintf(stderr, "%s\n", message.c_str());
#endif
}

// The features that the device does not report, split by what losing them costs.
struct DroppedFeatures {
    std::string optional;  // the renderer has a fallback (DeviceFeatures)
    std::string required;  // no fallback: the game may fail to start a shader or draw wrongly without them
};

// vkCreateDevice fails with VK_ERROR_FEATURE_NOT_PRESENT (-8) when a single requested feature is missing (a GeForce GTX 750 Ti
// has no min/max sampler filtering, for one). Asks the GPU what it supports (vkGetPhysicalDeviceFeatures2) and switches off every
// requested feature it lacks; `caps` receives what the optional ones ended up as.
DroppedFeatures DropUnsupportedFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures& f, VkPhysicalDeviceVulkan11Features& f11,
                                        VkPhysicalDeviceVulkan12Features& f12, VkPhysicalDeviceVulkan13Features& f13,
                                        VkPhysicalDeviceShaderObjectFeaturesEXT& so, VkPhysicalDeviceConditionalRenderingFeaturesEXT& cr,
                                        VkPhysicalDeviceDepthClipControlFeaturesEXT& dcc, DeviceFeatures& caps) {
    VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &h13};
    VkPhysicalDeviceVulkan11Features h11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &h12};
    VkPhysicalDeviceShaderObjectFeaturesEXT hso{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, &h11};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT hcr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, &hso};
    VkPhysicalDeviceDepthClipControlFeaturesEXT hdcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &hcr};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hdcc};
    vkGetPhysicalDeviceFeatures2(pd, &have);

    DroppedFeatures dropped;
    auto keep = [&](VkBool32& want, VkBool32 got, const char* name, bool optional) {
        if (!want || got) return;
        want = VK_FALSE;
        (optional ? dropped.optional : dropped.required) += std::format(" {}", name);
    };
#define KEEP(want, got, field, optional) keep((want).field, (got).field, #field, optional)
    const VkPhysicalDeviceFeatures& hf = have.features;
    // Core 1.0 features.
    KEEP(f, hf, shaderInt16, false);
    KEEP(f, hf, geometryShader, false);
    KEEP(f, hf, sparseBinding, false);
    KEEP(f, hf, sparseResidencyBuffer, false);
    KEEP(f, hf, shaderInt64, false);
    KEEP(f, hf, independentBlend, false);
    KEEP(f, hf, dualSrcBlend, false);
    KEEP(f, hf, samplerAnisotropy, false);
    KEEP(f, hf, imageCubeArray, false);
    KEEP(f, hf, fragmentStoresAndAtomics, false);
    KEEP(f, hf, vertexPipelineStoresAndAtomics, false);
    KEEP(f, hf, depthClamp, false);
    KEEP(f, hf, depthBounds, true);
    KEEP(f, hf, shaderStorageImageReadWithoutFormat, false);
    KEEP(f, hf, shaderStorageImageWriteWithoutFormat, false);
    KEEP(f, hf, shaderStorageImageExtendedFormats, false);
    KEEP(f, hf, shaderStorageImageMultisample, false);
    KEEP(f, hf, shaderStorageImageArrayDynamicIndexing, false);
    KEEP(f, hf, depthBiasClamp, false);
    KEEP(f, hf, logicOp, false);
    KEEP(f, hf, pipelineStatisticsQuery, true);
    KEEP(f, hf, occlusionQueryPrecise, true);
    // Vulkan 1.1.
    KEEP(f11, h11, storageBuffer16BitAccess, false);
    // Vulkan 1.2.
    KEEP(f12, h12, bufferDeviceAddress, false);
    KEEP(f12, h12, descriptorIndexing, false);
    KEEP(f12, h12, runtimeDescriptorArray, false);
    KEEP(f12, h12, descriptorBindingPartiallyBound, false);
    KEEP(f12, h12, descriptorBindingSampledImageUpdateAfterBind, false);
    KEEP(f12, h12, descriptorBindingStorageImageUpdateAfterBind, false);
    KEEP(f12, h12, shaderSampledImageArrayNonUniformIndexing, false);
    KEEP(f12, h12, shaderStorageImageArrayNonUniformIndexing, false);
    KEEP(f12, h12, scalarBlockLayout, false);
    KEEP(f12, h12, shaderInt8, false);
    KEEP(f12, h12, storageBuffer8BitAccess, false);
    KEEP(f12, h12, samplerFilterMinmax, true);
    KEEP(f12, h12, samplerMirrorClampToEdge, true);
    KEEP(f12, h12, hostQueryReset, false);
    KEEP(f12, h12, timelineSemaphore, false);
    // Vulkan 1.3.
    KEEP(f13, h13, dynamicRendering, false);
    KEEP(f13, h13, synchronization2, false);
    KEEP(f13, h13, maintenance4, false);
    KEEP(f13, h13, shaderDemoteToHelperInvocation, false);
    KEEP(f13, h13, robustImageAccess, true);
    // Extensions.
    KEEP(so, hso, shaderObject, false);
    KEEP(cr, hcr, conditionalRendering, false);
    KEEP(dcc, hdcc, depthClipControl, false);
#undef KEEP
    caps.sampler_filter_minmax = f12.samplerFilterMinmax;
    caps.sampler_mirror_clamp = f12.samplerMirrorClampToEdge;
    caps.occlusion_query_precise = f.occlusionQueryPrecise;
    caps.depth_bounds = f.depthBounds;
    caps.pipeline_statistics_query = f.pipelineStatisticsQuery;
    return dropped;
}

const auto temp_bind_t0 = std::chrono::steady_clock::now();
double TempBindMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - temp_bind_t0).count(); }
} // namespace

// ---- Physical device queries and device creation ----

const DeviceFeatures& Features() { return enabled_features; }

bool QueueFamilySuitable(VkQueueFlags flags) {
    const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
    return (flags & want) == want;
}

std::vector<VkQueueFamilyProperties> QueueFamilies(VkPhysicalDevice pd) {
    u32 n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fp(n);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, fp.data());
    return fp;
}

// The copy engine: a family that only transfers (~0u: none).
u32 CopyFamily(std::span<const VkQueueFamilyProperties> fp) {
    for (u32 i = 0; i < fp.size(); ++i)
        if (fp[i].queueFlags & VK_QUEUE_TRANSFER_BIT && !(fp[i].queueFlags & ~(VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT | VK_QUEUE_PROTECTED_BIT)))
            return i;
    return ~0u;
}

bool SupportsBlendInterlock(VkPhysicalDevice pd) { return SupportsBlendInterlock(pd, DeviceExtensions(pd)); }
bool SupportsMaintenance5(VkPhysicalDevice pd) { return SupportsMaintenance5(pd, DeviceExtensions(pd)); }

std::string MissingExtensions(VkPhysicalDevice pd) {
    const auto available = DeviceExtensions(pd);
    std::string missing;
    for (const char* name : kRequiredExtensions)
        if (!HasExtension(available, name)) missing += std::format(" {}", name);
    return missing;
}

VkResult CreateDevice(VkPhysicalDevice pd, u32 family, std::span<const char* const> extensions, VkDevice* device, bool* present_wait, bool* reflex, bool* unified_layouts,
                      std::string* requested) {
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, nullptr, VK_TRUE};
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, &dcc, VK_TRUE};  // predication
    const bool temp_stats = DebugDeep("GOW3_TEMP_STATS");  // driver statistics of each program (Compile)
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR temp_pep{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, &cr, VK_TRUE};
    VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, temp_stats ? (void*)&temp_pep : &cr, VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &so};
    f13.dynamicRendering = f13.synchronization2 = f13.maintenance4 = f13.shaderDemoteToHelperInvocation = VK_TRUE;
    // A slot that falls back to the 1x1 dummy: its out-of-bounds writes are dropped, not stray.
    f13.robustImageAccess = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.bufferDeviceAddress = f12.descriptorIndexing = f12.runtimeDescriptorArray = f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = f12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = f12.shaderStorageImageArrayNonUniformIndexing = f12.scalarBlockLayout = VK_TRUE;
    f12.shaderInt8 = f12.storageBuffer8BitAccess = f12.samplerFilterMinmax = f12.samplerMirrorClampToEdge = VK_TRUE;  // tiling.comp, S#
    f12.hostQueryReset = VK_TRUE;  // trace probes
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    f11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    auto& f = f2.features;
    f.shaderInt16 = VK_TRUE;
    f.geometryShader = f.sparseBinding = f.sparseResidencyBuffer = f.shaderInt64 = f.independentBlend = f.dualSrcBlend = VK_TRUE;
    f.samplerAnisotropy = f.imageCubeArray = f.fragmentStoresAndAtomics = f.vertexPipelineStoresAndAtomics = f.depthClamp = f.depthBounds = VK_TRUE;
    f.shaderStorageImageReadWithoutFormat = f.shaderStorageImageWriteWithoutFormat = f.shaderStorageImageExtendedFormats = f.depthBiasClamp = f.logicOp = VK_TRUE;
    f.pipelineStatisticsQuery = f.occlusionQueryPrecise = VK_TRUE;
    const float prio[2]{1.0f, 1.0f};
    const auto fp = QueueFamilies(pd);
    // The second queue (when the family has one) uploads the next frame while this one renders; two of the copy engine's
    // (Setup prefers them).
    VkDeviceQueueCreateInfo qci[2]{{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, family, std::min(2u, fp[family].queueCount), prio}};
    u32 queue_infos = 1;
    if (const u32 cf = CopyFamily(fp); cf != ~0u) qci[queue_infos++] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, cf, std::min(2u, fp[cf].queueCount), prio};
    std::vector<const char*> ext(std::begin(kRequiredExtensions), std::end(kRequiredExtensions));
    const auto available = DeviceExtensions(pd);
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unified{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    if (HasExtension(available, VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &unified};
        vkGetPhysicalDeviceFeatures2(pd, &query);
        unified.unifiedImageLayoutsVideo = VK_FALSE;
        if (unified.unifiedImageLayouts) {
            ext.push_back(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME);
            unified.pNext = dcc.pNext, dcc.pNext = &unified;
        }
    }
    if (unified_layouts) *unified_layouts = unified.unifiedImageLayouts;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    if (SupportsBlendInterlock(pd, available)) {
        ext.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME), ext.push_back(VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME);
        interlock.fragmentShaderPixelInterlock = VK_TRUE, interlock.pNext = dcc.pNext, dcc.pNext = &interlock;
        f.shaderStorageImageMultisample = f.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
    }
    if (temp_stats) ext.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, nullptr, VK_TRUE};  // sized index binding
    if (SupportsMaintenance5(pd, available)) ext.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME), m5.pNext = dcc.pNext, dcc.pNext = &m5;
    ext.insert(ext.end(), extensions.begin(), extensions.end());
    VkPhysicalDevicePresentIdFeaturesKHR present_id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDevicePresentWaitFeaturesKHR wait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    if (reflex) *reflex = false;
    if (present_wait) {
        *present_wait = false;
        if (HasExtension(available, VK_KHR_PRESENT_ID_EXTENSION_NAME) && HasExtension(available, VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            present_id.pNext = &wait;
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &present_id};
            vkGetPhysicalDeviceFeatures2(pd, &query);
            if (present_id.presentId && wait.presentWait) {
                ext.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
                ext.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
                wait.pNext = dcc.pNext;
                dcc.pNext = &present_id;
                *present_wait = true;
                if (reflex && HasExtension(available, VK_NV_LOW_LATENCY_2_EXTENSION_NAME)) {
                    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
                    VkPhysicalDeviceFeatures2 timeline_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
                    vkGetPhysicalDeviceFeatures2(pd, &timeline_query);
                    if (timeline.timelineSemaphore) {
                        ext.push_back(VK_NV_LOW_LATENCY_2_EXTENSION_NAME);
                        f12.timelineSemaphore = VK_TRUE;
                        *reflex = true;
                    }
                }
            }
        }
    }
    // Only what the GPU supports is requested: a missing feature no longer makes vkCreateDevice fail with -8. What was switched off
    // is logged before the device is created.
    DeviceFeatures caps;
    const DroppedFeatures dropped = DropUnsupportedFeatures(pd, f, f11, f12, f13, so, cr, dcc, caps);
    enabled_features = caps;
    if (dropped.optional.empty() && dropped.required.empty())
        LogFeatures("Vulkan features: the GPU supports every feature the renderer asks for");
    if (!dropped.optional.empty())
        LogFeatures(std::format("Vulkan features switched off, not supported by the GPU (fallback in use):{}", dropped.optional));
    if (!dropped.required.empty())
        LogFeatures(std::format("Vulkan features switched off, not supported by the GPU, THAT THE RENDERER NEEDS (the game may fail or draw wrongly):{}",
                                dropped.required));
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, queue_infos, qci, 0, nullptr, (u32)ext.size(), ext.data()};
    // TEMP fault (debug runs, GOW3_TEMP_FAULT=0 off): VK_EXT_device_fault and NVIDIA diagnostics (checkpoints), read when the device is lost.
    VkPhysicalDeviceFaultFeaturesEXT temp_fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, nullptr, VK_TRUE, VK_FALSE};
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV temp_diag{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, nullptr, VK_TRUE};
    VkPhysicalDeviceAddressBindingReportFeaturesEXT temp_abr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT, nullptr, VK_TRUE};
    VkDeviceDiagnosticsConfigCreateInfoNV temp_diag_ci{VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, &f2,
                                                       VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV |
                                                           VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |
                                                           VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV};
    if (DebugAuto("GOW3_TEMP_FAULT")) {
        const bool has_fault = HasExtension(available, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool has_ckpt = HasExtension(available, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        const bool has_abr = HasExtension(available, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
        const bool has_diag = HasExtension(available, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        if (has_fault) ext.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME), temp_fault.pNext = dcc.pNext, dcc.pNext = &temp_fault;
        if (has_ckpt) ext.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        if (has_abr) ext.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME), temp_abr.pNext = dcc.pNext, dcc.pNext = &temp_abr;
        // NVIDIA's automatic checkpoints, resource tracking and shader debug info cost on every draw: GOW3_TEMP_FAULT=1 only.
        if (has_diag && DebugDeep("GOW3_TEMP_FAULT")) ext.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME), temp_diag.pNext = dcc.pNext, dcc.pNext = &temp_diag, dci.pNext = &temp_diag_ci;
        dci.enabledExtensionCount = (u32)ext.size(), dci.ppEnabledExtensionNames = ext.data();
        std::fprintf(stderr, "TEMP fault: device_fault %d checkpoints %d diagnostics %d binding report %d\n",
                     has_fault, has_ckpt, has_diag && DebugDeep("GOW3_TEMP_FAULT"), has_abr);
    }
    if (requested)
        for (const char* name : ext) *requested += std::format(" {}{}", name, HasExtension(available, name) ? "" : " (MISSING)");
    return vkCreateDevice(pd, &dci, nullptr, device);
}

// ---- Renderer device setup and fault diagnostics ----

void Renderer::Impl::LoadDeviceFunctions() {
    const auto available = DeviceExtensions(pd);
    blend_interlock = SupportsBlendInterlock(pd, available);
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13};
    vkGetPhysicalDeviceProperties2(pd, &props);
    uma_budget = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && HasExtension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    image_copy_via_temp = p13.maxBufferSize < kMirrorMax;  // before SetupMirror: against the largest mirror
#define LOAD(var, name) var = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    LOAD(CreateShaders, vkCreateShadersEXT);
    LOAD(GetShaderBinary, vkGetShaderBinaryDataEXT);
    LOAD(BindShaders, vkCmdBindShadersEXT);
    LOAD(SetVertexInput, vkCmdSetVertexInputEXT);
    LOAD(SetPolygonMode, vkCmdSetPolygonModeEXT);
    LOAD(SetSamples, vkCmdSetRasterizationSamplesEXT);
    LOAD(SetSampleMask, vkCmdSetSampleMaskEXT);
    LOAD(SetAlphaToCoverage, vkCmdSetAlphaToCoverageEnableEXT);
    LOAD(SetBlendEnable, vkCmdSetColorBlendEnableEXT);
    LOAD(SetBlendEquation, vkCmdSetColorBlendEquationEXT);
    LOAD(SetWriteMask, vkCmdSetColorWriteMaskEXT);
    LOAD(SetDepthClamp, vkCmdSetDepthClampEnableEXT);
    LOAD(SetLogicOpEnable, vkCmdSetLogicOpEnableEXT);
    LOAD(SetNegativeOneToOne, vkCmdSetDepthClipNegativeOneToOneEXT);
    LOAD(DestroyShader, vkDestroyShaderEXT);
    LOAD(PushDescriptors, vkCmdPushDescriptorSetKHR);
    LOAD(BeginConditional, vkCmdBeginConditionalRenderingEXT), LOAD(EndConditional, vkCmdEndConditionalRenderingEXT);
    if (SupportsMaintenance5(pd, available)) LOAD(BindIndex2, vkCmdBindIndexBuffer2KHR);
    if (DebugAuto("GOW3_TEMP_LABELS")) LOAD(temp_label_begin, vkCmdBeginDebugUtilsLabelEXT), LOAD(temp_label_end, vkCmdEndDebugUtilsLabelEXT);
    if (DebugAuto("GOW3_TEMP_FAULT"))
        LOAD(temp_get_fault, vkGetDeviceFaultInfoEXT), LOAD(temp_set_check, vkCmdSetCheckpointNV), LOAD(temp_get_checks, vkGetQueueCheckpointDataNV);
#undef LOAD
}

// TEMP fault: GPU address ranges bound and unbound, in order (VK_EXT_device_address_binding_report).
std::mutex temp_bind_lock;
std::vector<TempBinding> temp_binds;
std::atomic<u32> temp_bind_frame{0};
void TempAddressBinding(const VkDebugUtilsMessengerCallbackDataEXT* d) {
    for (auto* p = (const VkBaseInStructure*)d->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            const auto* b = (const VkDeviceAddressBindingCallbackDataEXT*)p;
            const VkDebugUtilsObjectNameInfoEXT* o = d->objectCount ? d->pObjects : nullptr;
            std::string who;
            if (b->size >= (8u << 20)) {
#ifdef _WIN32
                PWSTR n = nullptr;
                if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &n)) && n)
                    for (const wchar_t* c = n; *c; ++c) who += (char)*c;
                if (n) LocalFree(n);
                void* f[40];
                for (u16 i = 0, k = RtlCaptureStackBackTrace(1, 40, f, nullptr); i < k; ++i) {
                    HMODULE m = nullptr;
                    char path[MAX_PATH] = "?";
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)f[i], &m)) GetModuleFileNameA(m, path, MAX_PATH);
                    who += std::format(" {}+{:#x}", std::filesystem::path(path).filename().string(), (u64)f[i] - (u64)m);
                }
#else
                who = Common::GetCurrentThreadNameView();
                void* frames[40];
                for (int i = 1, count = backtrace(frames, std::size(frames)); i < count; ++i) {
                    Dl_info info{};
                    if (dladdr(frames[i], &info) && info.dli_fname)
                        who += std::format(" {}+{:#x}", std::filesystem::path(info.dli_fname).filename().string(),
                                           reinterpret_cast<u64>(frames[i]) - reinterpret_cast<u64>(info.dli_fbase));
                    else
                        who += std::format(" ?+{:#x}", reinterpret_cast<u64>(frames[i]));
                }
#endif
            }
            std::scoped_lock lock(temp_bind_lock);
            temp_binds.push_back({b->baseAddress, b->size, o ? o->objectHandle : 0, o ? (u32)o->objectType : 0, (u32)b->flags,
                                  temp_bind_frame.load(std::memory_order_relaxed), b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT,
                                  temp_call ? temp_call : "-", std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000,
                                  (u32)Gow3::ThreadId(), TempBindMs(), std::move(who)});
        }
}

// TEMP device report (debug runs): what this GPU and driver answer where the renderer assumes: limits, queues, sparse memory,
// formats and extensions.
void Renderer::Impl::TempDeviceReport(std::span<const VkQueueFamilyProperties> fp) {
    VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p13};
    VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p12};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const VkPhysicalDeviceProperties& p = p2.properties;
    const VkPhysicalDeviceLimits& l = p.limits;
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(pd, &f);
    Report(std::format("Device: {} vendor {:04x} device {:04x} type {} api {}.{}.{} driver {:08x} \"{}\" \"{}\" id {} conformance {}.{}.{}.{}", p.deviceName, p.vendorID,
                       p.deviceID, (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
                       p.driverVersion, p12.driverName, p12.driverInfo, (int)p12.driverID, p12.conformanceVersion.major, p12.conformanceVersion.minor,
                       p12.conformanceVersion.subminor, p12.conformanceVersion.patch));
    Report(std::format("Limits: maxPushConstantsSize {} (used {}{}) maxBufferSize {:x} (mirror {:x}{}) maxMemoryAllocationSize {:x} sparseAddressSpaceSize {:x} "
                       "maxMemoryAllocationCount {} maxSamplerAllocationCount {} maxDrawIndexedIndexValue {:x} maxImageDimension2D {} maxColorAttachments {} "
                       "maxBoundDescriptorSets {} maxUniformBufferRange {:x} maxStorageBufferRange {:x} minUniformBufferOffsetAlignment {} "
                       "minStorageBufferOffsetAlignment {} bufferImageGranularity {} nonCoherentAtomSize {} timestampPeriod {} maxSamplerAnisotropy {}",
                       l.maxPushConstantsSize, sizeof(Push), l.maxPushConstantsSize < sizeof(Push) ? ": TOO SMALL" : "", p13.maxBufferSize, mirror_size,
                       p13.maxBufferSize < mirror_size ? ": TOO SMALL" : "", p11.maxMemoryAllocationSize, l.sparseAddressSpaceSize, l.maxMemoryAllocationCount,
                       l.maxSamplerAllocationCount, l.maxDrawIndexedIndexValue, l.maxImageDimension2D, l.maxColorAttachments, l.maxBoundDescriptorSets,
                       l.maxUniformBufferRange, l.maxStorageBufferRange, l.minUniformBufferOffsetAlignment, l.minStorageBufferOffsetAlignment, l.bufferImageGranularity,
                       l.nonCoherentAtomSize, l.timestampPeriod, l.maxSamplerAnisotropy));
    Report(std::format("Bindless: uses sampled {} samplers {} storage {}; update-after-bind limits: set sampled {} samplers {} storage {}, stage sampled {} samplers {} "
                       "storage {} resources {}, all pools {}; dynamic uniform buffers per set {}",
                       18 * kTextures, kSamplers, 9 * kStorage, p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindSamplers,
                       p12.maxDescriptorSetUpdateAfterBindStorageImages, p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                       p12.maxPerStageDescriptorUpdateAfterBindSamplers, p12.maxPerStageDescriptorUpdateAfterBindStorageImages, p12.maxPerStageUpdateAfterBindResources,
                       p12.maxUpdateAfterBindDescriptorsInAllPools, l.maxDescriptorSetUniformBuffersDynamic));
    Report(std::format("Subgroup: size {} (from {} to {}) stages {:x} sized stages {:x} operations {:x} quad in all stages {}", p11.subgroupSize, p13.minSubgroupSize,
                       p13.maxSubgroupSize, (u32)p11.subgroupSupportedStages, (u32)p13.requiredSubgroupSizeStages, (u32)p11.subgroupSupportedOperations,
                       p11.subgroupQuadOperationsInAllStages));
    Report(std::format("Sparse: binding {} residency buffer {} aliased {} non-resident strict {}; mirror page {:x} memory types {:x}, integrated with budget {}, image copies through a temp buffer {}",
                       f.sparseBinding, f.sparseResidencyBuffer, f.sparseResidencyAliased, p.sparseProperties.residencyNonResidentStrict, page, mirror_bits, uma_budget,
                       image_copy_via_temp));
    std::string s;
    for (u32 i = 0; i < fp.size(); ++i)
        s += std::format(" [{}: flags {:x} x{} timestamp bits {} granularity {}x{}x{}]", i, (u32)fp[i].queueFlags, fp[i].queueCount, fp[i].timestampValidBits,
                         fp[i].minImageTransferGranularity.width, fp[i].minImageTransferGranularity.height, fp[i].minImageTransferGranularity.depth);
    Report(std::format("Queues:{}; render family {}, uploads on {}, readback {}", s, family,
                       copy_family != ~0u ? std::format("the copy engine (family {})", copy_family) : xfer != queue ? "a second render queue" : "the render queue",
                       rbq ? "on its own queue" : "with the frame (no second copy queue)"));
    s.clear();
    for (u32 i = 0; i < mem.memoryHeapCount; ++i) s += std::format(" heap {} {} MB flags {:x}", i, mem.memoryHeaps[i].size >> 20, (u32)mem.memoryHeaps[i].flags);
    for (u32 i = 0; i < mem.memoryTypeCount; ++i) s += std::format(" type {} heap {} flags {:x}", i, mem.memoryTypes[i].heapIndex, (u32)mem.memoryTypes[i].propertyFlags);
    Report("Memory:" + s);
    static constexpr VkFormat formats[]{
        VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, VK_FORMAT_R5G5B5A1_UNORM_PACK16,
        VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT,
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK};
    s.clear();
    for (const VkFormat x : formats) {
        VkFormatProperties q;
        vkGetPhysicalDeviceFormatProperties(pd, x, &q);
        s += std::format(" {}:{:x}", (int)x, (u32)q.optimalTilingFeatures);
    }
    Report("Formats (VkFormat:optimal tiling features):" + s);
    const auto ext = DeviceExtensions(pd);
    s.clear();
    for (const auto& x : ext) s += std::format(" {}", x.extensionName + (std::strncmp(x.extensionName, "VK_", 3) ? 0 : 3));
    Report(std::format("Extensions ({}), fragment interlock used {}, sized index binding {}:{}", ext.size(), blend_interlock, BindIndex2 != nullptr, s));
}

void Renderer::Impl::TempLost(const char* where, VkResult r) {  // TEMP fault
    if (r != VK_ERROR_DEVICE_LOST || temp_lost.exchange(true)) return;
    std::scoped_lock lock(note_lock);
    auto note = [&](std::string s) { stats.notes.push_back("TEMP fault: " + std::move(s)); };
    note(std::format("{}: device lost at frame {}, mirror at {:#x}", where, frame, mirror_addr));
    if (temp_get_fault) {
        VkDeviceFaultCountsEXT n{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        temp_get_fault(dev, &n, nullptr);
        std::vector<VkDeviceFaultAddressInfoEXT> addr(n.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendor(n.vendorInfoCount);
        n.vendorBinarySize = 0;
        VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, nullptr, {}, addr.data(), vendor.data(), nullptr};
        note(std::format("fault info result {} '{}', {} addresses, {} vendor", (int)temp_get_fault(dev, &n, &info), info.description, addr.size(), vendor.size()));
        for (auto& a : addr) {
            const u64 g = a.reportedAddress - mirror_addr;
            note(std::format("address type {} {:#x} precision {:#x}{}", (int)a.addressType, a.reportedAddress, a.addressPrecision,
                             a.reportedAddress >= mirror_addr && g < mirror_size ? std::format(" = guest {:#x}, page bound {}", g, bound[g / page / 64] >> (g / page % 64) & 1) : ""));
            // Every binding over the faulting range, then the nearest ones below and above it, with the resource still holding the handle.
            auto name = [&](u64 h) -> std::string {
                auto is = [&](auto x) { return x && (u64)x == h; };
                if (is(mirror)) return "mirror";
                for (auto m : chunks)
                    if (is(m)) return "mirror chunk";
                const std::pair<const Buffer*, const char*> bufs[]{{&tables_host, "tables_host"}, {&tables, "tables"}, {&rb, "rb"}, {&scratch, "scratch"},
                                                                   {&flight.tables_host, "flight tables_host"}, {&flight.tables, "flight tables"},
                                                                   {&flight.rb, "flight rb"}, {&flight.scratch, "flight scratch"}, {&probe_buf, "probe"}, {&vertex_probe, "vertex probe"}};
                for (auto [b, n] : bufs)
                    if (is(b->buf) || is(b->mem)) return std::format("{} size {:#x}", n, b->size);
                for (auto [v, n] : {std::pair{&frame_temp, "temp"}, {&flight.temp, "flight temp"}, {&stage, "stage"}, {&flight.stage, "flight stage"}})
                    for (auto& b : *v)
                        if (is(b.buf) || is(b.mem)) return std::format("{} size {:#x}", n, b.size);
                for (auto& t : dummies)
                    if (is(t.img) || is(t.mem)) return "dummy";
                for (auto& [k, t] : targets)
                    if (is(t.t.img) || is(t.t.mem)) return std::format("target {:#x} {}x{} format {} samples {} usage {:#x}", t.base, t.w, t.h, (int)t.fmt, t.samples, t.t.usage);
                for (auto& [k, x] : textures)
                    if (is(x.t.img) || is(x.t.mem)) return std::format("texture {:#x} {}x{}x{} format {} tile {} storage {}", x.base, x.w, x.h, x.layers, (int)x.fmt, x.tile, x.storage);
                std::scoped_lock temp_l(temp_alloc_lock);
                for (auto it = temp_allocs.rbegin(); it != temp_allocs.rend(); ++it)
                    if (it->handle == h)
                        return std::format("freed buffer '{}' size {:#x}, made at frame {} slot {}, freed at frame {} (frame in flight busy {})", it->tag, it->size,
                                           it->made, it->slot, it->freed, it->busy);
                return "(no live resource)";
            };
            const u64 lo = a.reportedAddress & ~(std::max<u64>(a.addressPrecision, 1) - 1), hi = lo + std::max<u64>(a.addressPrecision, 1);
            std::scoped_lock bl(temp_bind_lock);
            const TempBinding *below = nullptr, *above = nullptr;
            u32 shown = 0;
            for (const TempBinding& b : temp_binds) {
                if (b.base < hi && b.base + b.size > lo && shown++ < 24)
                    note(std::format("binding over it: #{} {} at frame {} type {} flags {:#x} [{:#x}, {:#x}) handle {:#x}, thread {} tid {} in '{}' {:.1f} ms before this: {}; by '{}'",
                                     &b - temp_binds.data(), b.bound ? "bind" : "UNBIND", b.frame, b.type, b.flags, b.base, b.base + b.size, b.handle, b.thread, b.tid, b.call,
                                     TempBindMs() - b.ms, name(b.handle), b.who));
                if (b.bound && b.base + b.size <= lo && (!below || b.base + b.size > below->base + below->size)) below = &b;
                if (b.bound && b.base >= hi && (!above || b.base < above->base)) above = &b;
            }
            for (const TempBinding* b : {below, above})
                if (b)
                    note(std::format("nearest {}: #{} at frame {} type {} [{:#x}, {:#x}) handle {:#x}: {}", b == below ? "below" : "above", b - temp_binds.data(), b->frame, b->type,
                                     b->base, b->base + b->size, b->handle, name(b->handle)));
            note(std::format("{} bindings recorded; this (GPU) thread {} tid {}", temp_binds.size(), std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100000, Gow3::ThreadId()));
        }
        for (auto& v : vendor) note(std::format("vendor '{}' code {:#x} data {:#x}", v.description, v.vendorFaultCode, v.vendorFaultData));
    }
    if (temp_get_checks)
        for (VkQueue q : {queue, xfer, rbq}) {
            if (!q) continue;
            u32 c = 0;
            temp_get_checks(q, &c, nullptr);
            std::vector<VkCheckpointDataNV> d(c, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
            temp_get_checks(q, &c, d.data());
            for (auto& x : d)
                note(std::format("queue {} stage {:#x} checkpoint {:#x}", q == queue ? "main" : q == xfer ? "xfer" : "readback", (u32)x.stage, (u64)x.pCheckpointMarker));
        }
}
} // namespace Gpu
