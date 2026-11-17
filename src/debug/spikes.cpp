// SPDX-License-Identifier: GPL-2.0-or-later
// Debug: events (a host call, a lock wait) that lasted longer than their threshold, reported with the frame line.
#include "debug/debug.h"
#include <algorithm>
#include <format>
#include <mutex>
#include <string_view>
#include <vector>

namespace Gow3 {
namespace {
struct SpikeEvent {
    const char* what;
    std::chrono::steady_clock::time_point start;
    double ms;
    uint64_t detail;
};
std::mutex spike_mutex;
std::vector<SpikeEvent> spikes;
size_t dropped_spikes{};
}

// Host call wrappers carry the function in their signature: "...HostCallWrapperImpl<&Ns::func>::wrap(...)".
std::string_view HostCallName(std::string_view what) {
    if (const auto b = what.find("<&"), e = what.find(">::wrap"); b != what.npos && e != what.npos && e > b)
        return what.substr(b + 2, e - b - 2);
    return what;
}

void Spike(const char* what, std::chrono::steady_clock::time_point start, double threshold_ms, uint64_t detail) {
    if (!DebugEnabled()) return;
    const double ms = DiagMs(start, std::chrono::steady_clock::now());
    if (ms < threshold_ms) return;
    std::scoped_lock lock(spike_mutex);
    if (spikes.size() < 64) spikes.push_back({what, start, ms, detail});
    else ++dropped_spikes;
}

std::string TakeSpikes(std::chrono::steady_clock::time_point since) {
    std::vector<SpikeEvent> taken;
    size_t dropped{};
    {
        std::scoped_lock lock(spike_mutex);
        taken.swap(spikes);
        dropped = std::exchange(dropped_spikes, 0);
    }
    if (taken.empty()) return {};
    std::ranges::sort(taken, {}, &SpikeEvent::start);
    std::string out = "Spikes:";
    for (const auto& s : taken) {
        out += std::format(" {:+.1f} {} {:.1f}ms", DiagMs(since, s.start), HostCallName(s.what), s.ms);
        if (s.detail) out += std::format(" {:#x}", s.detail);
        out += ';';
    }
    if (dropped) out += std::format(" {} more", dropped);
    return out;
}
} // namespace Gow3
