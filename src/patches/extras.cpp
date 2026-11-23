// SPDX-License-Identifier: GPL-2.0-or-later
#include "patches/extras.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <vector>

#include "main/host.h"
#include "orbis/core/memory.h"
#include "orbis/internals/error_codes.h"
#include "patches/memory_patcher.h"

namespace Gow3::Extras {
namespace {

constexpr size_t kToggleCount = size_t(Toggle::Count);
constexpr size_t kStubSlotBytes = 128;
constexpr size_t kSignatureMax = 64;

std::atomic<u32> g_vibration{100};
alignas(64) std::atomic<u8> g_flags[kToggleCount]{};
std::atomic<bool> g_available[kToggleCount]{};

static_assert(std::atomic<u8>::is_always_lock_free && sizeof(std::atomic<u8>) == 1,
              "Guest stubs read g_flags directly as single bytes");

struct Signature {
    const char* pattern;
    u32 at;
};

int ParseHexByte(const char* text) {
    int value = 0;
    for (int i = 0; i < 2; ++i) {
        const char c = text[i];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0) return -1;
        value = value * 16 + digit;
    }
    return value;
}

unsigned ParsePattern(const char* text, int* out) {
    unsigned n = 0;
    while (*text) {
        if (*text == ' ') {
            ++text;
            continue;
        }
        if (n == kSignatureMax || !text[1]) return 0;
        if (text[0] == '?' && text[1] == '?') {
            out[n++] = -1;
        } else {
            const int v = ParseHexByte(text);
            if (v < 0) return 0;
            out[n++] = v;
        }
        text += 2;
    }
    return n;
}

// Returns the unique RVA of `sig` across executable PT_LOAD segments of `eboot`, or ~u64(0) if missing/ambiguous.
u64 FindUniqueSignature(const Core::Module& eboot, const Signature& sig) {
    int pattern[kSignatureMax]{};
    const unsigned n = ParsePattern(sig.pattern, pattern);
    if (!n || sig.at >= n) return ~u64(0);

    unsigned best = 0, best_len = 0;
    for (unsigned i = 0; i < n;) {
        unsigned j = i;
        while (j < n && pattern[j] >= 0) ++j;
        if (j - i > best_len) {
            best = i;
            best_len = j - i;
        }
        i = j + 1;
    }
    if (!best_len) return ~u64(0);

    u8 anchor[kSignatureMax]{};
    for (unsigned i = 0; i < best_len; ++i) anchor[i] = static_cast<u8>(pattern[best + i]);

    const VAddr base = eboot.GetBaseAddress();
    u64 found = ~u64(0);
    unsigned hits = 0;

    for (const auto& phdr : eboot.elf.GetProgramHeader()) {
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_EXEC) == 0) continue;
        const u64 seg_size = std::min<u64>(phdr.p_filesz, phdr.p_memsz);
        if (seg_size < n) continue;
        const auto* seg = reinterpret_cast<const u8*>(base + phdr.p_vaddr);
        for (u64 pos = best; pos + (n - best) <= seg_size;) {
            const auto* hit = static_cast<const u8*>(
                std::memchr(seg + pos, anchor[0], static_cast<size_t>(seg_size - (n - best) - pos + 1)));
            if (!hit) break;
            pos = static_cast<u64>(hit - seg);
            if (std::memcmp(hit, anchor, best_len) == 0) {
                const u64 start = pos - best;
                unsigned k = 0;
                while (k < n && (pattern[k] < 0 || seg[start + k] == static_cast<u8>(pattern[k]))) ++k;
                if (k == n) {
                    if (++hits > 1) return ~u64(0);
                    found = phdr.p_vaddr + start + sig.at;
                }
            }
            ++pos;
        }
    }
    return hits == 1 ? found : ~u64(0);
}

void EmitPtr(u8* dst, uintptr_t value) {
    std::memcpy(dst, &value, sizeof(value));
}

size_t EmitAbsJump(u8* out, size_t n, uintptr_t target) {
    out[n++] = 0xff;
    out[n++] = 0x25;
    std::memset(out + n, 0, 4);
    n += 4;
    EmitPtr(out + n, target);
    return n + 8;
}

// Preserves SysV red zone, GPRs, and RFLAGS; dispatches live between `repl` (flag != 0) and `orig` (flag == 0).
size_t BuildCheatStub(u8* out, std::span<const u8> orig, std::span<const u8> repl, uintptr_t flag, uintptr_t resume) {
    static constexpr u8 kPrefix[]{
        0x48, 0x8d, 0x64, 0x24, 0x80,             // lea rsp, [rsp-0x80]
        0x9c,                                     // pushfq
        0x50,                                     // push rax
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs rax, flag
        0x80, 0x38, 0x00,                         // cmp byte ptr [rax], 0
        0x74, 0x00,                               // je off_path
    };
    static constexpr u8 kRestore[]{0x58, 0x9d}; // pop rax; popfq
    static constexpr u8 kStack[]{0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}; // lea rsp, [rsp+0x80]

    size_t n = sizeof(kPrefix);
    std::memcpy(out, kPrefix, n);
    EmitPtr(out + 9, flag);
    out[21] = static_cast<u8>(sizeof(kRestore) + repl.size() + sizeof(kStack) + 14);

    std::memcpy(out + n, kRestore, sizeof(kRestore));
    n += sizeof(kRestore);
    std::memcpy(out + n, repl.data(), repl.size());
    n += repl.size();
    std::memcpy(out + n, kStack, sizeof(kStack));
    n += sizeof(kStack);
    n = EmitAbsJump(out, n, resume);

    std::memcpy(out + n, kRestore, sizeof(kRestore));
    n += sizeof(kRestore);
    std::memcpy(out + n, orig.data(), orig.size());
    n += orig.size();
    std::memcpy(out + n, kStack, sizeof(kStack));
    n += sizeof(kStack);
    return EmitAbsJump(out, n, resume);
}

// ---- QTE stubs (ported from SpartanPort qte_hook.h) ----

void BuildQteRotationStub(u8* out, uintptr_t flag, uintptr_t resume) {
    static constexpr u8 kOrig[]{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56}; // push rbp; mov rbp, rsp; push r14
    static constexpr u8 kBody[]{
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs rax, flag
        0x80, 0x38, 0x00,                         // cmp byte ptr [rax], 0
        0x74, 0x06,                               // je orig
        0xb8, 0x01, 0x00, 0x00, 0x00,             // mov eax, 1
        0xc3,                                     // ret
    };
    size_t n = sizeof(kBody);
    std::memcpy(out, kBody, n);
    EmitPtr(out + 2, flag);
    std::memcpy(out + n, kOrig, sizeof(kOrig));
    n += sizeof(kOrig);
    EmitAbsJump(out, n, resume);
}

void BuildQteTimedStub(u8* out, uintptr_t flag, uintptr_t resume) {
    static constexpr u8 kBody[]{
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs rax, flag
        0x80, 0x38, 0x00,                         // cmp byte ptr [rax], 0
        0x75, 0x12,                               // jne automatic
        0x8b, 0x87, 0x98, 0x00, 0x00, 0x00,       // mov eax, [rdi+0x98]
        0x89, 0xc1,                               // mov ecx, eax
        0xf7, 0xd1,                               // not ecx
        0x23, 0x8f, 0x94, 0x00, 0x00, 0x00,       // and ecx, [rdi+0x94]
        0xeb, 0x10,                               // jmp back
        0x0f, 0xb7, 0x4b, 0x10,                   // automatic: movzx ecx, word ptr [rbx+0x10]
        0x83, 0xc1, 0x04,                         // add ecx, 4
        0xb8, 0x01, 0x00, 0x00, 0x00,             // mov eax, 1
        0xd3, 0xe0,                               // shl eax, cl
        0x89, 0xc1,                               // mov ecx, eax
    };
    std::memcpy(out, kBody, sizeof(kBody));
    EmitPtr(out + 2, flag);
    EmitAbsJump(out, sizeof(kBody), resume);
}

void BuildQteRepeatStub(u8* out, uintptr_t flag, uintptr_t orig_callee) {
    static constexpr u8 kBody[]{
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs rax, flag
        0x80, 0x38, 0x00,                         // cmp byte ptr [rax], 0
        0x74, 0x10,                               // je original
        0xc5, 0xfa, 0x10, 0x45, 0xc4,             // vmovss xmm0, [rbp-0x3c]
        0xc5, 0xfa, 0x11, 0x43, 0x08,             // vmovss [rbx+8], xmm0
        0xb8, 0x01, 0x00, 0x00, 0x00,             // mov eax, 1
        0xc3,                                     // ret
    };
    std::memcpy(out, kBody, sizeof(kBody));
    EmitPtr(out + 2, flag);
    EmitAbsJump(out, sizeof(kBody), orig_callee);
}

void BuildQteChargedStub(u8* out, uintptr_t flag, uintptr_t resume) {
    static constexpr u8 kBody[]{
        0x49, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs r9, flag
        0x41, 0x80, 0x39, 0x00,                   // cmp byte ptr [r9], 0
        0x45, 0x8b, 0x8e, 0x94, 0x00, 0x00, 0x00, // mov r9d, [r14+0x94]
        0x74, 0x04,                               // je back
        0x41, 0x0f, 0xab, 0xc1,                   // bts r9d, eax
    };
    std::memcpy(out, kBody, sizeof(kBody));
    EmitPtr(out + 2, flag);
    EmitAbsJump(out, sizeof(kBody), resume);
}

void BuildQteCircleStub(u8* out, uintptr_t flag, uintptr_t pressed, uintptr_t skip) {
    static constexpr u8 kBody[]{
        0x48, 0xb9, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs rcx, flag
        0x80, 0x39, 0x00,                         // cmp byte ptr [rcx], 0
        0x75, 0x12,                               // jne pressed
        0xf6, 0x80, 0x94, 0x00, 0x00, 0x00, 0x20, // test byte ptr [rax+0x94], 0x20
        0x74, 0x17,                               // je skip
        0xf6, 0x80, 0x98, 0x00, 0x00, 0x00, 0x20, // test byte ptr [rax+0x98], 0x20
        0x75, 0x0e,                               // jne skip
    };
    std::memcpy(out, kBody, sizeof(kBody));
    EmitPtr(out + 2, flag);
    const size_t n = EmitAbsJump(out, sizeof(kBody), pressed);
    EmitAbsJump(out, n, skip);
}

struct StubHookSpec {
    Toggle toggle;
    Signature sig;
    u8 orig_len;
    u8 repl_len;
    std::array<u8, 12> orig;
    std::array<u8, 32> repl;
};

constexpr StubHookSpec kStubHooks[]{
    // Celogamez's GoldHEN cheats made live-toggleable (adapted from SpartanPort cheat_hook.h).
    {
        Toggle::InfiniteHealth,
        {"c4 c1 7a 11 85 cc 01 00 00 49 8b 45 00 4c 89 ef", 0},
        9, 30,
        {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0xcc, 0x01, 0x00, 0x00},
        {0x9c, 0x41, 0x80, 0xbd, 0x98, 0x02, 0x00, 0x00, 0x00, 0x75, 0x09,
         0xc4, 0xc1, 0x7a, 0x10, 0x85, 0xd0, 0x01, 0x00, 0x00, 0x9d,
         0xc4, 0xc1, 0x7a, 0x11, 0x85, 0xcc, 0x01, 0x00, 0x00},
    },
    {
        Toggle::InfiniteMagic,
        {"c5 fa 5f c2 c5 fa 5d c1 c5 fa 11 83 d0 01 00 00", 8},
        8, 16,
        {0xc5, 0xfa, 0x11, 0x83, 0xd0, 0x01, 0x00, 0x00},
        {0xc5, 0xfa, 0x10, 0x83, 0xd0, 0x01, 0x00, 0x00, 0xc5, 0xfa, 0x11, 0x83, 0xd0, 0x01, 0x00, 0x00},
    },
    {
        Toggle::InfiniteItem,
        {"c5 fa 5f c2 c5 fa 5d c1 c5 fa 11 87 d4 01 00 00 c3", 8},
        8, 16,
        {0xc5, 0xfa, 0x11, 0x87, 0xd4, 0x01, 0x00, 0x00},
        {0xc5, 0xfa, 0x10, 0x87, 0xd4, 0x01, 0x00, 0x00, 0xc5, 0xfa, 0x11, 0x87, 0xd4, 0x01, 0x00, 0x00},
    },
    {
        Toggle::InfiniteRage,
        {"80 8b 68 01 00 00 80 c5 fa 11 8b d8 01 00 00 c5 f8 2e e0 73 20", 7},
        8, 10,
        {0xc5, 0xfa, 0x11, 0x8b, 0xd8, 0x01, 0x00, 0x00},
        {0xc7, 0x83, 0xd8, 0x01, 0x00, 0x00, 0x28, 0x6b, 0x6e, 0x4e},
    },
    {
        Toggle::InfiniteRage,
        {"c5 fa 11 8b d8 01 00 00 c5 fa 10 4d f4 c5 f8 2e c8", 0},
        8, 10,
        {0xc5, 0xfa, 0x11, 0x8b, 0xd8, 0x01, 0x00, 0x00},
        {0xc7, 0x83, 0xd8, 0x01, 0x00, 0x00, 0x28, 0x6b, 0x6e, 0x4e},
    },
    {
        Toggle::MaxRedOrbs,
        {"c5 fa 11 87 e8 01 00 00 48 8b 87 b0 01 00 00 a9 00 00 04 00", 0},
        8, 10,
        {0xc5, 0xfa, 0x11, 0x87, 0xe8, 0x01, 0x00, 0x00},
        {0xc7, 0x87, 0xe8, 0x01, 0x00, 0x00, 0xf0, 0x23, 0x74, 0x49},
    },
    // SpartanPort cheats: Unlock Chaos Difficulty & Infinite Double Jumps.
    {
        Toggle::UnlockChaos,
        {"48 8b 08 b8 03 00 00 00 f6 c1 08 75 1b b8 02 00 00 00", 3},
        8, 8,
        {0xb8, 0x03, 0x00, 0x00, 0x00, 0xf6, 0xc1, 0x08},
        {0xb8, 0x03, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x90},
    },
    {
        Toggle::InfiniteDoubleJumps,
        {"0f 84 ?? ?? ?? ?? 41 f6 84 24 ac 04 00 00 04 0f 85 ?? ?? ?? ??", 6},
        9, 27,
        {0x41, 0xf6, 0x84, 0x24, 0xac, 0x04, 0x00, 0x00, 0x04},
        {0x41, 0x80, 0xbc, 0x24, 0x98, 0x02, 0x00, 0x00, 0x00, 0x75, 0x07,
         0xa9, 0x00, 0x00, 0x00, 0x00, 0xeb, 0x09,
         0x41, 0xf6, 0x84, 0x24, 0xac, 0x04, 0x00, 0x00, 0x04},
    },
    // erickmendes22 experimental DOF patch made live-toggleable.
    {
        Toggle::RemoveDofErick,
        {"c5 fa 10 6d e4 c4 c1 7a 10 47 48 c4 c1 7a 10 4f 44", 5},
        6, 4,
        {0xc4, 0xc1, 0x7a, 0x10, 0x47, 0x48},
        {0xc5, 0xf8, 0x57, 0xc0},
    },
    {
        Toggle::RemoveDofErick,
        {"c4 c1 7a 10 4f 44 c4 c1 7a 10 57 3c c4 c1 7a 10 5f 40 c5 da 58 e4", 12},
        6, 4,
        {0xc4, 0xc1, 0x7a, 0x10, 0x5f, 0x40},
        {0xc5, 0xe0, 0x57, 0xdb},
    },
};

struct ToggleBinding {
    Toggle toggle;
    bool Config::* member;
};

constexpr ToggleBinding kBindings[]{
    {Toggle::InfiniteHealth, &Config::cheat_infinite_health},
    {Toggle::InfiniteMagic, &Config::cheat_infinite_magic},
    {Toggle::InfiniteItem, &Config::cheat_infinite_item},
    {Toggle::InfiniteRage, &Config::cheat_infinite_rage},
    {Toggle::MaxRedOrbs, &Config::cheat_max_red_orbs},
    {Toggle::InfiniteDoubleJumps, &Config::cheat_infinite_double_jumps},
    {Toggle::UnlockChaos, &Config::cheat_unlock_chaos},
    {Toggle::AutoQte, &Config::cheat_auto_qte},
    {Toggle::RemoveMotionBlur, &Config::remove_motion_blur},
    {Toggle::RemoveDofSpartan, &Config::remove_dof_spartan},
    {Toggle::RemoveDofErick, &Config::remove_dof_erick},
};

} // namespace

void LoadFromConfig(const Config& config) {
    SetVibration(config.vibration);
    for (const auto& b : kBindings) {
        g_flags[size_t(b.toggle)].store(config.*(b.member) ? 1 : 0, std::memory_order_relaxed);
    }
}

void WriteToConfig(Config& config) {
    config.vibration = Vibration();
    for (const auto& b : kBindings) {
        config.*(b.member) = Enabled(b.toggle);
    }
}

u32 Vibration() {
    return g_vibration.load(std::memory_order_relaxed);
}

void SetVibration(u32 percent) {
    g_vibration.store(std::min(percent, 100u), std::memory_order_relaxed);
}

bool Enabled(Toggle toggle) {
    const size_t i = size_t(toggle);
    if (i >= kToggleCount) return false;
    return g_flags[i].load(std::memory_order_relaxed) != 0;
}

void SetEnabled(Toggle toggle, bool enabled) {
    const size_t i = size_t(toggle);
    if (i >= kToggleCount) return;
    g_flags[i].store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool Available(Toggle toggle) {
    const size_t i = size_t(toggle);
    if (i >= kToggleCount) return false;
    return g_available[i].load(std::memory_order_relaxed);
}

void ResetAll() {
    SetVibration(100);
    for (size_t i = 0; i < kToggleCount; ++i) {
        g_flags[i].store(0, std::memory_order_relaxed);
    }
}

void Save() {
    Config cfg;
    WriteToConfig(cfg);
    try {
        SaveExtrasConfig(PortDirectory() / PortDir::Settings / "config.cfg", cfg);
    } catch (const std::exception& error) {
        Log(std::format("Cannot save extras config: {}", error.what()));
    }
}

void InstallHooks(Core::Module& eboot) {
    // Renderer-backed toggles are always available.
    g_available[size_t(Toggle::RemoveMotionBlur)].store(true, std::memory_order_relaxed);
    g_available[size_t(Toggle::RemoveDofSpartan)].store(true, std::memory_order_relaxed);

    if (!eboot.memory) return;
    void* stub_mem = nullptr;
    const VAddr hint = eboot.GetBaseAddress() + eboot.aligned_base_size + 8_MB;
    if (eboot.memory->MapMemory(&stub_mem, hint, 64_KB,
                                Core::MemoryProt::CpuReadWrite | Core::MemoryProt::CpuExec,
                                Core::MemoryMapFlags::NoFlags, Core::VMAType::Code,
                                "gow3 extras stubs") != ORBIS_OK ||
        !stub_mem) {
        Log("Extras: cannot allocate executable stub memory; live code hooks unavailable");
        return;
    }

    auto* const stubs = static_cast<u8*>(stub_mem);
    const VAddr base = eboot.GetBaseAddress();
    size_t next_slot = 0;
    std::vector<MemoryPatch> patches;
    std::array<bool, kToggleCount> pending_available{};

    const auto make_jump_patch = [&](u64 rva, size_t len, u8 opcode, const u8* stub_ptr,
                                     std::vector<u8> expected, MemoryPatch& out) -> bool {
        const s64 delta = reinterpret_cast<intptr_t>(stub_ptr) -
                          static_cast<intptr_t>(base + rva + 5);
        if (delta < INT32_MIN || delta > INT32_MAX || len < 5) return false;
        const s32 rel = static_cast<s32>(delta);
        std::vector<u8> repl(len, 0x90);
        repl[0] = opcode;
        std::memcpy(repl.data() + 1, &rel, 4);
        out = {rva, std::move(expected), std::move(repl)};
        return true;
    };

    // 1. Scan all cheat and erickmendes DOF sites BEFORE patching any bytes.
    std::array<u64, std::size(kStubHooks)> hook_rvas{};
    for (size_t i = 0; i < std::size(kStubHooks); ++i) {
        const auto& spec = kStubHooks[i];
        const u64 rva = FindUniqueSignature(eboot, spec.sig);
        if (rva != ~u64(0) && eboot.Contains(rva, spec.orig_len, PF_EXEC) &&
            std::memcmp(reinterpret_cast<const void*>(base + rva), spec.orig.data(), spec.orig_len) == 0) {
            hook_rvas[i] = rva;
        } else {
            hook_rvas[i] = ~u64(0);
        }
    }

    // Group hooks by Toggle so multi-site toggles (InfiniteRage, RemoveDofErick) require all sites.
    for (size_t t = 0; t < kToggleCount; ++t) {
        const auto toggle = static_cast<Toggle>(t);
        bool has_spec = false;
        bool all_found = true;
        for (size_t i = 0; i < std::size(kStubHooks); ++i) {
            if (kStubHooks[i].toggle != toggle) continue;
            has_spec = true;
            if (hook_rvas[i] == ~u64(0)) all_found = false;
        }
        if (!has_spec || !all_found) continue;

        std::vector<MemoryPatch> group;
        bool ok = true;
        size_t slot = next_slot;
        for (size_t i = 0; i < std::size(kStubHooks); ++i) {
            if (kStubHooks[i].toggle != toggle) continue;
            const auto& spec = kStubHooks[i];
            u8* stub = stubs + slot * kStubSlotBytes;
            const u64 rva = hook_rvas[i];
            std::vector<u8> expected(spec.orig.begin(), spec.orig.begin() + spec.orig_len);
            MemoryPatch patch;
            if (!make_jump_patch(rva, spec.orig_len, 0xe9, stub, std::move(expected), patch)) {
                ok = false;
                break;
            }
            BuildCheatStub(stub,
                           {spec.orig.data(), spec.orig_len},
                           {spec.repl.data(), spec.repl_len},
                           reinterpret_cast<uintptr_t>(&g_flags[t]),
                           static_cast<uintptr_t>(base + rva + spec.orig_len));
            group.push_back(std::move(patch));
            ++slot;
        }
        if (ok) {
            next_slot = slot;
            patches.insert(patches.end(), std::make_move_iterator(group.begin()),
                           std::make_move_iterator(group.end()));
            pending_available[t] = true;
        }
    }

    // 2. Scan and prepare the 5 Automatic QTE hooks independently.
    const uintptr_t auto_qte_flag = reinterpret_cast<uintptr_t>(&g_flags[size_t(Toggle::AutoQte)]);
    static constexpr Signature kQteRotationSig{
        "55 48 89 e5 41 56 53 49 89 fe c5 fa 10 05 ?? ?? ?? ?? 31 ff 4c 89 f6 e8 ?? ?? ?? ?? 4c 8d 05 ?? ?? ?? ?? "
        "49 8b 58 10 0f b7 d3", 0};
    static constexpr Signature kQteTimedSig{
        "66 83 7b 10 03 77 ?? 8b 87 98 00 00 00 c4 e2 78 f2 8f 94 00 00 00 8b 53 10 8d 72 04", 7};
    static constexpr Signature kQteRepeatSig{
        "41 0f b6 77 2c 48 8d 55 cc 48 8b 7d b8 e8 ?? ?? ?? ?? 41 89 c4 45 85 e4", 13};
    static constexpr Signature kQteChargedSig{
        "83 c0 02 45 8b 8e 94 00 00 00 44 20 c2 31 f6 41 0f a3 c1", 3};
    static constexpr Signature kQteCircleSig{
        "48 8b 40 10 48 8b 40 08 f6 80 94 00 00 00 20 74 1d f6 80 98 00 00 00 20 75 14 c4 c1 7a 10 04 24", 8};

    const u64 qte_rvas[5]{
        FindUniqueSignature(eboot, kQteRotationSig),
        FindUniqueSignature(eboot, kQteTimedSig),
        FindUniqueSignature(eboot, kQteRepeatSig),
        FindUniqueSignature(eboot, kQteChargedSig),
        FindUniqueSignature(eboot, kQteCircleSig),
    };

    int qte_ready = 0;

    // Rotation check (6B: push rbp; mov rbp, rsp; push r14)
    if (const u64 rva = qte_rvas[0]; rva != ~u64(0) && eboot.Contains(rva, 6, PF_EXEC)) {
        static constexpr u8 kOrig[]{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), kOrig, sizeof(kOrig)) == 0) {
            u8* stub = stubs + next_slot * kStubSlotBytes;
            MemoryPatch patch;
            if (make_jump_patch(rva, sizeof(kOrig), 0xe9, stub, {kOrig, kOrig + sizeof(kOrig)}, patch)) {
                BuildQteRotationStub(stub, auto_qte_flag, static_cast<uintptr_t>(base + rva + sizeof(kOrig)));
                patches.push_back(std::move(patch));
                ++next_slot;
                ++qte_ready;
            }
        }
    }

    // Timed press (15B)
    if (const u64 rva = qte_rvas[1]; rva != ~u64(0) && eboot.Contains(rva, 15, PF_EXEC)) {
        static constexpr u8 kOrig[]{0x8b, 0x87, 0x98, 0x00, 0x00, 0x00,
                                    0xc4, 0xe2, 0x78, 0xf2, 0x8f, 0x94, 0x00, 0x00, 0x00};
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), kOrig, sizeof(kOrig)) == 0) {
            u8* stub = stubs + next_slot * kStubSlotBytes;
            MemoryPatch patch;
            if (make_jump_patch(rva, sizeof(kOrig), 0xe9, stub, {kOrig, kOrig + sizeof(kOrig)}, patch)) {
                BuildQteTimedStub(stub, auto_qte_flag, static_cast<uintptr_t>(base + rva + sizeof(kOrig)));
                patches.push_back(std::move(patch));
                ++next_slot;
                ++qte_ready;
            }
        }
    }

    // Repeat press (5B call `e8 rel32`)
    if (const u64 rva = qte_rvas[2]; rva != ~u64(0) && eboot.Contains(rva, 5, PF_EXEC)) {
        const auto* site = reinterpret_cast<const u8*>(base + rva);
        if (site[0] == 0xe8) {
            s32 call_rel = 0;
            std::memcpy(&call_rel, site + 1, sizeof(call_rel));
            const s64 callee_rva = static_cast<s64>(rva) + 5 + call_rel;
            if (callee_rva >= 0 && eboot.Contains(static_cast<u64>(callee_rva), 1, PF_EXEC)) {
                u8* stub = stubs + next_slot * kStubSlotBytes;
                MemoryPatch patch;
                if (make_jump_patch(rva, 5, 0xe8, stub, {site, site + 5}, patch)) {
                    BuildQteRepeatStub(stub, auto_qte_flag,
                                       static_cast<uintptr_t>(base + static_cast<u64>(callee_rva)));
                    patches.push_back(std::move(patch));
                    ++next_slot;
                    ++qte_ready;
                }
            }
        }
    }

    // Charged press (7B: mov r9d, [r14+0x94])
    if (const u64 rva = qte_rvas[3]; rva != ~u64(0) && eboot.Contains(rva, 7, PF_EXEC)) {
        static constexpr u8 kOrig[]{0x45, 0x8b, 0x8e, 0x94, 0x00, 0x00, 0x00};
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), kOrig, sizeof(kOrig)) == 0) {
            u8* stub = stubs + next_slot * kStubSlotBytes;
            MemoryPatch patch;
            if (make_jump_patch(rva, sizeof(kOrig), 0xe9, stub, {kOrig, kOrig + sizeof(kOrig)}, patch)) {
                BuildQteChargedStub(stub, auto_qte_flag, static_cast<uintptr_t>(base + rva + sizeof(kOrig)));
                patches.push_back(std::move(patch));
                ++next_slot;
                ++qte_ready;
            }
        }
    }

    // Circle struggle (18B)
    if (const u64 rva = qte_rvas[4]; rva != ~u64(0) && eboot.Contains(rva, 0x26, PF_EXEC)) {
        static constexpr u8 kOrig[]{
            0xf6, 0x80, 0x94, 0x00, 0x00, 0x00, 0x20, 0x74, 0x1d,
            0xf6, 0x80, 0x98, 0x00, 0x00, 0x00, 0x20, 0x75, 0x14,
        };
        if (std::memcmp(reinterpret_cast<const void*>(base + rva), kOrig, sizeof(kOrig)) == 0) {
            u8* stub = stubs + next_slot * kStubSlotBytes;
            MemoryPatch patch;
            if (make_jump_patch(rva, sizeof(kOrig), 0xe9, stub, {kOrig, kOrig + sizeof(kOrig)}, patch)) {
                BuildQteCircleStub(stub, auto_qte_flag,
                                   static_cast<uintptr_t>(base + rva + sizeof(kOrig)),
                                   static_cast<uintptr_t>(base + rva + 0x26));
                patches.push_back(std::move(patch));
                ++next_slot;
                ++qte_ready;
            }
        }
    }

    if (qte_ready > 0) {
        pending_available[size_t(Toggle::AutoQte)] = true;
    }

    if (patches.empty()) {
        Log("Extras: no live code hook signatures matched EBOOT");
        return;
    }

#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stubs, next_slot * kStubSlotBytes);
#endif
    try {
        MemoryPatcher(eboot).Apply(patches);
    } catch (const std::exception& error) {
        Log(std::format("Extras: failed to apply live code hooks ({})", error.what()));
        return;
    }

    for (size_t t = 0; t < kToggleCount; ++t) {
        if (pending_available[t]) g_available[t].store(true, std::memory_order_relaxed);
    }
    if (DebugEnabled()) {
        Log(std::format("Extras: installed {} live code hook sites (Auto QTE ready for {}/5 prompt kinds)",
                        patches.size(), qte_ready));
    }
}

} // namespace Gow3::Extras
