// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// sceVideodec, the only decoder library the eboot imports, for the cinematics. CINEMATICS/*.BSF are raw H.264 Annex B
// streams (x264 High, 3 B-frames; 1920x1080 in 8 slices, the 4K replacement 3840x2160 in one). The game's decoder thread
// splits them into access units and calls Decode once per unit with a free slot of its frame queue, Flush until no
// picture comes back, Reset after an error. A picture goes to its slot as NV12: luma pitch aligned to 64, chroma after
// pitch * height aligned to 16. The game converts it on the GPU (011f1d20) and picks frames by its movie clock (1/DAT_013cdc80
// per frame, pulled to the audio stream's position), so decoding only has to keep the queue ahead of it.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <format>
#include "gpu/gpu_thread.h"
#include "main/cpu.h"
#include "main/host.h"
#include "orbis/common/util.h"
#include "orbis/internals/error_codes.h"
#include "orbis/internals/libs.h"
#include "orbis/logging/log.h"

namespace Libraries::Videodec {
namespace {

constexpr s32 kApiFail = 0x80C10000, kHandle = 0x80C10003, kFrameBufferSize = 0x80C1000B;

struct ConfigInfo { u64 this_size; u32 codec_type, profile, max_level; s32 max_width, max_height, max_dpb_frames; u64 flags; };
struct ResourceInfo {
    u64 this_size, cpu_memory_size;
    void* cpu_memory;
    u64 cpu_gpu_memory_size;
    void* cpu_gpu_memory;
    u64 max_frame_buffer_size;
    u32 frame_buffer_alignment;
};
struct FrameBuffer { u64 this_size; u8* data; u64 size; };
struct AvcInfo {
    u32 num_units_in_tick, time_scale;
    u8 fixed_frame_rate, aspect_ratio_idc;
    u16 sar_width, sar_height;
    u8 colour_primaries, transfer_characteristics, matrix_coefficients, full_range;
    u32 crop_left, crop_right, crop_top, crop_bottom;
};
struct PictureInfo {
    u64 this_size;
    u32 is_valid, codec_type, frame_width, frame_pitch, frame_height, is_error_pic;
    u64 pts, attached_data;
    union { u8 reserved[64]; AvcInfo avc; } codec;
};
struct InputData { u64 this_size; u8* au_data; u64 au_size, pts, dts, attached_data; };
static_assert(sizeof(ConfigInfo) == 0x28 && sizeof(ResourceInfo) == 0x38 && sizeof(PictureInfo) == 0x70 && sizeof(InputData) == 0x30);

// Planar 4:2:0 into the slot as NV12; the rows of the 16-aligned crop space repeat the picture's last one.
void WriteNV12(u8* __restrict dst, const AVFrame& f, u32 pitch, u32 rows) {
    const u32 width = f.width, height = f.height, chroma_width = (width + 1) / 2, chroma_height = (height + 1) / 2;
    for (u32 y = 0; y < rows; ++y)
        std::memcpy(dst + u64(y) * pitch, f.data[0] + ptrdiff_t(std::min(y, height - 1)) * f.linesize[0], width);
    dst += u64(pitch) * rows;
    for (u32 y = 0; y < rows / 2; ++y) {
        const ptrdiff_t row = std::min(y, chroma_height - 1);
        const u8* __restrict u = f.data[1] + row * f.linesize[1];
        const u8* __restrict v = f.data[2] + row * f.linesize[2];
        u8* __restrict uv = dst + u64(y) * pitch;
        for (u32 x = 0; x < chroma_width; ++x) uv[2 * x] = u[x], uv[2 * x + 1] = v[x];
    }
}

struct Decoder {
    AVCodecContext* context;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool draining = false;
    u64 temp_pictures = 0, temp_calls = 0, temp_ns = 0, temp_max_ns = 0; // TEMP: one summary line per cinematic
    u32 temp_width = 0, temp_height = 0;

    Decoder() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        context = avcodec_alloc_context3(codec);
        context->flags |= AV_CODEC_FLAG_COPY_OPAQUE; // attachedData follows its access unit through the B-frame reordering
        // A frame thread per processor, at most 16 (Cpu::plan): each decodes its own picture, so the single-slice 4K
        // stream scales too. Each costs a picture of latency, which the game's 24-30 slot queue and its final Flush
        // absorb; they live only while a cinematic plays.
        context->thread_type = FF_THREAD_FRAME;
        context->thread_count = int(Gow3::Cpu::plan.decode_threads);
        if (avcodec_open2(context, codec, nullptr) < 0) LOG_ERROR(Lib_Videodec, "Could not open the H.264 decoder");
        Gow3::GpuThread::ResetVideoFrameVersions();
    }
    ~Decoder() {
        Gow3::Log(std::format("TEMP cinematic: {} pictures {}x{}, {} frame threads, {} calls, {:.2f} ms avg, {:.2f} ms max", temp_pictures,
                              temp_width, temp_height, context->thread_count, temp_calls, temp_calls ? temp_ns / 1e6 / temp_calls : 0.0, temp_max_ns / 1e6));
        av_frame_free(&frame), av_packet_free(&packet), avcodec_free_context(&context);
    }
    void TempTime(std::chrono::steady_clock::time_point start) {
        const u64 ns = u64((std::chrono::steady_clock::now() - start).count());
        ++temp_calls, temp_ns += ns, temp_max_ns = std::max(temp_max_ns, ns);
    }

    // The next picture in display order, if one is ready, into the game's slot.
    s32 Receive(FrameBuffer& out, PictureInfo& info) {
        info.is_valid = false;
        const int ret = avcodec_receive_frame(context, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return ORBIS_OK;
        if (ret < 0 || (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P)) {
            LOG_ERROR(Lib_Videodec, "Decoding failed ({}, format {})", ret, ret < 0 ? -1 : frame->format);
            av_frame_unref(frame);
            return kApiFail;
        }
        const u32 width = frame->width, height = frame->height;
        const u32 pitch = Common::AlignUp(width, 64u), rows = Common::AlignUp(height, 16u);
        if (u64(pitch) * rows * 3 / 2 > out.size) {
            LOG_ERROR(Lib_Videodec, "{}x{} picture does not fit a {} byte slot", width, height, out.size);
            av_frame_unref(frame);
            return kFrameBufferSize;
        }
        WriteNV12(out.data, *frame, pitch, rows);
        Gow3::GpuThread::TraceDecodedVideo(u64(out.data), width, height, u64(frame->opaque));
        ++temp_pictures, temp_width = width, temp_height = height;
        info.is_valid = true, info.codec_type = 0, info.is_error_pic = false;
        info.frame_width = Common::AlignUp(width, 16u), info.frame_pitch = pitch, info.frame_height = rows;
        info.pts = frame->pts, info.attached_data = u64(frame->opaque);
        info.codec.avc.colour_primaries = u8(frame->color_primaries);
        info.codec.avc.crop_left = info.codec.avc.crop_top = 0;
        info.codec.avc.crop_right = pitch - width, info.codec.avc.crop_bottom = rows - height;
        av_frame_unref(frame);
        return ORBIS_OK;
    }

    s32 Decode(const InputData& in, FrameBuffer& out, PictureInfo& info) {
        const auto temp_start = std::chrono::steady_clock::now();
        struct TempScope { Decoder* d; std::chrono::steady_clock::time_point t; ~TempScope() { d->TempTime(t); } } temp_scope{this, temp_start};
        if (draining) avcodec_flush_buffers(context), draining = false;
        packet->data = in.au_data, packet->size = int(in.au_size);
        packet->pts = s64(in.pts), packet->dts = s64(in.dts), packet->opaque = reinterpret_cast<void*>(in.attached_data);
        // One access unit in, at most one picture out: the receive below always takes the picture this send produced.
        if (const int ret = avcodec_send_packet(context, packet); ret < 0) {
            LOG_ERROR(Lib_Videodec, "Rejected access unit ({})", ret);
            return kApiFail;
        }
        return Receive(out, info);
    }

    // The pictures still in the frame threads and the reorder buffer, one per call.
    s32 Flush(FrameBuffer& out, PictureInfo& info) {
        if (!draining) avcodec_send_packet(context, nullptr), draining = true;
        return Receive(out, info);
    }
};

struct Ctrl { u64 this_size; Decoder* decoder; u64 version; };

// The decoder runs on the host: the game only maps what this asks for from direct memory (FUN_012e1fe0, rounded to 2 MiB),
// which the console's decoder sized for a full DPB (~130 MiB at 4K).
s32 PS4_SYSV_ABI sceVideodecQueryResourceInfo(const ConfigInfo* config, ResourceInfo* resources) {
    const u64 pitch = Common::AlignUp(u32(std::max(config->max_width, 1920)), 64u);
    const u64 rows = Common::AlignUp(u32(std::max(config->max_height, 1080)), 16u);
    *resources = {sizeof(ResourceInfo), 0, nullptr, 2_MB, nullptr, pitch * rows * 3 / 2, 0x100};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodecCreateDecoder(const ConfigInfo*, const ResourceInfo*, Ctrl* ctrl) {
    *ctrl = {sizeof(Ctrl), new Decoder, 1};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodecDecode(Ctrl* ctrl, const InputData* in, FrameBuffer* out, PictureInfo* info) {
    return ctrl && ctrl->decoder ? ctrl->decoder->Decode(*in, *out, *info) : kHandle;
}

s32 PS4_SYSV_ABI sceVideodecFlush(Ctrl* ctrl, FrameBuffer* out, PictureInfo* info) {
    return ctrl && ctrl->decoder ? ctrl->decoder->Flush(*out, *info) : kHandle;
}

s32 PS4_SYSV_ABI sceVideodecReset(Ctrl* ctrl) {
    if (!ctrl || !ctrl->decoder) return kHandle;
    avcodec_flush_buffers(ctrl->decoder->context), ctrl->decoder->draining = false;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideodecDeleteDecoder(Ctrl* ctrl) {
    if (!ctrl || !ctrl->decoder) return kHandle;
    delete ctrl->decoder, ctrl->decoder = nullptr;
    return ORBIS_OK;
}

} // namespace

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("leCAscipfFY", "libSceVideodec", 1, "libSceVideodec", sceVideodecQueryResourceInfo);
    LIB_FUNCTION("qkgRiwHyheU", "libSceVideodec", 1, "libSceVideodec", sceVideodecCreateDecoder);
    LIB_FUNCTION("q0W5GJMovMs", "libSceVideodec", 1, "libSceVideodec", sceVideodecDecode);
    LIB_FUNCTION("jeigLlKdp5I", "libSceVideodec", 1, "libSceVideodec", sceVideodecFlush);
    LIB_FUNCTION("f8AgDv-1X8A", "libSceVideodec", 1, "libSceVideodec", sceVideodecReset);
    LIB_FUNCTION("U0kpGF1cl90", "libSceVideodec", 1, "libSceVideodec", sceVideodecDeleteDecoder);
}

} // namespace Libraries::Videodec
