// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "orbis/common/types.h"
#include "orbis/logging/log.h"

#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())

enum HideCursorState : int {
    Never,
    Idle,
    Always,
};

enum UsbBackendType : int {
    Real,
    SkylandersPortal,
    InfinityBase,
    DimensionsToypad,
};

enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

enum class ConfigMode {
    Default,
    Global,
    Clean,
};

enum AudioBackend : int {
    SDL,
    OpenAL,
};

enum OpenALHrtfMode : int {
    HrtfAuto,
    HrtfOn,
    HrtfOff,
};

enum OpenALOutputMode : int {
    OutputAuto,
    OutputStereo,
    OutputQuad,
    OutputSurround51,
    OutputSurround71,
};

struct GameInstallDir {
    std::filesystem::path path;
    bool enabled{true};
};

struct GeneralSettings {
    std::vector<GameInstallDir> install_dirs;
    std::filesystem::path addon_install_dir;
    std::filesystem::path home_dir;
    std::filesystem::path sys_modules_dir;
    std::filesystem::path font_dir;

    int volume_slider{100};
    bool neo_mode{false};
    bool dev_kit_mode{false};
    int extra_dmem_in_mbytes{0};
    int extra_fmem_in_mbytes{0};
    bool trophy_popup_disabled{false};
    double trophy_notification_duration{6.0};
    std::string trophy_notification_side{"right"};
    bool show_splash{false};
    bool discord_rpc_enabled{false};
    bool show_fps_counter{false};
    int console_language{1};
    int big_picture_scale{1000};
    int big_picture_folder_depth{2};
    bool redzone_patches{false};
};

struct NetworkSettings {
    bool connected_to_network{false};
    bool shad_net_enabled{false};
    std::string shadnet_server{"srv.shadps4.net:31313"};
    std::string shadnet_webapi_server{"http://srv.shadps4.net:31315"};
    std::string signaling_info{};
    bool enable_upnp{true};
    int p2p_port{3658};
    bool disable_https{false};
};

struct LogSettings {
    bool append{false};
    bool enable{true};
    std::string filter{""};
    std::string flush_level{""};
    u32 max_skip_duration{5000};
    bool separate{false};
    unsigned long long size_limit{100ULL * 1024 * 1024};
    bool skip_duplicate{true};
    bool sync{true};
#ifdef _WIN32
    std::string type{"wincolor"};
#endif
};

struct DebugSettings {
    bool debug_dump{false};
    bool shader_collect{false};
    std::string config_version{""};
};

struct InputSettings {
    int cursor_state{HideCursorState::Idle};
    int cursor_hide_timeout{5};
    int usb_device_backend{UsbBackendType::Real};
    bool use_special_pad{false};
    int special_pad_class{1};
    bool motion_controls_enabled{true};
    bool use_unified_input_config{true};
    std::string default_controller_id{""};
    bool background_controller_input{false};
    bool ime_accessibility_enabled{false};
    bool ime_url_mail_short_panel{false};
    bool is_circle_enter{false};
    s32 camera_id{-1};
    bool use_mice_as_mice{false};
    bool use_keyboard_as_keyboard{false};
};

struct AudioSettings {
    u32 audio_backend{AudioBackend::SDL};
    std::string sdl_mic_device{"Default Device"};
    std::string sdl_main_output_device{"Default Device"};
    std::string sdl_padSpk_output_device{"Default Device"};
    std::string openal_mic_device{"Default Device"};
    std::string openal_main_output_device{"Default Device"};
    std::string openal_padSpk_output_device{"Default Device"};
    u32 openal_hrtf{OpenALHrtfMode::HrtfAuto};
    u32 openal_output_mode{OpenALOutputMode::OutputAuto};
};

struct GPUSettings {
    u32 window_width{1280};
    u32 window_height{720};
    u32 internal_screen_width{1280};
    u32 internal_screen_height{720};
    bool null_gpu{false};
    bool copy_gpu_buffers{false};
    u32 readbacks_mode{GpuReadbacksMode::Disabled};
    bool readback_linear_images_enabled{false};
    bool direct_memory_access_enabled{false};
    bool dump_shaders{false};
    bool patch_shaders{false};
    u32 vblank_frequency{60};
    bool full_screen{false};
    std::string full_screen_mode{"Windowed"};
    std::string present_mode{"Mailbox"};
    bool hdr_allowed{false};
    bool fsr_enabled{false};
    bool rcas_enabled{true};
    int rcas_attenuation{250};
    bool userfaultfd{false};
    bool inline_fetch_shader{false};
};

struct VulkanSettings {
    s32 gpu_id{-1};
    bool renderdoc_enabled{false};
    bool vkvalidation_enabled{false};
    bool vkvalidation_core_enabled{true};
    bool vkvalidation_sync_enabled{false};
    bool vkvalidation_gpu_enabled{false};
    bool vkcrash_diagnostic_enabled{false};
    bool vkhost_markers{false};
    bool vkguest_markers{false};
    bool pipeline_cache_enabled{false};
    bool pipeline_cache_archived{false};
};

class EmulatorSettingsImpl {
public:
    EmulatorSettingsImpl();
    ~EmulatorSettingsImpl();

    static std::shared_ptr<EmulatorSettingsImpl> GetInstance();
    static void SetInstance(std::shared_ptr<EmulatorSettingsImpl> instance);

    bool Save(const std::string& = "") { return true; }
    bool Load(const std::string& = "") { return true; }
    void SetDefaultValues();

    ConfigMode GetConfigMode() const { return m_configMode; }
    void SetConfigMode(ConfigMode mode) { m_configMode = mode; }
    void ClearGameSpecificOverrides() {}

    // general accessors
    bool AddGameInstallDir(const std::filesystem::path& dir, bool enabled = true);
    std::vector<std::filesystem::path> GetGameInstallDirs() const;
    void SetAllGameInstallDirs(const std::vector<GameInstallDir>& dirs);
    void RemoveGameInstallDir(const std::filesystem::path& dir);
    void SetGameInstallDirEnabled(const std::filesystem::path& dir, bool enabled);
    void SetGameInstallDirs(const std::vector<std::filesystem::path>& dirs_config);
    const std::vector<bool> GetGameInstallDirsEnabled();
    const std::vector<GameInstallDir>& GetAllGameInstallDirs() const;

    std::filesystem::path GetHomeDir();
    void SetHomeDir(const std::filesystem::path& dir);
    std::filesystem::path GetSysModulesDir();
    void SetSysModulesDir(const std::filesystem::path& dir);
    std::filesystem::path GetFontsDir();
    void SetFontsDir(const std::filesystem::path& dir);
    std::filesystem::path GetAddonInstallDir();
    void SetAddonInstallDir(const std::filesystem::path& dir);

    // General settings
    int GetVolumeSlider() const { return m_general.volume_slider; }
    void SetVolumeSlider(int v, bool = false) { m_general.volume_slider = v; }
    bool IsNeo() const { return m_general.neo_mode; }
    void SetNeo(bool v, bool = false) { m_general.neo_mode = v; }
    bool IsDevKit() const { return m_general.dev_kit_mode; }
    void SetDevKit(bool v, bool = false) { m_general.dev_kit_mode = v; }
    int GetExtraDmemInMBytes() const { return m_general.extra_dmem_in_mbytes; }
    void SetExtraDmemInMBytes(int v, bool = false) { m_general.extra_dmem_in_mbytes = v; }
    int GetExtraFmemInMBytes() const { return m_general.extra_fmem_in_mbytes; }
    void SetExtraFmemInMBytes(int v, bool = false) { m_general.extra_fmem_in_mbytes = v; }

    bool IsShadNetEnabled() const {
        return m_network.shad_net_enabled && !m_shadnet_session_disabled.load(std::memory_order_relaxed);
    }
    void SetShadNetEnabled(bool v, bool = false) { m_network.shad_net_enabled = v; }
    bool IsShadNetEnabledSetting() const { return m_network.shad_net_enabled; }
    void SetShadNetSessionDisabled(bool v) { m_shadnet_session_disabled.store(v, std::memory_order_relaxed); }
    bool IsShadNetSessionDisabled() const { return m_shadnet_session_disabled.load(std::memory_order_relaxed); }

    bool IsTrophyPopupDisabled() const { return m_general.trophy_popup_disabled; }
    void SetTrophyPopupDisabled(bool v, bool = false) { m_general.trophy_popup_disabled = v; }
    double GetTrophyNotificationDuration() const { return m_general.trophy_notification_duration; }
    void SetTrophyNotificationDuration(double v, bool = false) { m_general.trophy_notification_duration = v; }
    std::string GetTrophyNotificationSide() const { return m_general.trophy_notification_side; }
    void SetTrophyNotificationSide(const std::string& v, bool = false) { m_general.trophy_notification_side = v; }
    bool IsShowSplash() const { return m_general.show_splash; }
    void SetShowSplash(bool v, bool = false) { m_general.show_splash = v; }
    bool IsDiscordRPCEnabled() const { return m_general.discord_rpc_enabled; }
    void SetDiscordRPCEnabled(bool v, bool = false) { m_general.discord_rpc_enabled = v; }
    bool IsShowFpsCounter() const { return m_general.show_fps_counter; }
    void SetShowFpsCounter(bool v, bool = false) { m_general.show_fps_counter = v; }
    int GetConsoleLanguage() const { return m_general.console_language; }
    void SetConsoleLanguage(int v, bool = false) { m_general.console_language = v; }
    int GetBigPictureScale() const { return m_general.big_picture_scale; }
    void SetBigPictureScale(int v, bool = false) { m_general.big_picture_scale = v; }
    int GetBigPictureFolderDepth() const { return m_general.big_picture_folder_depth; }
    void SetBigPictureFolderDepth(int v, bool = false) { m_general.big_picture_folder_depth = v; }
    bool IsRedZonePatchingEnabled() const { return m_general.redzone_patches; }
    void SetRedZonePatchingEnabled(bool v, bool = false) { m_general.redzone_patches = v; }

    // Network settings
    bool IsConnectedToNetwork() const { return m_network.connected_to_network; }
    void SetConnectedToNetwork(bool v, bool = false) { m_network.connected_to_network = v; }
    std::string GetShadNetServer() const { return m_network.shadnet_server; }
    void SetShadNetServer(const std::string& v, bool = false) { m_network.shadnet_server = v; }
    std::string GetShadNetWebApiServer() const { return m_network.shadnet_webapi_server; }
    void SetShadNetWebApiServer(const std::string& v, bool = false) { m_network.shadnet_webapi_server = v; }
    std::string GetSignalingInfo() const { return m_network.signaling_info; }
    void SetSignalingInfo(const std::string& v, bool = false) { m_network.signaling_info = v; }
    bool IsUPnPEnabled() const { return m_network.enable_upnp; }
    void SetUPnPEnabled(bool v, bool = false) { m_network.enable_upnp = v; }
    int GetP2PPort() const { return m_network.p2p_port; }
    void SetP2PPort(int v, bool = false) { m_network.p2p_port = v; }
    bool IsForcedHttpsDisabled() const { return m_network.disable_https; }
    void SetForcedHttpsDisabled(bool v, bool = false) { m_network.disable_https = v; }

    // Log settings
    bool IsLogAppend() const { return m_log.append; }
    void SetLogAppend(bool v, bool = false) { m_log.append = v; }
    bool IsLogEnable() const { return m_log.enable; }
    void SetLogEnable(bool v, bool = false) { m_log.enable = v; }
    std::string GetLogFilter() const { return m_log.filter; }
    void SetLogFilter(const std::string& v, bool = false) { m_log.filter = v; }
    std::string GetLogFlushLevel() const { return m_log.flush_level; }
    void SetLogFlushLevel(const std::string& v, bool = false) { m_log.flush_level = v; }
    u32 GetLogMaxSkipDuration() const { return m_log.max_skip_duration; }
    void SetLogMaxSkipDuration(u32 v, bool = false) { m_log.max_skip_duration = v; }
    bool IsLogSeparate() const { return m_log.separate; }
    void SetLogSeparate(bool v, bool = false) { m_log.separate = v; }
    unsigned long long GetLogSizeLimit() const { return m_log.size_limit; }
    void SetLogSizeLimit(unsigned long long v, bool = false) { m_log.size_limit = v; }
    bool IsLogSkipDuplicate() const { return m_log.skip_duplicate; }
    void SetLogSkipDuplicate(bool v, bool = false) { m_log.skip_duplicate = v; }
    bool IsLogSync() const { return m_log.sync; }
    void SetLogSync(bool v, bool = false) { m_log.sync = v; }
#ifdef _WIN32
    std::string GetLogType() const { return m_log.type; }
    void SetLogType(const std::string& v, bool = false) { m_log.type = v; }
#endif

    // Audio settings
    u32 GetAudioBackend() const { return m_audio.audio_backend; }
    void SetAudioBackend(u32 v, bool = false) { m_audio.audio_backend = v; }
    std::string GetSDLMicDevice() const { return m_audio.sdl_mic_device; }
    void SetSDLMicDevice(const std::string& v, bool = false) { m_audio.sdl_mic_device = v; }
    std::string GetSDLMainOutputDevice() const { return m_audio.sdl_main_output_device; }
    void SetSDLMainOutputDevice(const std::string& v, bool = false) { m_audio.sdl_main_output_device = v; }
    std::string GetSDLPadSpkOutputDevice() const { return m_audio.sdl_padSpk_output_device; }
    void SetSDLPadSpkOutputDevice(const std::string& v, bool = false) { m_audio.sdl_padSpk_output_device = v; }
    std::string GetOpenALMicDevice() const { return m_audio.openal_mic_device; }
    void SetOpenALMicDevice(const std::string& v, bool = false) { m_audio.openal_mic_device = v; }
    std::string GetOpenALMainOutputDevice() const { return m_audio.openal_main_output_device; }
    void SetOpenALMainOutputDevice(const std::string& v, bool = false) { m_audio.openal_main_output_device = v; }
    std::string GetOpenALPadSpkOutputDevice() const { return m_audio.openal_padSpk_output_device; }
    void SetOpenALPadSpkOutputDevice(const std::string& v, bool = false) { m_audio.openal_padSpk_output_device = v; }
    u32 GetOpenALHrtf() const { return m_audio.openal_hrtf; }
    void SetOpenALHrtf(u32 v, bool = false) { m_audio.openal_hrtf = v; }
    u32 GetOpenALOutputMode() const { return m_audio.openal_output_mode; }
    void SetOpenALOutputMode(u32 v, bool = false) { m_audio.openal_output_mode = v; }

    // Debug settings
    bool IsDebugDump() const { return m_debug.debug_dump; }
    void SetDebugDump(bool v, bool = false) { m_debug.debug_dump = v; }
    bool IsShaderCollect() const { return m_debug.shader_collect; }
    void SetShaderCollect(bool v, bool = false) { m_debug.shader_collect = v; }
    std::string GetConfigVersion() const { return m_debug.config_version; }
    void SetConfigVersion(const std::string& v, bool = false) { m_debug.config_version = v; }

    // GPU Settings
    bool IsNullGPU() const { return m_gpu.null_gpu; }
    void SetNullGPU(bool v, bool = false) { m_gpu.null_gpu = v; }
    bool IsDumpShaders() const { return m_gpu.dump_shaders; }
    void SetDumpShaders(bool v, bool = false) { m_gpu.dump_shaders = v; }
    bool IsCopyGpuBuffers() const { return m_gpu.copy_gpu_buffers; }
    void SetCopyGpuBuffers(bool v, bool = false) { m_gpu.copy_gpu_buffers = v; }
    bool IsFullScreen() const { return m_gpu.full_screen; }
    void SetFullScreen(bool v, bool = false) { m_gpu.full_screen = v; }
    std::string GetFullScreenMode() const { return m_gpu.full_screen_mode; }
    void SetFullScreenMode(const std::string& v, bool = false) { m_gpu.full_screen_mode = v; }
    std::string GetPresentMode() const { return m_gpu.present_mode; }
    void SetPresentMode(const std::string& v, bool = false) { m_gpu.present_mode = v; }
    u32 GetWindowHeight() const { return m_gpu.window_height; }
    void SetWindowHeight(u32 v, bool = false) { m_gpu.window_height = v; }
    u32 GetWindowWidth() const { return m_gpu.window_width; }
    void SetWindowWidth(u32 v, bool = false) { m_gpu.window_width = v; }
    u32 GetInternalScreenHeight() const { return m_gpu.internal_screen_height; }
    void SetInternalScreenHeight(u32 v, bool = false) { m_gpu.internal_screen_height = v; }
    u32 GetInternalScreenWidth() const { return m_gpu.internal_screen_width; }
    void SetInternalScreenWidth(u32 v, bool = false) { m_gpu.internal_screen_width = v; }
    bool IsHdrAllowed() const { return m_gpu.hdr_allowed; }
    void SetHdrAllowed(bool v, bool = false) { m_gpu.hdr_allowed = v; }
    bool IsFsrEnabled() const { return m_gpu.fsr_enabled; }
    void SetFsrEnabled(bool v, bool = false) { m_gpu.fsr_enabled = v; }
    bool IsRcasEnabled() const { return m_gpu.rcas_enabled; }
    void SetRcasEnabled(bool v, bool = false) { m_gpu.rcas_enabled = v; }
    int GetRcasAttenuation() const { return m_gpu.rcas_attenuation; }
    void SetRcasAttenuation(int v, bool = false) { m_gpu.rcas_attenuation = v; }
    u32 GetReadbacksMode() const { return m_gpu.readbacks_mode; }
    void SetReadbacksMode(u32 v, bool = false) { m_gpu.readbacks_mode = v; }
    bool IsReadbackLinearImagesEnabled() const { return m_gpu.readback_linear_images_enabled; }
    void SetReadbackLinearImagesEnabled(bool v, bool = false) { m_gpu.readback_linear_images_enabled = v; }
    bool DirectMemoryAccessEnabled() const { return m_gpu.direct_memory_access_enabled; }
    bool IsDirectMemoryAccessEnabled() const { return m_gpu.direct_memory_access_enabled; }
    void SetDirectMemoryAccessEnabled(bool v, bool = false) { m_gpu.direct_memory_access_enabled = v; }
    bool IsPatchShaders() const { return m_gpu.patch_shaders; }
    bool IsUserfaultfdTracking() const { return m_gpu.userfaultfd; }
    void SetUserfaultfdTracking(bool v, bool = false) { m_gpu.userfaultfd = v; }
    bool IsInlineFetchShader() const { return m_gpu.inline_fetch_shader; }

    u32 GetVblankFrequency() const {
        return m_gpu.vblank_frequency < 30 ? 30 : m_gpu.vblank_frequency;
    }
    void SetVblankFrequency(const u32& v, bool = false) {
        m_gpu.vblank_frequency = v < 30 ? 30 : v;
    }

    // Input Settings
    int GetCursorState() const { return m_input.cursor_state; }
    void SetCursorState(int v, bool = false) { m_input.cursor_state = v; }
    int GetCursorHideTimeout() const { return m_input.cursor_hide_timeout; }
    void SetCursorHideTimeout(int v, bool = false) { m_input.cursor_hide_timeout = v; }
    int GetUsbDeviceBackend() const { return m_input.usb_device_backend; }
    void SetUsbDeviceBackend(int v, bool = false) { m_input.usb_device_backend = v; }
    bool IsMotionControlsEnabled() const { return m_input.motion_controls_enabled; }
    void SetMotionControlsEnabled(bool v, bool = false) { m_input.motion_controls_enabled = v; }
    bool IsBackgroundControllerInput() const { return m_input.background_controller_input; }
    void SetBackgroundControllerInput(bool v, bool = false) { m_input.background_controller_input = v; }
    bool IsImeAccessibilityEnabled() const { return m_input.ime_accessibility_enabled; }
    void SetImeAccessibilityEnabled(bool v, bool = false) { m_input.ime_accessibility_enabled = v; }
    bool IsImeUrlMailShortPanel() const { return m_input.ime_url_mail_short_panel; }
    void SetImeUrlMailShortPanel(bool v, bool = false) { m_input.ime_url_mail_short_panel = v; }
    std::string GetDefaultControllerId() const { return m_input.default_controller_id; }
    void SetDefaultControllerId(const std::string& v, bool = false) { m_input.default_controller_id = v; }
    bool IsUsingSpecialPad() const { return m_input.use_special_pad; }
    void SetUsingSpecialPad(bool v, bool = false) { m_input.use_special_pad = v; }
    int GetSpecialPadClass() const { return m_input.special_pad_class; }
    void SetSpecialPadClass(int v, bool = false) { m_input.special_pad_class = v; }
    bool IsUseUnifiedInputConfig() const { return m_input.use_unified_input_config; }
    void SetUseUnifiedInputConfig(bool v, bool = false) { m_input.use_unified_input_config = v; }
    s32 GetCameraId() const { return m_input.camera_id; }
    void SetCameraId(s32 v, bool = false) { m_input.camera_id = v; }
    bool IsCircleEnter() const { return m_input.is_circle_enter; }
    void SetCircleEnter(bool v, bool = false) { m_input.is_circle_enter = v; }
    bool IsMiceUsedAsMice() const { return m_input.use_mice_as_mice; }
    void SetMiceUsedAsMice(bool v, bool = false) { m_input.use_mice_as_mice = v; }
    bool IsKeyboardUsedAsKeyboard() const { return m_input.use_keyboard_as_keyboard; }
    void SetKeyboardUsedAsKeyboard(bool v, bool = false) { m_input.use_keyboard_as_keyboard = v; }

    // Vulkan settings
    s32 GetGpuId() const { return m_vulkan.gpu_id; }
    void SetGpuId(s32 v, bool = false) { m_vulkan.gpu_id = v; }
    bool IsRenderdocEnabled() const { return m_vulkan.renderdoc_enabled; }
    void SetRenderdocEnabled(bool v, bool = false) { m_vulkan.renderdoc_enabled = v; }
    bool IsVkValidationEnabled() const { return m_vulkan.vkvalidation_enabled; }
    void SetVkValidationEnabled(bool v, bool = false) { m_vulkan.vkvalidation_enabled = v; }
    bool IsVkValidationCoreEnabled() const { return m_vulkan.vkvalidation_core_enabled; }
    void SetVkValidationCoreEnabled(bool v, bool = false) { m_vulkan.vkvalidation_core_enabled = v; }
    bool IsVkValidationSyncEnabled() const { return m_vulkan.vkvalidation_sync_enabled; }
    void SetVkValidationSyncEnabled(bool v, bool = false) { m_vulkan.vkvalidation_sync_enabled = v; }
    bool IsVkValidationGpuEnabled() const { return m_vulkan.vkvalidation_gpu_enabled; }
    void SetVkValidationGpuEnabled(bool v, bool = false) { m_vulkan.vkvalidation_gpu_enabled = v; }
    bool IsVkCrashDiagnosticEnabled() const { return m_vulkan.vkcrash_diagnostic_enabled; }
    void SetVkCrashDiagnosticEnabled(bool v, bool = false) { m_vulkan.vkcrash_diagnostic_enabled = v; }
    bool IsVkHostMarkersEnabled() const { return m_vulkan.vkhost_markers; }
    void SetVkHostMarkersEnabled(bool v, bool = false) { m_vulkan.vkhost_markers = v; }
    bool IsVkGuestMarkersEnabled() const { return m_vulkan.vkguest_markers; }
    void SetVkGuestMarkersEnabled(bool v, bool = false) { m_vulkan.vkguest_markers = v; }
    bool IsPipelineCacheEnabled() const { return m_vulkan.pipeline_cache_enabled; }
    void SetPipelineCacheEnabled(bool v, bool = false) { m_vulkan.pipeline_cache_enabled = v; }
    bool IsPipelineCacheArchived() const { return m_vulkan.pipeline_cache_archived; }
    void SetPipelineCacheArchived(bool v, bool = false) { m_vulkan.pipeline_cache_archived = v; }

private:
    GeneralSettings m_general{};
    NetworkSettings m_network{};
    LogSettings m_log{};
    DebugSettings m_debug{};
    InputSettings m_input{};
    AudioSettings m_audio{};
    GPUSettings m_gpu{};
    VulkanSettings m_vulkan{};
    ConfigMode m_configMode{ConfigMode::Default};

    std::atomic<bool> m_shadnet_session_disabled{false};

    static std::shared_ptr<EmulatorSettingsImpl> s_instance;
    static std::mutex s_mutex;
};
