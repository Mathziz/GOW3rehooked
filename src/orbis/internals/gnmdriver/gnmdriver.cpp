// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// libSceGnmDriver imports used by the EBOOT, writing PM4 as the PS4 library does; submits go to the GPU thread
// (renderer/gpu_thread.cpp).
#include "orbis/internals/gnmdriver/gnmdriver.h"
#include <cstring>
#include "orbis/common/small_vector.h"
#include "orbis/core/platform.h"
#include "gpu/gpu_thread.h"
#include "orbis/internals/gnmdriver/gnmdriver_init.h"
#include "orbis/internals/kernel/equeue.h"
#include "orbis/internals/kernel/orbis_error.h"
#include "orbis/internals/kernel/process.h"
#include "orbis/internals/libs.h"

namespace Libraries::GnmDriver {
namespace {
enum Op : u32 { Nop = 0x10, SetContextReg = 0x69, SetShReg = 0x76 };
constexpr u32 Type3(u32 op, u32 payload) { return 0xC0000000u | (payload - 1) << 16 | op << 8; }

template <class... T>
u32* SetReg(u32* cmd, u32 op, u32 reg, T... values) {
    *cmd++ = Type3(op, 1 + sizeof...(T));
    *cmd++ = reg;
    ((*cmd++ = values), ...);
    return cmd;
}
void TrailingNop(u32* cmd, u32 payload) { cmd[0] = Type3(Nop, payload); cmd[1] = 0; }

s32 PS4_SYSV_ABI sceGnmAddEqEvent(Kernel::OrbisKernelEqueue eq, u64 id, void* udata) {
    auto equeue = Kernel::GetEqueue(eq);
    if (!equeue) return ORBIS_KERNEL_ERROR_EBADF;
    Kernel::EqueueEvent kernel_event{};
    kernel_event.event.ident = id;
    kernel_event.event.filter = Kernel::OrbisKernelEvent::Filter::GraphicsCore;
    kernel_event.event.flags = Kernel::OrbisKernelEvent::Flags::Add;
    kernel_event.event.data = id;
    kernel_event.event.udata = udata;
    equeue->AddEvent(kernel_event);
    Platform::IrqC::Instance()->Register(
        static_cast<Platform::InterruptId>(id),
        [=](Platform::InterruptId irq) {
            if (u32(irq) <= u32(Platform::InterruptId::Compute6RelMem) || irq == Platform::InterruptId::GfxEop)
                equeue->TriggerEvent(id, Kernel::OrbisKernelEvent::Filter::GraphicsCore, reinterpret_cast<void*>(id));
        },
        equeue);
    return ORBIS_OK;
}

u32 PS4_SYSV_ABI sceGnmDrawInitDefaultHardwareState200(u32* cmdbuf, u32 size) {
    if (size < HwInitPacketSize) return 0;
    static constexpr u32 ClearState[]{0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0055800u,
                                      0x2ec47fc0u, 0xffffffffu, 0u, 0u, 0u, 10u};
    u32* const end = cmdbuf + HwInitPacketSize;
    std::memcpy(cmdbuf, ClearState, sizeof(ClearState));
    cmdbuf += std::size(ClearState);
    const auto copy = [&](const auto& sequence) {
        std::memcpy(cmdbuf, sequence.data() + 2, (sequence.size() - 2) * 4);
        cmdbuf += sequence.size() - 2;
    };
    Kernel::sceKernelIsNeoMode() ? copy(InitSequence200Neo) : copy(InitSequence200);
    TrailingNop(cmdbuf, u32(end - cmdbuf - 1));
    return HwInitPacketSize;
}

s32 InsertMarker(u32* cmdbuf, u32 size, u32 kind, const char* marker) {
    if (!cmdbuf || !marker) return -1;
    const auto length = std::strlen(marker);
    const u32 packet = u32(((length + 8) >> 2) + ((length + 0xc) >> 3) * 2);
    if (packet + 2 != size) return -1;
    cmdbuf[0] = Type3(Nop, packet + 1);
    cmdbuf[1] = kind;
    std::memset(cmdbuf + 2, 0, packet * 4);
    std::memcpy(cmdbuf + 2, marker, length);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceGnmInsertPushMarker(u32* cmdbuf, u32 size, const char* marker) {
    return InsertMarker(cmdbuf, size, 0x68750001u, marker);
}
s32 PS4_SYSV_ABI sceGnmInsertSetMarker(u32* cmdbuf, u32 size, const char* marker) {
    return InsertMarker(cmdbuf, size, 0x68750003u, marker);
}
s32 PS4_SYSV_ABI sceGnmInsertPopMarker(u32* cmdbuf, u32 size) {
    if (!cmdbuf || size != 6) return -1;
    cmdbuf[0] = Type3(Nop, 5);
    cmdbuf[1] = 0x68750002u;
    std::memset(cmdbuf + 2, 0, 16);
    return ORBIS_OK;
}

bool PS4_SYSV_ABI sceGnmIsUserPaEnabled() { return false; }

s32 PS4_SYSV_ABI sceGnmSetVsShader(u32* cmdbuf, u32 size, const u32* vs, u32 modifier) {
    if (!cmdbuf || size <= 0x1c || !vs || (modifier & 0xfcfffc3f) || vs[1]) return -1;
    const u32 rsrc1 = modifier == 0 ? vs[2] : (vs[2] & 0xfcfffc3f) | modifier;
    cmdbuf = SetReg(cmdbuf, SetShReg, 0x48u, vs[0], 0u);           // SPI_SHADER_PGM_LO/HI_VS
    cmdbuf = SetReg(cmdbuf, SetShReg, 0x4au, rsrc1, vs[3]);        // SPI_SHADER_PGM_RSRC1/2_VS
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x207u, vs[6]);         // PA_CL_VS_OUT_CNTL
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x1b1u, vs[4]);         // SPI_VS_OUT_CONFIG
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x1c3u, vs[5]);         // SPI_SHADER_POS_FORMAT
    TrailingNop(cmdbuf, 11);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceGnmSetPsShader(u32* cmdbuf, u32 size, const u32* ps) {
    if (!cmdbuf || size <= 0x27 || (ps && ps[1])) return -1;
    if (!ps) {
        cmdbuf = SetReg(cmdbuf, SetShReg, 8u, 0u, 0u);
        cmdbuf = SetReg(cmdbuf, SetContextReg, 0x203u, 0u);
        TrailingNop(cmdbuf, 0x20);
        return ORBIS_OK;
    }
    cmdbuf = SetReg(cmdbuf, SetShReg, 8u, ps[0], 0u);              // SPI_SHADER_PGM_LO/HI_PS
    cmdbuf = SetReg(cmdbuf, SetShReg, 10u, ps[2], ps[3]);          // SPI_SHADER_PGM_RSRC1/2_PS
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x1c4u, ps[4], ps[5]);  // SPI_SHADER_Z_FORMAT/COL_FORMAT
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x1b3u, ps[6], ps[7]);  // SPI_PS_INPUT_ENA/ADDR
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x1b6u, ps[8]);         // SPI_PS_IN_CONTROL
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x1b8u, ps[9]);         // SPI_BARYC_CNTL
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x203u, ps[10]);        // DB_SHADER_CONTROL
    cmdbuf = SetReg(cmdbuf, SetContextReg, 0x8fu, ps[11]);         // CB_SHADER_MASK
    TrailingNop(cmdbuf, 11);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceGnmSetCsShader(u32* cmdbuf, u32 size, const u32* cs) {
    if (!cmdbuf || size <= 0x18 || !cs || cs[1]) return -1;
    cmdbuf = SetReg(cmdbuf, SetShReg, 0x20cu, cs[0], 0u);          // COMPUTE_PGM_LO/HI
    cmdbuf = SetReg(cmdbuf, SetShReg, 0x212u, cs[2], cs[3]);       // COMPUTE_PGM_RSRC1/2
    cmdbuf = SetReg(cmdbuf, SetShReg, 0x207u, cs[4], cs[5], cs[6]); // COMPUTE_NUM_THREAD_X/Y/Z
    TrailingNop(cmdbuf, 11);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceGnmSubmitCommandBuffers(u32 count, const u32* dcbs[], u32* dcb_sizes, const u32**, u32*) {
    if (!dcbs || !dcb_sizes) return 0x80d11000;
    SmallVector<u32, 64> dwords(count);
    for (u32 i = 0; i < count; ++i) dwords[i] = dcb_sizes[i] / 4;
    Gow3::GpuThread::Submit({dcbs, count}, {dwords.data(), count});
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceGnmSubmitDone() { return ORBIS_OK; }
} // namespace

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Gow3::GpuThread::Start();
    LIB_FUNCTION("b0xyllnVY-I", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmAddEqEvent);
    LIB_FUNCTION("0H2vBYbTLHI", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmDrawInitDefaultHardwareState200);
    LIB_FUNCTION("7qZVNgEu+SY", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmInsertPopMarker);
    LIB_FUNCTION("W1Etj-jlW7Y", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmInsertPushMarker);
    LIB_FUNCTION("jiItzS6+22g", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmInsertSetMarker);
    LIB_FUNCTION("jg33rEKLfVs", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmIsUserPaEnabled);
    LIB_FUNCTION("KXltnCwEJHQ", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmSetCsShader);
    LIB_FUNCTION("bQVd5YzCal0", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmSetPsShader);
    LIB_FUNCTION("gAhCn6UiU4Y", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmSetVsShader);
    LIB_FUNCTION("zwY0YV91TTI", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmSubmitCommandBuffers);
    LIB_FUNCTION("yvZ73uQUqrk", "libSceGnmDriver", 1, "libSceGnmDriver", sceGnmSubmitDone);
}
} // namespace Libraries::GnmDriver
