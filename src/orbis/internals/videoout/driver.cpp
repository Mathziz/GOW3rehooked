// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "orbis/common/assert.h"
#include "orbis/common/debug.h"
#include "orbis/common/thread.h"
#include "main/config.h"
#include <immintrin.h>
#include "orbis/core/debug_state.h"
#include "orbis/core/emulator_settings.h"
#include "orbis/internals/kernel/time.h"
#include "orbis/internals/videoout/driver.h"
#include "orbis/internals/videoout/videoout_error.h"
#include "present/presenter.h"
#include "gpu/gpu_thread.h"
#include "debug/debug.h"

namespace Libraries::VideoOut {

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    vblank_thread = std::jthread([this](const std::stop_token& token) { VblankThread(token); });
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* /*params*/) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    return 1;
}

void VideoOutDriver::Close(s32 /*handle*/) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (static_cast<size_t>(group_index) >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (static_cast<size_t>(startIndex + bufferNum) > MaxDisplayBuffers || static_cast<size_t>(startIndex) > MaxDisplayBuffers ||
        static_cast<size_t>(bufferNum) > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < static_cast<u32>(bufferNum); i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };

        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (static_cast<size_t>(attributeIndex) >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (static_cast<size_t>(attributeIndex) >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

void VideoOutDriver::CompleteFlip(const Request& req) {
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
    }

    // A CPU flip takes effect at the next vblank, unordered with GPU work (as on the console): queued here, not behind the
    // GPU thread's current frame. The game flips after the frame's EOP, so the frame is already rendered.
    SubmitFlipInternal(port, index, flip_arg, is_eop);

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    ASSERT_MSG(index == -1 || port->buffer_slots[index].group_index >= 0, "Trying to flip an unregistered buffer!");
    {
        std::scoped_lock lock{mutex};
        requests.push({
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
            .frame_id = Gow3::Presenter::FrameForFlip(),
        });
    }
    Common::InterruptSleep(vblank_thread.native_handle());
}


std::atomic<u64> temp_vblank_loops{0}, temp_vblank_flips{0};  // debug: read by the GPU thread's per-second line

void VideoOutDriver::VblankThread(const std::stop_token& token) {
    using namespace std::chrono;
    const nanoseconds period{Gow3::FramePeriodNs(EmulatorSettings.GetVblankFrequency())};
    Common::SetCurrentThreadName("GOW3 vblank");
    Common::SetCurrentThreadRealtime(period);
    auto deadline = steady_clock::now(), last_flip = deadline - period;
    u32 flip_us = 0, event_us = 0;  // debug: how long the previous pass spent completing its flip and on the vblank events
    const auto us = [](steady_clock::time_point a, steady_clock::time_point b) { return u32(std::min<s64>(duration_cast<microseconds>(b - a).count(), 0xFFFFFFFF)); };
    while (!token.stop_requested()) {
        constexpr auto spin = 150us; // covers the high-resolution timer's wake-up latency
        // At flip rate 0 a flip completes once submitted and a vblank period after the previous one: a display that
        // follows the frames (variable refresh) gets them at the game's own cadence, not on the grid of guest vblanks,
        // which still tick for the game. A submit ends the sleep.
        const bool on_submit = main_port.flip_rate.load() == 0;
        auto wake = deadline;
        if (on_submit) {
            std::scoped_lock lock(mutex);
            if (!requests.empty()) wake = std::min(wake, last_flip + period);
        }
        const auto planned = Gow3::DiagNow();
        if (const auto left = wake - steady_clock::now();
            left > spin && !Common::AccurateSleep(duration_cast<nanoseconds>(left - spin), nullptr, on_submit))
            continue; // a flip was submitted: plan again
        while (steady_clock::now() < wake && !token.stop_requested()) _mm_pause();
        if (token.stop_requested()) return;
        const auto now = steady_clock::now();
        const bool tick = now >= deadline;
        auto& status = main_port.vblank_status;
        // The oldest pending flip completes at its vblank; the presentation thread shows its buffer.
        if (!DebugState.IsGuestThreadsPaused() &&
            (on_submit ? now >= last_flip + period : tick && status.count % (main_port.flip_rate.load() + 1) == 0)) {
            std::unique_lock lock(mutex);
            if (!requests.empty()) {
                const auto request = requests.front();
                requests.pop();
                const u32 pending = u32(requests.size());
                lock.unlock();
                last_flip = now;
                Gow3::GpuThread::RecordFlip(duration_cast<nanoseconds>(now - wake).count(), pending, planned > wake ? us(wake, planned) : 0, flip_us, event_us);
                temp_vblank_flips.fetch_add(1, std::memory_order_relaxed);  // TEMP perf
                const auto flip_start = Gow3::DiagNow();
                CompleteFlip(request);
                if (request.index >= 0) {
                    const auto& slot = request.port->buffer_slots[request.index];
                    // A8R8G8B8: bytes B, G, R, A in memory, the order the renderer's image holds.
                    Gow3::Presenter::Flipped(slot.address_left, slot.group_index >= 0 &&
                        request.port->groups[slot.group_index].attrib.pixel_format == PixelFormat::A8R8G8B8Srgb, request.frame_id);
                } else {
                    Gow3::Presenter::Flipped(0, false, request.frame_id);
                }
                FRAME_END;
                flip_us = us(flip_start, Gow3::DiagNow());
            }
        }
        if (!tick) continue;
        const auto event_start = Gow3::DiagNow();
        temp_vblank_loops.fetch_add(1, std::memory_order_relaxed);  // TEMP perf
        const u64 ticks = 1 + u64(std::max<s64>(0, duration_cast<nanoseconds>(now - deadline).count()) / period.count());
        {
            std::scoped_lock lock(main_port.vo_mutex);
            status.count += ticks;
            status.process_time = Kernel::sceKernelGetProcessTime();
            status.tsc = Kernel::sceKernelReadTsc();
            for (auto event : main_port.vblank_events)
                if (auto equeue = Kernel::GetEqueue(event)) equeue->TriggerEvent(
                    static_cast<u64>(OrbisVideoOutInternalEventId::Vblank), Kernel::OrbisKernelEvent::Filter::VideoOut,
                    reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) | (status.count << 16)));
            main_port.vblank_cv.notify_all();
        }
        event_us = us(event_start, Gow3::DiagNow());
        deadline += period * ticks + nanoseconds(std::clamp(Gow3::Presenter::TakeVblankCorrection(), -period.count() / 4, period.count() / 4));
    }
}
} // namespace Libraries::VideoOut
