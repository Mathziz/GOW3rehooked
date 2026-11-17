// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _MSC_VER
#define BREAKPOINT __debugbreak
#elif defined(__GNUC__)
#define BREAKPOINT __builtin_trap
#else
#error Unsupported compiler
#endif

static inline bool IsProfilerConnected() {
    return false;
}

#define TRACY_GPU_ENABLED 0
#define TRACK_ALLOC(ptr, size, pool) (void)0
#define TRACK_FREE(ptr, pool) (void)0
#define EMULATOR_TRACE (void)0
#define RENDERER_TRACE (void)0
#define HLE_TRACE (void)0
#define TRACE_HINT(str) (void)(str)
#define TRACE_WARN(msg) (void)(msg)
#define TRACE_ERROR(msg) (void)(msg)
#define TRACE_CRIT(msg) (void)(msg)
#define FRAME_END (void)0
#define FIBER_ENTER(name) (void)0
#define FIBER_EXIT (void)0
