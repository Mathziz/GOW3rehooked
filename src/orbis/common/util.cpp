// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <windows.h>

#include "orbis/common/assert.h"
#include "orbis/common/ntapi.h"
#include "orbis/common/slot_vector.h"
#include "orbis/common/util.h"

NtClose_t NtClose = nullptr;
NtSetInformationFile_t NtSetInformationFile = nullptr;
NtCreateThread_t NtCreateThread = nullptr;
NtTerminateThread_t NtTerminateThread = nullptr;
NtQueueApcThreadEx_t NtQueueApcThreadEx = nullptr;

namespace Common {

namespace NtApi {
void Initialize() {
    HMODULE nt_handle = GetModuleHandleA("ntdll.dll");
    NtClose = (NtClose_t)GetProcAddress(nt_handle, "NtClose");
    NtSetInformationFile =
        (NtSetInformationFile_t)GetProcAddress(nt_handle, "NtSetInformationFile");
    NtCreateThread = (NtCreateThread_t)GetProcAddress(nt_handle, "NtCreateThread");
    NtTerminateThread = (NtTerminateThread_t)GetProcAddress(nt_handle, "NtTerminateThread");
    NtQueueApcThreadEx = (NtQueueApcThreadEx_t)GetProcAddress(nt_handle, "NtQueueApcThreadEx");
}
} // namespace NtApi

const char g_scm_rev[] = "5d6ff7934d1296faf9684f1e585fc6deb52f19cb";

static u64 EstimateRDTSCFrequency() {
    // Discard the first measurement.
    FencedRDTSC();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    FencedRDTSC();

    const auto now_ns = [] {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        return 100 * ((u64{ft.dwHighDateTime} << 32) | ft.dwLowDateTime);
    };
    const u64 start_time = now_ns();
    const u64 tsc_start = FencedRDTSC();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const u64 end_time = now_ns();
    const u64 tsc_end = FencedRDTSC();
    const u64 tsc_freq =
        MultiplyAndDivide64(tsc_end - tsc_start, 1000000000ULL, end_time - start_time);
    constexpr u64 Nearest = 100'000;
    const u64 mod = tsc_freq % Nearest;
    return mod >= Nearest / 2 ? tsc_freq - mod + Nearest : tsc_freq - mod;
}

NativeClock::NativeClock()
    : rdtsc_frequency{EstimateRDTSCFrequency()},
      us_rdtsc_factor{static_cast<u64>((static_cast<unsigned __int128>(std::micro::den) << 64) /
                                       rdtsc_frequency)} {}

std::string GetLastErrorMsg() {
    LPSTR err_str;
    const DWORD res = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, GetLastError(), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<LPSTR>(&err_str), 1, nullptr);
    if (!res) {
        return "(FormatMessageA failed to format error)";
    }
    std::string ret(err_str);
    LocalFree(err_str);
    return ret;
}

std::string ToLower(std::string_view input) {
    std::string str(input.size(), '\0');
    std::ranges::transform(input, str.begin(), tolower);
    return str;
}

std::vector<std::string> SplitString(const std::string& str, char delimiter) {
    std::istringstream iss(str);
    std::vector<std::string> output(1);
    while (std::getline(iss, output.back(), delimiter)) {
        output.emplace_back();
    }
    output.pop_back();
    return output;
}

std::string UTF16ToUTF8(std::wstring_view input) {
    const int len = static_cast<int>(input.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), len, nullptr, 0, nullptr, nullptr);
    std::string output(size, '\0');
    if (size != WideCharToMultiByte(CP_UTF8, 0, input.data(), len, output.data(), size, nullptr,
                                    nullptr)) {
        output.clear();
    }
    return output;
}

std::wstring UTF8ToUTF16W(std::string_view input) {
    const int len = static_cast<int>(input.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), len, nullptr, 0);
    std::wstring output(size, L'\0');
    if (size != MultiByteToWideChar(CP_UTF8, 0, input.data(), len, output.data(), size)) {
        output.clear();
    }
    return output;
}

void* GetRip(void* ctx) {
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
}

bool IsWriteError(void* ctx) {
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
}

bool IsExecuteError(void* ctx) {
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 0xf;
}

void* ReserveMemoryPages(std::size_t size) noexcept {
    void* const base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    ASSERT(base);
    return base;
}

void CommitMemoryPages(void* address, std::size_t size) noexcept {
    ASSERT(VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) == address);
}

void ReleaseMemoryPages(void* base, std::size_t) noexcept {
    if (base) {
        ASSERT(VirtualFree(base, 0, MEM_RELEASE));
    }
}

} // namespace Common
