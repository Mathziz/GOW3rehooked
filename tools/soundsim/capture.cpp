// SPDX-License-Identifier: GPL-2.0-or-later
#include "capture.h"
#include "orbis/common/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace SoundSim {
namespace {
struct BlockSummary {
    u64 first{}, first_signal{}, last_cursor{};
    size_t count{}, signal{}, gated{};
    double sum_squares{};
    u64 frames{};
    float peak{};

    void Add(u64 time, u64 cursor, u32 samples, float rms, float block_peak, bool gate) {
        if (!count) first = time;
        ++count;
        if (block_peak > 1.e-6f && !signal++) first_signal = time;
        gated += gate;
        last_cursor = cursor;
        frames += samples;
        sum_squares += double(rms) * rms * samples;
        peak = std::max(peak, block_peak);
    }

    void Write(const char* stage, u64 load) const {
        std::cout << "  " << stage << " blocks=" << count << " signal=" << signal << " gated=" << gated
                  << " cursor=" << last_cursor << " rms=" << (frames ? std::sqrt(sum_squares / double(frames)) : 0.)
                  << " peak=" << peak;
        if (signal) std::cout << " first-signal=" << double(s64(first_signal) - s64(load ? load : first)) / 1.e6 << " ms";
        std::cout << '\n';
    }
};

template <typename T>
T Read(std::istream& in, u64 offset) {
    T value;
    if (!in.seekg(offset).read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("Truncated WAV file");
    return value;
}

std::vector<std::array<double, 2>> StereoEnvelope(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open WAV file");
    constexpr u32 Riff = 0x46464952, Wave = 0x45564157, Fmt = 0x20746d66, Data = 0x61746164;
    if (Read<u32>(in, 0) != Riff || Read<u32>(in, 8) != Wave)
        throw std::runtime_error("Expected a RIFF/WAVE file");
    const u64 end = u64(Read<u32>(in, 4)) + 8;
    in.seekg(0, std::ios::end);
    if (end > u64(in.tellg())) throw std::runtime_error("Truncated WAV file");
    u16 format{}, channels{}, bits{}, block_align{};
    u32 sample_rate{}, size{};
    u64 data{};
    for (u64 offset = 12; offset + 8 <= end;) {
        const u32 tag = Read<u32>(in, offset);
        const u32 length = Read<u32>(in, offset + 4);
        offset += 8;
        if (length > end - offset) throw std::runtime_error("Truncated WAV chunk");
        if (tag == Fmt && length >= 16) {
            format = Read<u16>(in, offset);
            if (format == 0xfffe && length >= 40) format = Read<u16>(in, offset + 24);
            channels = Read<u16>(in, offset + 2);
            sample_rate = Read<u32>(in, offset + 4);
            block_align = Read<u16>(in, offset + 12);
            bits = Read<u16>(in, offset + 14);
        } else if (tag == Data) {
            data = offset;
            size = length;
        }
        offset += u64(length) + (length & 1);
    }
    if (channels != 2 || !((format == 1 && bits == 16) || (format == 3 && bits == 32)) ||
        block_align != 2 * bits / 8 || sample_rate < 8000 || sample_rate > 192000 || !data || size % block_align)
        throw std::runtime_error("Expected stereo PCM16 or float32 WAV");

    // Integrate 2 ms energy windows, using a fractional boundary at 44.1 kHz.
    constexpr u32 windows_per_second = 500;
    std::vector<std::array<double, 2>> envelope;
    in.seekg(data);
    u64 sample_index{};
    for (u64 window = 1; window * sample_rate / windows_per_second <= size / block_align; ++window) {
        const u64 boundary = window * sample_rate / windows_per_second;
        std::array<double, 2> energy{};
        for (; sample_index < boundary; ++sample_index) {
            std::array<u8, 8> sample;
            if (!in.read(reinterpret_cast<char*>(sample.data()), block_align)) throw std::runtime_error("Truncated WAV samples");
            for (u32 ch = 0; ch < 2; ++ch) {
                double value;
                if (format == 1) {
                    s16 pcm;
                    std::memcpy(&pcm, sample.data() + ch * sizeof(pcm), sizeof(pcm));
                    value = pcm / 32768.;
                } else {
                    float pcm;
                    std::memcpy(&pcm, sample.data() + ch * sizeof(pcm), sizeof(pcm));
                    value = pcm;
                }
                if (!std::isfinite(value)) throw std::runtime_error("Nonfinite WAV sample");
                energy[ch] += value * value;
            }
        }
        const u64 count = boundary - (window - 1) * sample_rate / windows_per_second;
        envelope.push_back({std::sqrt(energy[0] / double(count)), std::sqrt(energy[1] / double(count))});
    }
    return envelope;
}
} // namespace

void ReportGaiaCapture(const std::filesystem::path& path) {
    struct Voice { u64 load{}; BlockSummary decoder, envelope; };
    std::map<u32, Voice> voices;
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "time_ns,generation,stage,lane,flags,cursor,frames,rms,peak")
        throw std::runtime_error("Expected a decoder/envelope CSV capture");
    u64 zero{};
    size_t motion_events{};
    while (std::getline(in, line)) {
        std::ranges::replace(line, ',', ' ');
        std::istringstream row(line);
        u64 time, cursor;
        u32 generation, lane, flags, frames;
        float rms, peak;
        std::string stage;
        if (!(row >> time >> generation >> stage >> lane >> flags >> cursor >> frames >> rms >> peak))
            throw std::runtime_error("Invalid Gaia trace row");
        if (!zero) zero = time;
        if (stage == "start" || stage == "stop") {
            ++motion_events;
            std::cout << "Motion " << stage << " at " << double(s64(time) - s64(zero)) / 1.e6 << " ms\n";
            continue;
        }
        auto& voice = voices[generation];
        if (stage == "load") voice.load = time;
        else if (stage == "decoder") voice.decoder.Add(time, cursor, frames, rms, peak, (flags & 4) != 0);
        else if (stage == "envelope") voice.envelope.Add(time, cursor, frames, rms, peak, (flags & 4) != 0);
        else throw std::runtime_error("Unknown Gaia trace stage");
    }
    if (voices.empty()) throw std::runtime_error("Capture has no matching VAG voices");
    std::cout << "Motion events in capture: " << motion_events << '\n';
    std::cout << "The VAG signature is shared by multiple cues. This format does not identify the owning sound handle.\n";
    for (const auto& [generation, voice] : voices) {
        std::cout << "Matching VAG load " << generation;
        if (voice.load) std::cout << " at " << double(s64(voice.load) - s64(zero)) / 1.e6 << " ms";
        else std::cout << " (start outside capture)";
        std::cout << '\n';
        voice.decoder.Write("decoder ", voice.load);
        voice.envelope.Write("envelope", voice.load);
    }
}

void ReportStereo(const std::filesystem::path& path) {
    const auto signal = StereoEnvelope(path);
    if (signal.size() < 100) throw std::runtime_error("Expected at least 200 ms of stereo audio");
    const int size = int(signal.size());
    const int max_lag = std::min(500, size / 4);  // Up to one second of channel offset.
    const auto correlation = [&](int lag) {
        const int begin = std::max(0, -lag), end = std::min(size, size - lag);
        double x{}, y{}, xx{}, yy{}, xy{};
        for (int i = begin; i < end; ++i) {
            const double l = signal[i][0], r = signal[i + lag][1];
            x += l, y += r, xx += l*l, yy += r*r, xy += l*r;
        }
        const double n = end - begin;
        const double var_l = xx - x*x/n, var_r = yy - y*y/n;
        return var_l > 1.e-15 && var_r > 1.e-15 ? (xy - x*y/n) / std::sqrt(var_l * var_r) : -1.;
    };
    const double aligned = correlation(0);
    double best = aligned;
    int offset = 0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        const double score = correlation(lag);
        if (score > best + 1.e-9) best = score, offset = lag;
    }
    std::cout << path.filename().string() << "  aligned correlation=" << aligned << "  best=" << best;
    if (best < 0.3) std::cout << "  no clear shared signal\n";
    else std::cout << "  right-channel lag=" << offset * 2 << " ms (2 ms energy windows)\n";
}
} // namespace SoundSim
