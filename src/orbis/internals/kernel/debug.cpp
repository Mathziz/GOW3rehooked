// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "orbis/common/assert.h"
#include "orbis/internals/kernel/file_system.h"
#include "orbis/internals/kernel/orbis_error.h"
#include "orbis/internals/libs.h"

namespace Libraries::Kernel {

void PS4_SYSV_ABI sceKernelDebugOutText(void* /*unk*/, char* text) {
    sceKernelWrite(1, text, strlen(text));
    return;
}

void RegisterDebug(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("9JYNqN6jAKI", "libkernel", 1, "libkernel", sceKernelDebugOutText);
}

} // namespace Libraries::Kernel