// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include "orbis/common/types.h"
#include "orbis/common/assert.h"
#include "orbis/internals/kernel/threads/pthread.h"
#include "orbis/core/tls.h"
#include "main/host.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/sysarch.h>
#elif defined(__APPLE__) && defined(ARCH_X86_64)
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
#include <sys/mman.h>
#include <vector>
#elif !defined(ARCH_X86_64)
#include <pthread.h>
#endif
#if defined(__linux__) && defined(ARCH_X86_64)
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Core {

#ifdef _WIN32

// Windows

static std::atomic<DWORD> slot = TLS_OUT_OF_INDEXES;
static std::once_flag slot_alloc_flag;

static void AllocTcbKey() {
    slot = TlsAlloc();
}

u32 GetTcbKey() {
    // Once the key exists the call_once (an OS call each time) is skipped.
    if (slot.load(std::memory_order_acquire) == TLS_OUT_OF_INDEXES) [[unlikely]] std::call_once(slot_alloc_flag, &AllocTcbKey);
    return slot.load(std::memory_order_relaxed);
}

static constinit thread_local Tcb* tcb_base = nullptr;

void SetTcbBase(void* image_address) {
    tcb_base = static_cast<Tcb*>(image_address);
    const BOOL result = TlsSetValue(GetTcbKey(), image_address);
    ASSERT(result != 0);
}

Tcb* GetTcbBase() {
    return tcb_base;
}

#elif defined(__APPLE__) && defined(ARCH_X86_64)

// Apple x86_64

// Reserve space in the 32-bit address range for allocating TCB pages.
asm(".zerofill TCB_SPACE,TCB_SPACE,__tcb_space,0x3FC000");

struct LdtPage {
    void* tcb;
    u16 index;
};

static constexpr uintptr_t ldt_region_base = 0x4000;
static constexpr size_t ldt_region_size = 0x3FC000;
static constexpr u16 ldt_block_size = 0x1000;
static constexpr u16 ldt_index_base = 8;
static constexpr u16 ldt_index_total = (ldt_region_size - ldt_region_base) / ldt_block_size;

static std::vector<u16> free_ldts{};
static std::mutex free_ldts_lock;
static std::once_flag ldt_region_init_flag;
static pthread_key_t ldt_page_slot = 0;

static void FreeLdtPage(void* raw) {
    const auto* ldt_page = static_cast<LdtPage*>(raw);

    std::unique_lock lock{free_ldts_lock};
    free_ldts.push_back(ldt_page->index);
}

static void InitLdtRegion() {
    const void* result =
        mmap(reinterpret_cast<void*>(ldt_region_base), ldt_region_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    ASSERT_MSG(result != MAP_FAILED, "Failed to map memory region for LDT entries.");

    free_ldts.reserve(ldt_index_total);
    for (u16 i = ldt_index_base + ldt_index_total; i > ldt_index_base; --i) {
        free_ldts.push_back(i - 1);
    }
    ASSERT_MSG(pthread_key_create(&ldt_page_slot, FreeLdtPage) == 0,
               "Failed to create thread LDT page key: {}", errno);
}

void SetTcbBase(void* image_address) {
    std::call_once(ldt_region_init_flag, InitLdtRegion);

    auto* ldt_page = static_cast<LdtPage*>(pthread_getspecific(ldt_page_slot));
    if (ldt_page != nullptr) {
        // Update TCB pointer in existing page.
        ldt_page->tcb = image_address;
        return;
    }

    // Allocate a new LDT index for the current thread.
    u16 ldt_index;
    {
        std::unique_lock lock{free_ldts_lock};
        ASSERT_MSG(!free_ldts.empty(), "Out of LDT space.");
        ldt_index = free_ldts.back();
        free_ldts.pop_back();
    }

    const uintptr_t addr = ldt_region_base + (ldt_index - ldt_index_base) * ldt_block_size;

    // Create an LDT entry for the TCB.
    ldt_entry ldt{};
    ldt.data = {
        .limit00 = static_cast<u16>(ldt_block_size - 1),
        .base00 = static_cast<u16>(addr),
        .base16 = static_cast<u8>(addr >> 16),
        .type = DESC_DATA_WRITE,
        .dpl = 3,     // User accessible
        .present = 1, // Segment present
        .limit16 = 0,
        .stksz = DESC_DATA_32B,
        .granular = DESC_GRAN_BYTE,
        .base24 = static_cast<u8>(addr >> 24),
    };
    int ret = i386_set_ldt(ldt_index, &ldt, 1);
    ASSERT_MSG(ret == ldt_index,
               "Failed to set LDT {} at {:#x} for TLS area: syscall returned {}, errno {}",
               ldt_index, addr, ret, errno);

    // Set the FS segment to the created LDT.
    const sel_t new_selector{
        .rpl = USER_PRIV,
        .ti = SEL_LDT,
        .index = ldt_index,
    };
    asm volatile("mov %0, %%fs" ::"r"(new_selector));

    // Store the TCB base pointer and index in the created LDT area.
    ldt_page = reinterpret_cast<LdtPage*>(addr);
    ldt_page->tcb = image_address;
    ldt_page->index = ldt_index;

    ASSERT_MSG(pthread_setspecific(ldt_page_slot, ldt_page) == 0,
               "Failed to store thread LDT page pointer: {}", errno);
}

Tcb* GetTcbBase() {
    Tcb* tcb;
    asm volatile("mov %%fs:0x0, %0" : "=r"(tcb));
    return tcb;
}

#elif defined(ARCH_X86_64)

// Linux x86_64
#if defined(__FreeBSD__)
void SetTcbBase(void* image_address) {
    amd64_set_gsbase(image_address);
}
#else
void SetTcbBase(void* image_address) {
    const int ret = syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)image_address);
    ASSERT_MSG(ret == 0, "Failed to set GS base: errno {}", errno);
}
#endif

Tcb* GetTcbBase() {
    return Libraries::Kernel::g_curthread->tcb;
}

#else

// POSIX non-x86_64
// Just sets up a simple thread-local variable to store it, then instruction translation can point
// code to it.

static pthread_key_t slot = 0;
static std::once_flag slot_alloc_flag;

static void AllocTcbKey() {
    ASSERT(pthread_key_create(&slot, nullptr) == 0);
}

pthread_key_t GetTcbKey() {
    std::call_once(slot_alloc_flag, &AllocTcbKey);
    return slot;
}

void SetTcbBase(void* image_address) {
    ASSERT(pthread_setspecific(GetTcbKey(), image_address) == 0);
}

Tcb* GetTcbBase() {
    return static_cast<Tcb*>(pthread_getspecific(GetTcbKey()));
}

#endif

thread_local std::once_flag init_tls_flag;

void InitializeTLS() {
    std::call_once(init_tls_flag, [] { SetTcbBase(Libraries::Kernel::g_curthread->tcb); });
}

HostCallTimer::HostCallTimer(const char* name_)
    : name(name_), start(std::chrono::steady_clock::now().time_since_epoch().count()) {
    ++host_call_totals.depth;
}

HostCallTimer::~HostCallTimer() {
    const s64 elapsed = std::chrono::steady_clock::now().time_since_epoch().count() - start;
    // Only the outermost call counts: a call that runs guest code, which calls the host again, would count twice.
    if (--host_call_totals.depth == 0) {
        host_call_totals.ns += elapsed;
        if (elapsed > host_call_totals.longest_ns) host_call_totals.longest_ns = elapsed, host_call_totals.longest = name;
    }
}

} // namespace Core
