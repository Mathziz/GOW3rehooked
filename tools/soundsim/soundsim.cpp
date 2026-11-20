// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <intrin.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <windows.h>

#include "capture.h"
#include "main/config.h"
#include "orbis/core/loader/elf.h"
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace {
namespace fs = std::filesystem;
constexpr char Cue[] = "SND_PUSHPULL_LP";
constexpr u32 CueHash = 0xb56460de;
constexpr int ReferenceFps = 60;
constexpr u32 MotionUpdate = 0x9e820, DisplacementLoad = 0x9e9fc, MotionCutoff = 0x2fea3c;
constexpr u32 StackGuard = 0x408d28, CandidateCode = 0x1c00000;
constexpr u8 LoadOriginal[]{0xc5, 0xf8, 0x28, 0x82, 0x40, 0x01, 0x00, 0x00};

template <typename T>
T ReadAt(std::istream& in, u64 offset) {
    T value{};
    if (!in.seekg(offset).read(reinterpret_cast<char*>(&value), sizeof(value)))
        throw std::runtime_error("Truncated asset");
    return value;
}

template <typename T>
void Write(u8* target, const T& value) {
    std::memcpy(target, &value, sizeof(value));
}

template <typename T>
T Read(const u8* source) {
    T value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

using MotionFn = u64(PS4_SYSV_ABI*)(void*, int*, const u8**, int, const u8*);
using EnvelopeBlockFn = void(PS4_SYSV_ABI*)(void*, const u8*, const u8*, const u8*);

size_t PS4_SYSV_ABI GuestStrlen(const char* name) { return std::strlen(name); }
float PS4_SYSV_ABI GuestModf(float x, float* whole) { return std::modf(x, whole); }
float PS4_SYSV_ABI GuestExp2(float x) { return std::exp2(x); }
void* PS4_SYSV_ABI GuestMemset(void* dst, int value, size_t size) { return std::memset(dst, value, size); }
void* PS4_SYSV_ABI GuestMemcpy(void* dst, const void* src, size_t size) { return std::memcpy(dst, src, size); }
int PS4_SYSV_ABI GuestMutex(void*) { return 0; }  // The replay executes on one thread.
u64 PS4_SYSV_ABI GuestTscFrequency() { return 1'000'000; }
u64 PS4_SYSV_ABI GuestTsc() { return 0; }

const u16* PS4_SYSV_ABI FindCue(u32 hash, const void*) {
    static constexpr u16 cue_resource[]{0, 1};  // Ordinary sound entry, not an alias.
    return hash == CueHash ? cue_resource : nullptr;
}

void* PS4_SYSV_ABI AllocateRequest(void* slot) {
    return slot;
}

struct ReleaseImage {
    void operator()(u8* address) const { VirtualFree(address, 0, MEM_RELEASE); }
};

class GuestImage {
public:
    explicit GuestImage(const fs::path& path)
        : bytes(static_cast<u8*>(VirtualAlloc(nullptr, CandidateCode + 0x3000, MEM_RESERVE, PAGE_NOACCESS))) {
        if (!bytes) throw std::runtime_error("Cannot reserve guest image");
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open eboot.bin");
        const auto self = ReadAt<self_header>(in, 0);
        if (self.magic != self_header::signature) throw std::runtime_error("Expected a decrypted PS4 SELF");
        const auto elf_at = sizeof(self_header) + self.segment_count * sizeof(self_segment_header);
        const auto elf = ReadAt<elf_header>(in, elf_at);
        if (elf.e_phnum < 2 || elf.e_phentsize != sizeof(elf_program_header))
            throw std::runtime_error("Invalid SELF program headers");
        const auto load = [&](u32 id, u32 rva, u32 size, u64 hash) {
            const auto header = ReadAt<elf_program_header>(in, elf_at + elf.e_phoff + id * sizeof(elf_program_header));
            if (header.p_type != PT_LOAD || header.p_vaddr != rva || header.p_filesz != size)
                throw std::runtime_error("Unsupported EBOOT segment");
            for (u16 i = 0; i < self.segment_count; ++i) {
                const auto segment = ReadAt<self_segment_header>(in, sizeof(self_header) + i * sizeof(self_segment_header));
                if (!segment.IsBlocked() || segment.GetId() != id) continue;
                if (segment.IsEncrypted() || segment.IsCompressed() || segment.file_size != size)
                    throw std::runtime_error("Expected a plain EBOOT segment");
                Commit(rva, size);
                if (!in.seekg(segment.file_offset).read(reinterpret_cast<char*>(At(rva)), size) ||
                    XXH64(At(rva), size, 0) != hash)
                    throw std::runtime_error("Unsupported EBOOT revision for the audio addresses");
                return;
            }
            throw std::runtime_error("EBOOT segment missing");
        };
        constexpr u32 text_size = 0x3bd43c;
        load(0, 0, text_size, 0xad45795a65408ad6ULL);
        load(1, 0x3c0000, 0x498e8, 0x0f77f147728baf87ULL);
        Commit(0x1b0c000, 0x1000);  // Scream's group mixer state.
        Commit(0x1b0d000, 0x2000);  // Scream's channel table and the render tick counter.
        Commit(0x1b45000, 0x3000);  // Boomerang's render packet, scratch, and mono sampler buses.

        Write(At(StackGuard), At(StackGuard + sizeof(u64)));  // Back the canary with a stable guest value.
        Import(0x2e9dd0, &GuestStrlen);
        Import(0x2ea2b0, &GuestMutex);
        Import(0x2ea2c0, &GuestMutex);
        Import(0x2eac90, &GuestExp2);
        Import(0x2eae30, &GuestModf);
        Import(0x2e9e60, &GuestMemset);
        Import(0x2e9de0, &GuestMemcpy);
        Import(0x2e9fa0, &GuestTsc);
        Import(0x2e9fb0, &GuestTscFrequency);
        // Relative relocations consumed by the render packet and the VAG codec.
        Write(At(0x408fe8), At(0x1b45700));
        Write(At(0x408fd8), At(0x1b0e128));  // Scream's pending KeyOff mask.
        Write(At(0x3f98c8), At(0x261dd0));  // Scream's sine LFO.
        Write(At(0x3fc520), At(0x29e820));
        Write(At(0x3fc528), At(0x29e850));
        // Leave actor state selection, hashing and enqueue/cancel in the guest.
        Redirect(0x1db830, &FindCue);
        Redirect(0x18dc60, &AllocateRequest);
        ProtectCode(0, text_size, PAGE_EXECUTE_READ);
    }

    template <typename Fn>
    Fn Function(u32 rva) const { return reinterpret_cast<Fn>(At(rva)); }
    MotionFn Update() const { return Function<MotionFn>(MotionUpdate); }
    float Cutoff() const { return Read<float>(At(MotionCutoff)); }
    u32 VoiceFlags() const { return Read<u32>(At(0x1b45760)); }
    u64 VoiceCommand() const { return Read<u64>(At(0x1b45770)); }

    void SetScreamChannel(u8* channel, u32 count = 1) const {
        Write(At(0x1b0e32c), count);
        Write(At(0x1b0e330), channel);
        Write(At(0x1b0c0d8), 1.f);
        Write(At(0x1b0c0e0), 1.f);
    }

    const u8* InitSampler(u8* timing, u8* speakers, std::span<u8> scratch, EnvelopeBlockFn tap) {
        Write(At(0x1b46608), timing);
        Write(At(0x1b46610), speakers);
        Write(At(0x1b46628), At(0x1b45700));
        using InitScratch = void(PS4_SYSV_ABI*)(void*, void*, u32, u32, void*);
        Function<InitScratch>(0x2869f0)(At(0x1b46388), scratch.data(), u32(scratch.size()), 0, At(0x1b45700));

        const u32 descriptor = 0x3fbd10;
        Write(At(descriptor + 0x248), At(0x292690));
        Write(At(descriptor + 0x250), At(0x2926a0));
        Write(At(descriptor + 0x258), At(0x292be0));
        Write(At(descriptor + 0x260), At(0x293480));
        Write(At(descriptor + 0x268), At(0x2934a0));

        if (sampler_hook_installed) return At(descriptor);
        constexpr u32 entry = 0x2a3820, trampoline = CandidateCode + 0x2000;
        constexpr u8 prologue[]{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x20};
        if (std::memcmp(At(entry), prologue, sizeof(prologue))) throw std::runtime_error("Unexpected envelope prologue");
        Commit(trampoline, 0x1000);
        Write(At(trampoline), prologue);
        Redirect(trampoline + sizeof(prologue), At(entry + sizeof(prologue)));
        ProtectCode(trampoline, 0x1000, PAGE_EXECUTE_READ);
        ProtectCode(entry, sizeof(prologue), PAGE_READWRITE);
        Redirect(entry, tap);
        ProtectCode(entry, sizeof(prologue), PAGE_EXECUTE_READ);
        sampler_hook_installed = true;
        return At(descriptor);
    }

    EnvelopeBlockFn EnvelopeBlock() const { return Function<EnvelopeBlockFn>(CandidateCode + 0x2000); }

    u32 SamplerFlags(size_t lane) const { return Read<u32>(At(0x1b45760 + u32(lane) * 4)); }

    void DispatchVoiceCommands(u32 key_on, u32 key_off) const {
        Write(At(0x1b0e0e8), key_on);
        Write(At(0x1b0e128), key_off);
        using Dispatch = void(PS4_SYSV_ABI*)();
        Function<Dispatch>(0x26aad0)();
    }

    void EnableCandidate() {
        // Reproduce the stash's vmulps after loading the actor's displacement.
        Commit(CandidateCode, 0x2000);
        constexpr u32 scale = CandidateCode + 0x1000;
        u8 stub[]{
            0xc5, 0xf8, 0x28, 0x82, 0x40, 0x01, 0x00, 0x00,  // vmovaps xmm0, [rdx+0x140]
            0xc5, 0xf8, 0x59, 0x05, 0x00, 0x00, 0x00, 0x00,  // vmulps xmm0, xmm0, [rip+scale]
            0xe9, 0x00, 0x00, 0x00, 0x00,                    // jmp DisplacementLoad+8
        };
        Write(stub + 12, s32(scale - (CandidateCode + 16)));
        Write(stub + 17, s32(DisplacementLoad + sizeof(LoadOriginal)) - s32(CandidateCode + sizeof(stub)));
        Write(At(CandidateCode), stub);
        ProtectCode(CandidateCode, 0x1000, PAGE_EXECUTE_READ);

        u8 jump[]{0xe9, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x1f, 0x00};
        Write(jump + 1, s32(CandidateCode - (DisplacementLoad + 5)));
        ProtectCode(DisplacementLoad, sizeof(jump), PAGE_READWRITE);
        if (std::memcmp(At(DisplacementLoad), LoadOriginal, sizeof(LoadOriginal)))
            throw std::runtime_error("Unexpected displacement instruction");
        Write(At(DisplacementLoad), jump);
        ProtectCode(DisplacementLoad, sizeof(jump), PAGE_EXECUTE_READ);
    }

    void SetCandidateScale(float factor) {
        Write(At(CandidateCode + 0x1000), std::array{factor, factor, factor, factor});
    }

private:
    u8* At(u32 rva) const { return bytes.get() + rva; }

    template <typename Fn>
    void Import(u32 plt, Fn target) {
        const auto* instruction = At(plt);
        if (instruction[0] != 0xff || instruction[1] != 0x25)
            throw std::runtime_error("Unexpected EBOOT import instruction");
        Write(At(plt + 6 + Read<s32>(instruction + 2)), target);
    }

    void Commit(u32 rva, size_t size) {
        if (!VirtualAlloc(At(rva), size, MEM_COMMIT, PAGE_READWRITE))
            throw std::runtime_error("Cannot commit guest pages");
    }

    void ProtectCode(u32 rva, size_t size, DWORD protection) {
        DWORD previous;
        if (!VirtualProtect(At(rva), size, protection, &previous) ||
            (protection == PAGE_EXECUTE_READ && !FlushInstructionCache(GetCurrentProcess(), At(rva), size)))
            throw std::runtime_error("Cannot publish guest code");
    }

    template <typename Fn>
    void Redirect(u32 rva, Fn target) {
        constexpr u8 jump[]{0xff, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
        Write(At(rva), jump);
        Write(At(rva + sizeof(jump)), target);
    }

    std::unique_ptr<u8, ReleaseImage> bytes;
    bool sampler_hook_installed{};
};

using CueRecord = std::array<u8, 0x28 + sizeof(Cue)>;

CueRecord LoadCue(const fs::path& game) {
    std::ifstream wad(game / "EXEC/WAD/PS4_LE/POS650.WAD", std::ios::binary);
    if (!wad) throw std::runtime_error("Cannot open POS650.WAD");
    constexpr u32 record_at = 0x78d1dbc;
    const auto record = ReadAt<CueRecord>(wad, record_at);
    const float gain = ReadAt<float>(wad, record_at + 0x10);
    if (ReadAt<s32>(wad, record_at + 0xc) != 0x10 || record[0x20] != 1 ||
        ReadAt<s32>(wad, record_at + 0x24) != 4 || !std::isfinite(gain) || gain <= 0.f ||
        std::memcmp(record.data() + 0x28, Cue, sizeof(Cue)))
        throw std::runtime_error("Unexpected Gaia motion cue layout in POS650.WAD");
    return record;
}

struct Tone {
    u16 adsr1, adsr2;
    u32 samples;
    s32 loop_start;
    u32 pan, bend_up, bend_down;
    float gain, sample_rate;
    std::vector<u8> vag;
};

struct GaiaBank {
    std::array<Tone, 2> tones;
    u16 pitch_depth;
    u32 pitch_step, impact_delay;
};

GaiaBank LoadGaiaBank(const fs::path& game) {
    std::ifstream wad(game / "EXEC/WAD/PS4_LE/POS650.WAD", std::ios::binary);
    if (!wad) throw std::runtime_error("Cannot open POS650.WAD");
    constexpr u32 chunk = 0xefa0;  // SBlk 056p, which owns Gaia's cue.
    const u32 bank = chunk + ReadAt<u32>(wad, chunk + 8);
    const u32 wave_data = chunk + ReadAt<u32>(wad, chunk + 0x10);
    if (ReadAt<std::array<char, 4>>(wad, bank) != std::array{'S', 'B', 'l', 'k'} ||
        ReadAt<std::array<char, 4>>(wad, bank + 0xc) != std::array{'0', '5', '6', 'p'})
        throw std::runtime_error("Expected SBlk 056p");
    const u32 sound = bank + ReadAt<u32>(wad, bank + 0x18);
    const u32 grains = bank + ReadAt<u32>(wad, bank + 0x1c) + ReadAt<u32>(wad, sound + 0xc);
    const u32 grain_data = bank + ReadAt<u32>(wad, bank + 0x2c);
    constexpr std::array<u8, 9> opcodes{4, 4, 4, 1, 0x15, 0x1b, 1, 0x1a, 0x16};
    if (ReadAt<u8>(wad, sound + 8) != opcodes.size())
        throw std::runtime_error("Unexpected Gaia grain count");
    for (u32 i = 0; i < opcodes.size(); ++i)
        if (ReadAt<u32>(wad, grains + i * 8) >> 24 != opcodes[i])
            throw std::runtime_error("Unexpected Gaia grain program");

    const auto load = [&](u32 index) {
        const u32 tone = grain_data + (ReadAt<u32>(wad, grains + index * 8) & 0xffffff);
        const u32 wave = wave_data + ReadAt<u32>(wad, tone + 0x44);
        const u32 samples = ReadAt<u32>(wad, wave + 0x10);
        const s32 loop_start = ReadAt<s32>(wad, wave + 0x18);
        const s32 loop_size = ReadAt<s32>(wad, wave + 0x1c);
        if (ReadAt<u32>(wad, wave) != 0x10000 || ReadAt<u32>(wad, wave + 0x14) != 1 ||
            !samples || samples > 1'000'000 || samples % 28 ||
            ReadAt<u32>(wad, tone + 0x48) != 0x20 + samples / 28 * 16 ||
            (index == 3 ? loop_start != 0 || u32(loop_size) != samples : loop_start != -1 || loop_size != 0))
            throw std::runtime_error("Unexpected Gaia mono VAG layout");
        Tone result{ReadAt<u16>(wad, tone + 0xc), ReadAt<u16>(wad, tone + 0xe), samples, loop_start,
                    ReadAt<u16>(wad, tone + 6), ReadAt<u8>(wad, tone + 0xa) * 128u, ReadAt<u8>(wad, tone + 0xb) * 128u,
                    ReadAt<u8>(wad, tone + 5) / 127.f, ReadAt<float>(wad, tone + 0x4c),
                    std::vector<u8>(samples / 28 * 16)};
        if (!std::isfinite(result.sample_rate) || result.sample_rate <= 0.f ||
            !wad.seekg(wave + 0x20).read(reinterpret_cast<char*>(result.vag.data()), result.vag.size()) ||
            result.vag[1] != (index == 3 ? 6 : 0) || result.vag[result.vag.size() - 15] != (index == 3 ? 3 : 1))
            throw std::runtime_error("Invalid Gaia VAG data");
        return result;
    };
    const u32 pitch_lfo = grain_data + (ReadAt<u32>(wad, grains + 2 * 8) & 0xffffff);
    if (ReadAt<u8>(wad, pitch_lfo + 1) != 4 || ReadAt<u8>(wad, pitch_lfo + 3) != 1 ||
        (ReadAt<u8>(wad, pitch_lfo + 8) & 1))
        throw std::runtime_error("Expected Gaia's positive sine pitch LFO");
    return {{load(3), load(6)}, ReadAt<u16>(wad, pitch_lfo + 6), ReadAt<u32>(wad, pitch_lfo + 0xc),
            ReadAt<u32>(wad, grains + 7 * 8 + 4)};
}

constexpr u32 PcmRate = 48000, PcmBlock = 256;

struct PcmLevels {
    double sum_squares{};
    float peak{};
    size_t count{};

    void Add(std::span<const float> pcm) {
        for (float value : pcm) {
            if (!std::isfinite(value)) throw std::runtime_error("Guest produced nonfinite PCM");
            sum_squares += double(value) * value;
            peak = std::max(peak, std::abs(value));
        }
        count += pcm.size();
    }

    double Rms() const { return count ? std::sqrt(sum_squares / double(count)) : 0.; }
};

struct GaiaSnapshot {
    u32 handle, frame, age, voice_mask, grain;
    u32 emitter_gain, lfo_gain, pan, bend, lfo_bend, pitch_phase;
};

struct VoiceControls {
    float gain, sample_rate;
    u32 pan, spread;
};

VoiceControls CalculateControls(const GuestImage& image, const Tone& tone, const GaiaSnapshot& snapshot) {
    alignas(16) std::array<u8, 0x3c0> handle{};
    alignas(16) std::array<u8, 0xb8> channel{};
    Write(handle.data(), snapshot.handle);
    Write(handle.data() + 4, u32{0x08800000});
    Write(handle.data() + 0x1b0, 1.f);
    Write(handle.data() + 0x1b4, snapshot.emitter_gain);
    Write(handle.data() + 0x1b8, snapshot.lfo_gain);
    Write(handle.data() + 0x1bc, 1.f);
    Write(handle.data() + 0x1c0, snapshot.pan);
    Write(handle.data() + 0x200, snapshot.bend);
    Write(handle.data() + 0x204, snapshot.lfo_bend);
    Write(channel.data() + 0x24, tone.bend_up);
    Write(channel.data() + 0x28, tone.bend_down);
    Write(channel.data() + 0x30, handle.data());
    Write(channel.data() + 0x40, s16{-1});
    Write(channel.data() + 0x64, tone.gain);
    Write(channel.data() + 0x6c, tone.pan);
    Write(channel.data() + 0x7c, tone.sample_rate);
    image.SetScreamChannel(channel.data());
    using GetLevel = float(PS4_SYSV_ABI*)(int);
    using GetPan = void(PS4_SYSV_ABI*)(int, u32*, u32*);
    VoiceControls controls{};
    controls.gain = image.Function<GetLevel>(0x2696c0)(0);
    controls.sample_rate = image.Function<GetLevel>(0x269b50)(0);
    image.Function<GetPan>(0x2698b0)(0, &controls.pan, &controls.spread);
    image.SetScreamChannel(nullptr);
    return controls;
}

constexpr u32 LfoPhaseMask = 0x7ffffff, LfoTickSpan = 2, ScreamTickRate = 240;

float GaiaPitchLfo(const GuestImage& image, const GaiaBank& bank, u32 phase) {
    alignas(16) std::array<u8, 0x48> lfo{};
    alignas(16) std::array<u8, 0x120> control{};
    Write(lfo.data(), u16{1});
    Write(lfo.data() + 2, u8{4});
    Write(lfo.data() + 6, bank.pitch_depth);
    Write(lfo.data() + 0xc, phase);
    Write(lfo.data() + 0x10, bank.pitch_step);
    using Evaluate = void(PS4_SYSV_ABI*)(void*, void*);
    image.Function<Evaluate>(0x262240)(lfo.data(), control.data());
    if (Read<u32>(lfo.data() + 0xc) != ((phase + LfoTickSpan * bank.pitch_step) & LfoPhaseMask))
        throw std::runtime_error("Unexpected Scream LFO phase advance");
    return Read<float>(control.data() + 0x28);
}

double ImpactDurationMs(const GuestImage& image, const GaiaBank& bank, GaiaSnapshot snapshot) {
    const u32 delay_remaining = snapshot.grain & 0xffff;
    if (snapshot.grain >> 16 != 7 || delay_remaining > bank.impact_delay)
        throw std::runtime_error("Gaia impact clock requires a snapshot in grain 7");
    const u32 elapsed = bank.impact_delay - delay_remaining;
    const u32 step = LfoTickSpan * bank.pitch_step;
    u32 phase = (snapshot.pitch_phase - (elapsed / LfoTickSpan) * step) & LfoPhaseMask;
    double samples{};
    for (u32 tick = 0; tick < 10 * ScreamTickRate; tick += LfoTickSpan) {
        snapshot.lfo_bend = std::bit_cast<u32>(GaiaPitchLfo(image, bank, phase));
        const auto controls = CalculateControls(image, bank.tones[1], snapshot);
        samples += double(controls.sample_rate) * LfoTickSpan / ScreamTickRate;
        if (samples >= bank.tones[1].samples) return 1000. * (tick + LfoTickSpan) / ScreamTickRate;
        phase = (phase + step) & LfoPhaseMask;
    }
    throw std::runtime_error("Gaia impact exceeded ten seconds");
}

class GaiaVoice {
public:
    GaiaVoice(const GuestImage& guest, const Tone& source, VoiceControls settings)
        : image(guest), tone(source), controls(settings) {
        Write(voice_context.data() + 0xa8, rack_voice.data());
        Write(rack_voice.data() + 0x18, plugin.data());
        Write(rack_voice.data() + 0x20, mixer.data());
        Write(rack_voice.data() + 0x30, rack.data());
        Write(rack.data() + 0x20, descriptor.data());
        Write(descriptor.data() + 0x240, u32{0x2d681});  // Sampler Fat Mono Multi V2.
        Write(descriptor.data() + 0x250, &PrepareBlock);
        Write(descriptor.data() + 0x258, &MixBlock);
        Write(descriptor.data() + 0x270, u32{1});  // One voice per packet in the isolated rack.
        Write(engine.data() + 0x28, timing.data());
        Write(timing.data() + 0x10, float(PcmBlock));
        Write(mixer.data(), u32{1});
        Write(voice_context.data(), controls.gain);
        Write(voice_context.data() + 0x20, controls.pan);
        Write(voice_context.data() + 0x24, controls.spread);

        Write(render.data() + 0x48, scratch_stack.data());
        Write(scratch_stack.data() + 0x18, scratch.data() + scratch.size());
        Write(scratch_stack.data() + 0x24, u32(scratch.size()));
        Write(scratch_stack.data() + 0x28, u32{16});
        Write(bus.data(), PcmBlock);
        Write(bus.data() + 4, u32{1});
        Write(bus.data() + 0x40, decoded.data());
    }

    void KeyOn() {
        image.Function<KeyFn>(0x252ee0)(voice_context.data());
        if ((PendingCommand() & 7) != 2 || Read<u32>(plugin.data() + 0x10) != 1)
            throw std::runtime_error("Guest did not post the expected KeyOn");
    }

    std::span<const float> Render() {
        using RackTick = void(PS4_SYSV_ABI*)(void*, void*, u8* const*, u32);
        output.fill(0.f);
        auto* const member = rack_voice.data();
        current = this;
        image.Function<RackTick>(0x282ab0)(engine.data(), rack.data(), &member, 1);
        current = nullptr;
        return output;
    }

    u64 Cursor() const { return Read<u64>(decoder.data() + 0x108); }
    u64 PendingCommand() const { return Read<u64>(plugin.data()); }
    float Azimuth() const { return Read<float>(plugin.data() + 0x70 + 0x128); }
    float Spread() const { return Read<float>(plugin.data() + 0x70 + 0x130); }

private:
    using KeyFn = void(PS4_SYSV_ABI*)(void*);
    static inline GaiaVoice* current;  // The standalone replay runs on one thread.

    static void PS4_SYSV_ABI PrepareBlock() {
        auto& voice = *current;
        const auto command = voice.image.VoiceCommand();
        if (command & 2) {
            voice.StartDecoder();
            voice.playing = true;
        }
    }

    static void PS4_SYSV_ABI MixBlock(const void*, const void*) {
        if (current->playing) current->Mix();
    }

    void StartDecoder() {
        using DecoderInitFn = void(PS4_SYSV_ABI*)(float, float, void*, u32, u32, u32, u8, void*);
        using DecoderLoadFn = void(PS4_SYSV_ABI*)(void*, u32, const u8*, u32, u32, s32, u32, u8);
        using AdsrFn = void(PS4_SYSV_ABI*)(u32, u32, u32*);
        using EnvelopeSetupFn = void(PS4_SYSV_ABI*)(float, float*, u32, u32*, void*, float*);
        // Supply the WAD tone in the rack's preparation callback, once the guest has selected its command.
        decoder.fill(0);
        envelope.fill(0.f);
        Write(decoder.data() + 0x120, controls.sample_rate / PcmRate);
        image.Function<DecoderInitFn>(0x29fcd0)(4.f, float(PcmRate), decoder.data(), 1, PcmRate, 0, 0, render.data());
        image.Function<DecoderLoadFn>(0x29feb0)(decoder.data(), 0, tone.vag.data(), u32(tone.vag.size()),
                                              tone.samples, tone.loop_start, 0, 1);
        std::array<u32, 17> adsr{};
        image.Function<AdsrFn>(0x26b280)(tone.adsr1, tone.adsr2, adsr.data());
        image.Function<EnvelopeSetupFn>(0x2a3940)(float(PcmRate) / PcmBlock, envelope.data(), adsr[0], adsr.data() + 1,
                                                render.data(), envelope.data() + 9);
    }

    void Mix() {
        using DecodeFn = void(PS4_SYSV_ABI*)(void*, u64, void*, void*);
        using EnvelopeFn = void(PS4_SYSV_ABI*)(float*, float*, float*, float*);
        using GainFn = void(PS4_SYSV_ABI*)(float, float, const float*, float*, u32);
        Write(render.data() + 0x60, image.VoiceFlags());
        decoded.fill(0.f);
        image.Function<DecodeFn>(0x2a0530)(decoder.data(), 0, bus.data(), render.data());
        float start, end;
        image.Function<EnvelopeFn>(0x2a3d90)(envelope.data(), &start, &end, envelope.data() + 9);
        image.Function<GainFn>(0x286f10)(controls.gain * start, controls.gain * (end - start) / PcmBlock,
                                       decoded.data(), output.data(), PcmBlock);
    }

    const GuestImage& image;
    const Tone& tone;
    VoiceControls controls;
    bool playing{};
    alignas(16) std::array<u8, 0xb0> voice_context{};
    alignas(16) std::array<u8, 0x38> rack_voice{};
    alignas(16) std::array<u8, 0xf0> rack{};
    alignas(16) std::array<u8, 0x148> engine{};
    alignas(16) std::array<u8, 0x18> timing{};
    alignas(16) std::array<u8, 0x280> descriptor{}, plugin{};
    alignas(16) std::array<u8, 0x180> decoder{}, render{};
    alignas(16) std::array<u8, 0x80> bus{}, mixer{};
    alignas(16) std::array<u8, 0x30> scratch_stack{};
    alignas(16) std::array<u8, 0x4000> scratch{};
    alignas(16) std::array<float, 32> envelope{};
    alignas(16) std::array<float, PcmBlock> decoded{}, output{};
};

class NativeGaiaRack {
public:
    NativeGaiaRack(GuestImage& image, u8* speakers) : image(image) {
        using InitTiming = void(PS4_SYSV_ABI*)(void*, u32, u32);
        image.Function<InitTiming>(0x2837c0)(timing.data(), PcmBlock, PcmRate);
        Write(engine.data() + 0x28, timing.data());
        const auto* desc = image.InitSampler(timing.data(), speakers, scratch, &EnvelopeBlock);
        if (Read<u32>(desc + 0x240) != 0x2d681 || Read<u32>(desc + 0x270) != members.size())
            throw std::runtime_error("Unexpected native mono sampler descriptor");
        Write(rack.data() + 0x20, desc);
        using InitCreateParams = u32(PS4_SYSV_ABI*)(void*, u8);
        image.Function<InitCreateParams>(0x2934a0)(rack.data() + 0x48, 1);
        Write(rack.data() + 0xf0, voices.data());
        Write(rack.data() + 0x100, u32(lanes.size()));
        for (size_t i = 0; i < lanes.size(); ++i) {
            auto& lane = lanes[i];
            auto* voice = voices[i].data();
            Write(lane.context.data() + 0xa8, voice);
            Write(voice + 0x18, lane.params.data());
            Write(voice + 0x20, lane.state.data());
            Write(voice + 0x30, rack.data());
            Write(lane.state.data() + 4, u32(i));
            Write(lane.params.data() + 0x30, main_bus.data());
            Write(lane.params.data() + 0x38, u32{0x00ffbb80});  // Main port, current speaker mode, 48 kHz.
            Write(channels[i].data() + 0x14, u32{1});
            Write(channels[i].data() + 0xa8, lane.context.data());
            image.Function<VoiceFn>(0x292690)(voice);
        }
    }

    void KeyOn(size_t index, const Tone& tone, const VoiceControls& controls) {
        auto& lane = lanes[index];
        auto* params = lane.params.data() + 0x70;
        const u64 address = reinterpret_cast<u64>(tone.vag.data());
        Write(params + 4, u32(address >> 32));
        Write(params + 8, u32(address));
        Write(params + 0xc, u32(tone.vag.size()));
        Write(params + 0x10, u32{0});  // VAG.
        Write(params + 0x14, float(PcmRate));
        Write(params + 0x18, u32{1});
        Write(params + 0x1c, tone.samples);
        Write(params + 0x20, tone.loop_start);
        Write(params + 0x28, u32{1});
        Write(params + 0x30, controls.sample_rate / PcmRate);
        using AdsrFn = void(PS4_SYSV_ABI*)(u32, u32, void*);
        image.Function<AdsrFn>(0x26b280)(tone.adsr1, tone.adsr2, params + 0x34);
        Write(lane.context.data(), controls.gain);
        Write(lane.context.data() + 0x20, controls.pan);
        Write(lane.context.data() + 0x24, controls.spread);
        key_on |= 1u << index;
        Write(rack.data() + 0x10c, Read<u32>(rack.data() + 0x10c) | (1u << index));
    }

    void KeyOff(size_t index) { key_off |= 1u << index; }
    void Pause(size_t index) { image.Function<VoiceFn>(0x253280)(lanes[index].context.data()); }
    void Silence(size_t index) { image.Function<VoiceFn>(0x252db0)(lanes[index].context.data()); }
    bool IsPaused(size_t index) const { return (Read<u32>(lanes[index].state.data()) & 4) != 0; }

    void Render() {
        image.SetScreamChannel(channels.front().data(), u32(channels.size()));
        image.DispatchVoiceCommands(key_on, key_off);
        for (u32 pending = key_on; pending; pending &= pending - 1)
            if ((Read<u64>(lanes[std::countr_zero(pending)].params.data()) & 7) != 2)
                throw std::runtime_error("Native KeyOn was not posted");
        key_on = key_off = 0;
        main_bus.fill(0.f);
        Write(rack.data() + 0xa8, u32{0});
        // Scream sets this flag after dispatch in snd_Synth's audio tick.
        Write(rack.data() + 0xc4, u32{1});
        using ConsumeSync = u32(PS4_SYSV_ABI*)(void*);
        const u8 sync = u8(image.Function<ConsumeSync>(0x27d4e0)(rack.data()));
        u32 count{};
        using Collect = bool(PS4_SYSV_ABI*)(void*, u8**, u8, u32*);
        using Tick = void(PS4_SYSV_ABI*)(void*, void*, u8* const*, u32);
        const auto collect = image.Function<Collect>(0x2e55e0);
        const auto tick = image.Function<Tick>(0x282ab0);
        while (collect(rack.data(), members.data(), sync, &count)) {
            for (u32 i = 0; i < count; ++i)
                rendered_lanes[i] = Read<u32>(Read<const u8*>(members[i] + 0x20) + 4);
            current = this;
            tick(engine.data(), rack.data(), members.data(), count);
            current = nullptr;
            for (u32 i = 0; i < count; ++i)
                if (image.SamplerFlags(i) & 0x10)
                    throw std::runtime_error("Native sampler rejected lane " + std::to_string(i));
        }
        main_pcm.Add(main_bus);
    }

    void ClearLevels() {
        main_pcm = {};
        for (auto& lane : lanes) lane.pcm = {};
    }

    const PcmLevels& Levels(size_t index) const { return lanes[index].pcm; }
    const PcmLevels& MainLevels() const { return main_pcm; }

private:
    using VoiceFn = void(PS4_SYSV_ABI*)(void*);
    static inline NativeGaiaRack* current;

    static void PS4_SYSV_ABI EnvelopeBlock(void* envelope, const u8* input, const u8* output, const u8* packet) {
        current->image.EnvelopeBlock()(envelope, input, output, packet);
        const u32 lane = Read<u32>(packet + 0x14);
        const auto* pcm = Read<const float*>(output + 0x40);
        if (pcm) current->lanes[current->rendered_lanes[lane]].pcm.Add({pcm, PcmBlock});
    }

    struct Lane {
        alignas(16) std::array<u8, 0xb0> context{};
        alignas(16) std::array<u8, 0x270> params{};
        alignas(16) std::array<u8, 0x500> state{};
        PcmLevels pcm;
    };

    GuestImage& image;
    u32 key_on{}, key_off{};
    alignas(16) std::array<u8, 0x148> engine{};
    alignas(16) std::array<u8, 0x20> timing{};
    alignas(16) std::array<u8, 0x110> rack{};
    alignas(32) std::array<u8, 0x40000> scratch{};
    alignas(32) std::array<float, PcmBlock * 8> main_bus{};
    PcmLevels main_pcm;
    std::array<Lane, 16> lanes{};
    std::array<std::array<u8, 0xb8>, 16> channels{};
    alignas(16) std::array<std::array<u8, 0x60>, 16> voices{};
    std::array<u8*, 4> members{};
    std::array<size_t, 4> rendered_lanes{};
};

void ReplayGaiaVoice(GuestImage& image, const GaiaBank& bank) {
    // First seven snapshots are from the 543-frame trace. The last two are from the 313-frame silent run.
    // Both traces were dumped as motionsound-handles-60fps.txt.
    // Dwords at +0x18, +0x398, +0x1b4, +0x1b8, +0x1c0, +0x200, +0x204 and +0x10c.
    constexpr GaiaSnapshot snapshots[]{
        {0x8507001d, 3574, 10, 0xc000, 0x700c8, 0x3f5ad3dc, 0x3f79668d, 15, 0xbefacdfb, 0x3e75742e, 0x00d78518},
        {0x8507001d, 3624, 60, 0xc000, 0x70001, 0x3f5eef71, 0x3f35dd2f, 12, 0xbefacdfb, 0xbe3862b1, 0x02d784f8},
        {0x85050030, 3752, 10, 0x0005, 0x700c9, 0x3f658266, 0x3f79668d,  7, 0x3efacffa, 0x3e4d7757, 0x01168518},
        {0x85050030, 3802, 60, 0x0001, 0x8008c, 0x3f663763, 0x3f35dd2f, 11, 0x3efacffa, 0xbe641a24, 0x031684f8},
        {0x85000033, 3883, 10, 0x0003, 0x700c9, 0x3f567c43, 0x3f79668d, 21, 0xbe76eef6, 0x3de5bb7b, 0x06828518},
        {0x85050031, 3900, 10, 0x0018, 0x700c8, 0x3f554af4, 0x3f79668d, 23, 0x3eb51fb6, 0x3e86b363, 0x07628518},
        {0x85050031, 3950, 60, 0x0008, 0x70001, 0x3f5b3371, 0x3f35dd2f, 21, 0x3eb51fb6, 0x3e133300, 0x016284f8},
        {0x8507001a, 2969, 10, 0x1010, 0x700c9, 0x3f5a1e24, 0x3f79668d, 347, 0xbedf8de0, 0xbe5daf2a, 0x030c8518},
        {0x8507001a, 3019, 60, 0x1010, 0x70002, 0x3f61d157, 0x3f35dd2f, 346, 0xbedf8de0, 0xbe545ca5, 0x050c84f8},
    };
    alignas(16) std::array<u8, 0xc30> speakers{};
    using InitSpeakers = void(PS4_SYSV_ABI*)(void*, u32, u32);
    image.Function<InitSpeakers>(0x27dbd0)(speakers.data(), 8, 0);
    const auto* angles = Read<const u8*>(speakers.data() + 8);
    constexpr std::array modes{1u, 0xdu};  // Scream speaker presets for stereo and 7.1.
    using Pan = void(PS4_SYSV_ABI*)(float, float, float, float, float, const void*, const void*, float*, u32);
    const auto pan = image.Function<Pan>(0x2a73f0);
    std::cout << "Gaia at 60 FPS. Replay of captured handle controls with the original Scream and VAG routines.\n"
                 "Fresh voices, unity group gain, dry main route. Speaker columns show RMS.\n"
                 "frame age handle   mask next-grain grain gain   pan   source-Hz  PCM RMS  PCM peak  stereo L stereo R  7.1 max\n";
    for (const auto& snapshot : snapshots) {
        const u32 previous = (snapshot.pitch_phase - LfoTickSpan * bank.pitch_step) & LfoPhaseMask;
        if (std::abs(GaiaPitchLfo(image, bank, previous) - std::bit_cast<float>(snapshot.lfo_bend)) > 1.e-6f)
            throw std::runtime_error("Gaia pitch LFO diverged from the capture");
        const auto active_tones = size_t(std::popcount(snapshot.voice_mask));
        if (active_tones == 0 || active_tones > bank.tones.size())
            throw std::runtime_error("Unexpected Gaia snapshot voice count");
        // A one-voice snapshot follows the end of the one-shot impact.
        for (size_t i = 0; i < active_tones; ++i) {
            const auto controls = CalculateControls(image, bank.tones[i], snapshot);
            GaiaVoice voice(image, bank.tones[i], controls);
            voice.KeyOn();
            if (controls.pan > 0 && controls.pan < 360 && !(voice.Azimuth() > 0.f))
                throw std::runtime_error("Guest KeyOn did not publish the voice azimuth");
            PcmLevels pcm;
            for (u32 block = 0; block < PcmRate / PcmBlock; ++block) pcm.Add(voice.Render());
            if ((voice.PendingCommand() & 7) || !std::isfinite(controls.gain) ||
                !std::isfinite(controls.sample_rate) || controls.sample_rate <= 0.f)
                throw std::runtime_error("Invalid state in Gaia snapshot replay");
            std::cout << std::setw(5) << snapshot.frame << ' ' << std::setw(3) << snapshot.age << ' '
                      << std::hex << snapshot.handle << ' ' << std::setw(4) << snapshot.voice_mask << std::dec << ' '
                      << std::setw(10) << (snapshot.grain >> 16) << ' ' << std::setw(5) << (i == 0 ? 3 : 6)
                      << std::fixed << std::setprecision(4) << ' ' << std::setw(6) << controls.gain
                      << ' ' << std::setw(5) << controls.pan << ' ' << std::setw(11) << controls.sample_rate
                      << ' ' << std::setw(8) << pcm.Rms() << ' ' << std::setw(8) << pcm.peak;
            for (u32 mode : modes) {
                const auto* layout = speakers.data() + 0x970 + mode * 0x20;
                const u32 count = Read<u32>(layout + 4);
                if (!angles || Read<u32>(layout + 0xc) != mode || count < 2 || count > 7)
                    throw std::runtime_error("Guest did not initialize the default speaker layout");
                alignas(16) std::array<float, 8> speaker_gain{};
                pan(voice.Azimuth(), 0.f, voice.Spread(), 1.f, 0.f, layout, angles + (count - 1) * 0xa0,
                    speaker_gain.data(), 0);
                float max_gain = 0.f;
                for (float gain : speaker_gain) {
                    if (!std::isfinite(gain)) throw std::runtime_error("Guest panner produced nonfinite gain");
                    max_gain = std::max(max_gain, std::abs(gain));
                }
                if (mode == 1) {
                    // Stereo uses the two front positions in Boomerang's speaker order.
                    for (size_t front : {1, 6})
                        std::cout << ' ' << std::setw(8) << pcm.Rms() * std::abs(speaker_gain[front]);
                } else {
                    std::cout << ' ' << std::setw(8) << pcm.Rms() * max_gain;
                }
            }
            std::cout << '\n';
        }
    }
    std::cout << "Impact duration from captured LFO phase (ms):";
    for (const auto& snapshot : snapshots)
        if (snapshot.age == 10)
            std::cout << ' ' << std::hex << snapshot.handle << std::dec << '=' << std::setprecision(1)
                      << ImpactDurationMs(image, bank, snapshot);
    std::cout << '\n';
    struct LoopEvent { u32 first, last; size_t snapshot; };
    constexpr LoopEvent events[]{{3563, 3718, 0}, {3741, 3846, 2}, {3872, 3888, 4}, {3889, 3995, 5}};
    constexpr LoopEvent silent_run[]{{2958, 3129, 7}};
    std::cout << "Native sampler across recorded motion edges. Earliest captured controls per handle.\n"
                 "Each capture starts with an empty rack. Slots follow the recorded Scream channel indices.\n"
                 "start stop  handle   loop RMS impact RMS main RMS\n";
    const auto replay = [&](std::span<const LoopEvent> recorded) {
        auto rack = std::make_unique<NativeGaiaRack>(image, speakers.data());
        u32 block{};
        const auto render_until = [&](u32 frame) {
            const u32 end = (frame - recorded.front().first) * PcmRate / (PcmBlock * ReferenceFps);
            for (; block < end; ++block) rack->Render();
        };
        for (const auto& event : recorded) {
            render_until(event.first);
            rack->ClearLevels();
            const auto& snapshot = snapshots[event.snapshot];
            const u32 loop = std::countr_zero(snapshot.voice_mask);
            const u32 impact = std::countr_zero(snapshot.voice_mask & (snapshot.voice_mask - 1));
            rack->KeyOn(loop, bank.tones[0], CalculateControls(image, bank.tones[0], snapshot));
            rack->KeyOn(impact, bank.tones[1], CalculateControls(image, bank.tones[1], snapshot));
            render_until(event.last);
            std::cout << std::setw(5) << event.first << ' ' << std::setw(5) << event.last << ' '
                      << std::hex << snapshot.handle << std::dec << ' ' << std::setprecision(4)
                      << std::setw(8) << rack->Levels(loop).Rms() << ' ' << std::setw(10) << rack->Levels(impact).Rms()
                      << ' ' << std::setw(8) << rack->MainLevels().Rms() << '\n';
            rack->KeyOff(loop);
            rack->KeyOff(impact);
        }
        rack->Render();
        return rack;
    };
    replay(silent_run);
    auto rack = replay(events);

    // Control for the stale mixer pause hypothesis. This state was not observed in the capture.
    constexpr size_t control_slot = 8;
    const auto controls = CalculateControls(image, bank.tones[0], snapshots[0]);
    rack->KeyOn(control_slot, bank.tones[0], controls);
    for (u32 i = 0; i < 8; ++i) rack->Render();
    const double playing = rack->Levels(control_slot).Rms();
    rack->Pause(control_slot);
    rack->Render();
    rack->ClearLevels();
    for (u32 i = 0; i < 8; ++i) rack->Render();
    const double paused = rack->Levels(control_slot).Rms();
    if (playing <= 0 || !rack->IsPaused(control_slot) || paused != 0)
        throw std::runtime_error("Native pause control did not enter a silent pause");
    rack->Silence(control_slot);
    rack->KeyOn(control_slot, bank.tones[0], controls);
    rack->ClearLevels();
    for (u32 i = 0; i < 8; ++i) rack->Render();
    const double restarted = rack->Levels(control_slot).Rms();
    std::cout << "Pause/Silence/KeyOn control RMS: " << playing << ' ' << paused << ' ' << restarted << '\n';
    if (rack->IsPaused(control_slot) || restarted <= 0)
        throw std::runtime_error("Native KeyOn did not restart a paused lane");
}

struct Sample {
    u32 frame;
    float moved, recorded_scale;
    bool playing, queued, live;
};

std::vector<Sample> LoadTrace(const fs::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open the motionsound trace");
    std::string line;
    if (!std::getline(in, line) || !std::getline(in, line) ||
        !line.starts_with("frame moved scale scaled type before after name emitter active queued nodes live"))
        throw std::runtime_error("Expected a motionsound trace from the motion-sound diagnostic");
    std::vector<Sample> samples;
    u64 emitter{};
    int playing{};
    while (std::getline(in, line)) {
        std::istringstream row(line);
        u32 frame;
        float moved, scale, scaled;
        int type, before, after, active, queued, nodes, live;
        u64 address;
        std::string name;
        if (!(row >> frame >> moved >> scale >> scaled >> type >> before >> after >> name >> std::hex >> address >>
              std::dec >> active >> queued >> nodes >> live))
            throw std::runtime_error("Invalid motionsound trace row");
        if (name != Cue || type != 1 || !active || !address) continue;
        if (!emitter) emitter = address;
        if (address != emitter || before != playing || (!samples.empty() && frame <= samples.back().frame) ||
            !std::isfinite(moved) || moved < 0.f || !std::isfinite(scale) || scale <= 0.f)
            throw std::runtime_error("Expected one ordered Gaia emitter trace starting with the loop stopped");
        samples.push_back({frame, moved, scale, after != 0, queued != 0, live != 0});
        playing = after;
    }
    if (samples.empty()) throw std::runtime_error("No active Gaia loop samples in trace");
    return samples;
}

struct ReplayResult {
    size_t starts{}, stops{}, mismatches{};
    float max_step{};
    u32 first_difference{};
};

ReplayResult Replay(GuestImage& image, const CueRecord& cue, const std::vector<Sample>& samples, int parts,
                    bool use_captured_scale = false) {
    alignas(16) std::array<u8, 0xb10> actor{};
    alignas(16) std::array<u8, 0x130> emitter{};
    alignas(16) std::array<u8, 0x38> request{};
    auto* const list = emitter.data() + 0x98;
    auto* const node = request.data();
    Write(actor.data() + 0x228, emitter.data());
    Write(emitter.data() + 0xf8, u32{3});  // Gaia trace's emitter flags.
    Write(emitter.data() + 0x128, node);
    Write(list, list);
    Write(list + 8, list);

    const auto update = image.Update();
    const u8* record = cue.data();
    int playing{};
    ReplayResult result;
    for (const auto& sample : samples) {
        if (use_captured_scale) image.SetCandidateScale(sample.recorded_scale);
        const float step = sample.moved / static_cast<float>(parts);
        result.max_step = std::max(result.max_step, step);
        Write(actor.data() + 0x140, step);
        for (int part = 0; part < parts; ++part) {
            const bool before = playing != 0;
            update(actor.data(), &playing, &record, 0, nullptr);
            u8* head;
            const void* sound;
            std::memcpy(&head, list, sizeof(head));
            std::memcpy(&sound, node + 0x18, sizeof(sound));
            const bool live = head == node && sound;
            const bool start = !before && playing;
            const bool stop = before && !playing;
            if ((playing != 0) != live || (start && head != node))
                throw std::runtime_error("Guest motion state disagrees with its emitter queue");
            result.starts += start;
            result.stops += stop;
            if ((playing != 0) != sample.playing || start != (sample.queued && part == 0) || live != sample.live) {
                if (!result.mismatches++) result.first_difference = sample.frame;
            }
            if (stop) {
                // The real mixer retires a cancelled request asynchronously. Recycle it for the next frame here.
                Write(list, list);
                Write(list + 8, list);
            }
        }
    }
    return result;
}

void Report(int fps, bool candidate, const ReplayResult& result) {
    std::cout << std::setw(3) << fps << "  " << std::setw(10) << (candidate ? "normalized" : "original") << "  "
              << std::setw(6) << result.starts << "  " << std::setw(5) << result.stops << "  "
              << std::setw(8) << std::fixed << std::setprecision(5) << result.max_step << "  " << result.mismatches;
    if (result.mismatches) std::cout << " (first trace frame " << result.first_difference << ')';
    std::cout << '\n';
}
} // namespace

int wmain(int argc, wchar_t** argv) try {
    if (argc > 4) throw std::runtime_error("See soundsim --help");
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        std::cout << "soundsim [--gaia] [game-dir]                   Replay captured Gaia controls through the sampler and panner.\n"
                     "soundsim game-dir motionsound-trace [fps]     Replay motion up to the emitter queue.\n"
                     "soundsim --gaia-trace gaia-audio.csv         Analyze a live decoder and envelope capture.\n"
                     "soundsim --stereo audio-in.wav [audio-out.wav] Compare front-channel lag before and after conversion.\n"
                     "The default game directory is GOW3_GAME_DIR.\n";
        return 0;
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--gaia-trace") {
        if (argc != 3) throw std::runtime_error("Usage: soundsim --gaia-trace gaia-audio.csv");
        SoundSim::ReportGaiaCapture(argv[2]);
        return 0;
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--stereo") {
        if (argc < 3) throw std::runtime_error("Usage: soundsim --stereo audio-in.wav [audio-out.wav]");
        for (int i = 2; i < argc; ++i) SoundSim::ReportStereo(argv[i]);
        return 0;
    }
    int cpu[4];
    __cpuid(cpu, 0x80000000);
    if (static_cast<u32>(cpu[0]) < 0x80000001)
        throw std::runtime_error("Cannot check the EBOOT's CPU requirements");
    __cpuid(cpu, 0x80000001);
    if (!IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) || !(cpu[2] & (1 << 5)))
        throw std::runtime_error("AVX and LZCNT are required by the EBOOT");
    __cpuidex(cpu, 7, 0);
    if (!(cpu[1] & (1 << 3))) throw std::runtime_error("BMI1 is required by the EBOOT envelope code");
    const bool gaia = argc < 3 || std::wstring_view(argv[1]) == L"--gaia";
    if (argc > 3 && gaia) throw std::runtime_error("Usage: soundsim --gaia [game-dir]");
    if (argc > 1 && std::wstring_view(argv[1]) == L"--gaia") {
        --argc;
        ++argv;
    }
    const auto* game_env = _wgetenv(L"GOW3_GAME_DIR");
    if (argc < 2 && (!game_env || !*game_env)) throw std::runtime_error("Set GOW3_GAME_DIR or pass a game directory");
    const fs::path game = argc >= 2 ? argv[1] : game_env;
    if (gaia) {
        const auto bank = LoadGaiaBank(game);
        GuestImage image(game / "eboot.bin");
        ReplayGaiaVoice(image, bank);
        return 0;
    }
    const fs::path trace = argc >= 3 ? fs::path(argv[2]) : game / "_rehooked_/motionsound-60fps.txt";
    size_t fps_length{};
    const int trace_fps = argc == 4 ? std::stoi(argv[3], &fps_length) : ReferenceFps;
    if ((argc == 4 && fps_length != std::wstring_view(argv[3]).size()) ||
        (trace_fps != 60 && trace_fps != 120 && trace_fps != 240 && trace_fps != 480))
        throw std::runtime_error("Trace FPS must be 60, 120, 240 or 480");
    const auto samples = LoadTrace(trace);
    const auto cue = LoadCue(game);
    GuestImage image(game / "eboot.bin");
    const bool scaled_capture = std::ranges::any_of(samples, [](const Sample& s) { return s.recorded_scale != 1.f; });
    std::cout << Cue << ", " << samples.size() << " captured frames at " << trace_fps
              << " FPS. EBOOT motion threshold = " << image.Cutoff() / 2.f << " units/step.\n";
    if (trace_fps == ReferenceFps)
        std::cout << "120/240/480 FPS rows subdivide captured displacement at constant velocity within each frame.\n";
    std::cout << "FPS        mode  starts  stops  raw peak  steps different from capture\n";
    const auto original = Replay(image, cue, samples, 1);
    Report(trace_fps, false, original);
    if (trace_fps == ReferenceFps) {
        if (original.mismatches || scaled_capture || !original.starts || !original.stops)
            throw std::runtime_error("60 FPS trace must match an original start/stop cycle before FPS comparison");
        for (int fps : {120, 240, 480}) Report(fps, false, Replay(image, cue, samples, fps / ReferenceFps));
    }
    image.EnableCandidate();
    bool consistent = scaled_capture || !original.mismatches;
    for (int fps : {60, 120, 240, 480}) {
        if (trace_fps != ReferenceFps && trace_fps != fps) continue;
        image.SetCandidateScale(1.f / ((1.f / Gow3::GameFrameRate(fps)) * Gow3::GameFrameRate(ReferenceFps)));
        const auto normalized = Replay(image, cue, samples, fps / trace_fps, scaled_capture);
        Report(fps, true, normalized);
        if (trace_fps == ReferenceFps || scaled_capture) consistent &= !normalized.mismatches;
    }
    if (trace_fps != ReferenceFps)
        std::cout << (scaled_capture ? "The normalized run used the capture's per-frame displacement scales.\n"
                                    : "The normalized run used a nominal frame delta.\n");
    return consistent ? 0 : 1;
} catch (const std::exception& e) {
    std::cerr << "soundsim: " << e.what() << '\n';
    return 2;
}
