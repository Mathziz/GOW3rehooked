// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/video_trace.h"
#include <atomic>
#include <cstdio>
#include <format>
#include <memory>
#include "debug/dump_archive.h"
#include "gpu/gpu_thread.h"
#include "main/host.h"

namespace Gow3::VideoTrace {
namespace {
constexpr size_t capacity = 1 << 16;
template <class T> std::unique_ptr<T[]> Buffer(size_t n) { return enabled ? std::make_unique<T[]>(n) : nullptr; }
struct EventRing {
    std::unique_ptr<Event[]> data = Buffer<Event>(capacity);
    std::atomic<u64> count{};
    void Add(Event e) { const u64 n = count.load(std::memory_order_relaxed); data[n % capacity] = e; count.store(n + 1, std::memory_order_release); }
};
EventRing rings[6];
struct AudioEvent { u64 time, stream, start, schedule; u32 queued, threshold, buffer_size, rate, channels; };
struct AudioSlot { std::atomic<u64> seq{}; AudioEvent event{}; };
std::unique_ptr<AudioSlot[]> audio = Buffer<AudioSlot>(capacity);
std::atomic<u64> audio_count{};
constexpr size_t flow_capacity = 1 << 19;
struct FlowEvent { u64 time, port; s64 value; u32 kind, thread; };
struct FlowSlot { std::atomic<u64> seq{}; FlowEvent event{}; };
std::unique_ptr<FlowSlot[]> flow = Buffer<FlowSlot>(flow_capacity);
std::atomic<u64> flow_count{};
} // namespace

void Add(Ring ring, Event event) { rings[ring].Add(event); }

void Write() {
    if (!enabled) return;
    std::string trace = "time,address,serial,tag,kind,width,height,queue\n";
    for (auto& ring : rings) {
        const u64 end = ring.count.load(std::memory_order_acquire);
        for (u64 i = end > capacity ? end - capacity : 0; i < end; ++i) {
            const auto& e = ring.data[i % capacity];
            trace += std::format("{},{},{},{},{},{},{},{}\n", e.time, e.address, e.serial, e.tag, e.kind, e.width, e.height, e.queue);
        }
    }
    DumpArchive::Add("video-trace.csv", trace);

    std::string audio_csv = "time,stream,start,schedule,queued,threshold,buffer_size,rate,channels\n";
    const u64 end = audio_count.load(std::memory_order_acquire);
    for (u64 i = end > capacity ? end - capacity : 0; i < end; ++i) {
        auto& slot = audio[i % capacity];
        if (slot.seq.load(std::memory_order_acquire) != i + 1) continue;
        const auto& e = slot.event;
        audio_csv += std::format("{},{},{},{},{},{},{},{},{}\n", e.time, e.stream, e.start, e.schedule, e.queued, e.threshold, e.buffer_size, e.rate, e.channels);
    }
    DumpArchive::Add("video-audio-trace.csv", audio_csv);

    std::string flow_csv = "time,port,value,kind,thread\n";
    const u64 flow_end = flow_count.load(std::memory_order_acquire);
    for (u64 i = flow_end > flow_capacity ? flow_end - flow_capacity : 0; i < flow_end; ++i) {
        auto& slot = flow[i % flow_capacity];
        if (slot.seq.load(std::memory_order_acquire) != i + 1) continue;
        const auto& e = slot.event;
        flow_csv += std::format("{},{},{},{},{}\n", e.time, e.port, e.value, e.kind, e.thread);
    }
    DumpArchive::Add("video-audio-flow.csv", flow_csv);
}
} // namespace Gow3::VideoTrace

namespace Gow3::GpuThread {
void TraceVideoCopy(u64 address, u64 frame, bool end) {
    if (VideoTrace::enabled) VideoTrace::Add(VideoTrace::Copies, {DiagClock(), address, VideoFrameVersion(address), frame, end ? 6u : 5u, 0, 0, 0});
}
bool VideoTraceEnabled() { return VideoTrace::enabled; }
u64 VideoTraceClock() { return DiagClock(); }
void TraceAudioFlow(u32 kind, u64 port, s64 value) {
    if (!VideoTrace::enabled) return;
    using namespace VideoTrace;
    const u64 i = flow_count.fetch_add(1, std::memory_order_relaxed);
    auto& slot = flow[i % flow_capacity];
    slot.event = {DiagClock(), port, value, kind, Gow3::ThreadId()};
    slot.seq.store(i + 1, std::memory_order_release);
}
void TraceVideoReadback(u64 address, u64 size, u64 frame, bool skipped) {
    if (VideoTrace::enabled) VideoTrace::Add(VideoTrace::Readbacks, {DiagClock(), address, size, frame, skipped ? 12u : 11u, 0, 0, 0});
}
void TraceAudioQueue(u64 stream, u64 start, u64 schedule, u32 queued, u32 threshold, u32 buffer_size, u32 rate, u32 channels) {
    if (!VideoTrace::enabled) return;
    using namespace VideoTrace;
    const u64 n = audio_count.fetch_add(1, std::memory_order_relaxed);
    auto& slot = audio[n % capacity];
    slot.event = {DiagClock(), stream, start, schedule, queued, threshold, buffer_size, rate, channels};
    slot.seq.store(n + 1, std::memory_order_release);
}
} // namespace Gow3::GpuThread
