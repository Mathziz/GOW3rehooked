// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "orbis/common/types.h"
#include "orbis/common/assert.h"
#include "orbis/core/signals.h"
#include <atomic>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

#if defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

void assert_fail_impl() {
    Core::Signals::Instance()->RemoveHandlers();
    Crash();
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    ShowFatalError(msg);
    assert_fail_impl();
}

bool ShowFatalError(const char* message) {
    static std::atomic_flag shown;
    if (shown.test_and_set()) return false;
#ifdef _WIN32
    const std::string text = std::string("The game hit a fatal error and has to close.\n\n") + message;
    std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), static_cast<int>(wide.size()));
    MessageBoxW(nullptr, wide.c_str(), L"God of War III",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL | MB_SETFOREGROUND);
#endif
    return true;
}
