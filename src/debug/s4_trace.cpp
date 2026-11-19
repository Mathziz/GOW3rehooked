// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/s4_trace.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <fstream>
#include <sys/mman.h>
#include "orbis/core/signals.h"
#endif
#include <cinttypes>
#include "debug/dump_archive.h"
#include "main/host.h"

namespace Gow3::S4 {
namespace {
enum Kind : u32 { Colour, Depth, Stencil, Event };
const char* const kKind[] = {"colour", "depth", "stencil", "event"};
struct Surface { u64 lo, hi; Kind kind; u32 info; };
#ifdef _WIN32
using Protection = DWORD;
#else
using Protection = int;
constexpr Protection no_access = PROT_NONE;
#endif
struct Page {
    u64 lo, hi;
    Protection old;
#ifndef _WIN32
    size_t first{}; // first 4 KB page in the fault record array
#endif
};

#ifndef _WIN32
bool Protect(u64 address, u64 size, Protection protection) {
    return mprotect(reinterpret_cast<void*>(address), size, protection) == 0;
}

std::vector<Page> HostMappings() {
    std::vector<Page> result;
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        u64 begin, end;
        char mode[5];
        if (std::sscanf(line.c_str(), "%" SCNx64 "-%" SCNx64 " %4s", &begin, &end, mode) == 3) {
            result.push_back({begin, end, (mode[0] == 'r' ? PROT_READ : 0) | (mode[1] == 'w' ? PROT_WRITE : 0) |
                                          (mode[2] == 'x' ? PROT_EXEC : 0)});
        }
    }
    return result;
}
#endif
std::vector<u32> ctxr(0x400);
std::set<std::tuple<u64, u64, u32, u32>> pending;  // GPU thread: this submit's surfaces
#ifdef _WIN32
std::shared_mutex lock;
#else
constexpr u64 page_size = 0x1000;
enum PageState : u8 { Armed, Opening, Open };
struct Touch { u64 addr{}, rip{}; u32 access{}; };
struct Batch { std::vector<Surface> surfaces; std::vector<Touch> touches; };
std::vector<u8> page_states;
std::vector<Touch> touches; // at most one record per page, appended in fault order
std::atomic<size_t> touch_count{};
std::vector<Batch> batches;
std::condition_variable_any batch_ready;
std::mutex arm_mutex; // Arm and Stop
bool stopped{};

constexpr u32 changing = 1u << 31;
std::atomic<u32> page_users{};
thread_local volatile sig_atomic_t changing_here{};
static_assert(std::atomic<u32>::is_always_lock_free && std::atomic<u64>::is_always_lock_free &&
              std::atomic<size_t>::is_always_lock_free && std::atomic_ref<u8>::is_always_lock_free);

// Only swap prepared tables and change protections here. A fault on another thread retries the
// instruction during the change, so the writer must never allocate or acquire a library lock here.
struct ChangePages {
    ChangePages() {
        changing_here = true;
        page_users.fetch_or(changing);
        while (page_users.load() != changing) _mm_pause();
    }
    ~ChangePages() { page_users.fetch_and(~changing); changing_here = false; }
};
struct ReadPages {
    u32 previous = page_users.fetch_add(1);
    ~ReadPages() { page_users.fetch_sub(1); }
};
#endif
std::vector<Surface> surfaces;  // armed surfaces, sorted by lo
std::vector<Page> pages;        // armed page ranges, sorted by lo
std::vector<Page> reopened;     // the previous submit's ranges: a fault racing Arm() lands here
struct Hit { u64 count, addr; };
std::mutex hits_lock;
std::map<std::tuple<u64, u32, u32, u32>, Hit> hits;  // (rip, kind or 9 = neighbour page, write, info) -> hit
std::atomic<u64> arms, faults, protect_failures;
std::jthread report_thread;
std::filesystem::path report_path;

auto FindPage(const std::vector<Page>& ranges, u64 addr) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), addr, [](u64 a, const Page& p) { return a < p.lo; });
    return it != ranges.begin() && addr < std::prev(it)->hi ? std::prev(it) : ranges.end();
}

void RecordHit(const std::vector<Surface>& watched, u64 addr, u64 rip, u32 access) {
    u32 kind = 9, info = 0;
    for (const auto& s : watched)
        if (addr >= s.lo && addr < s.hi) { kind = s.kind; info = s.info; break; }
    auto& hit = hits[{rip, kind, access, info}];
    if (!hit.count++) hit.addr = addr;
}

void Add(u64 lo, u64 bytes, Kind kind, u32 info) {
    if (lo && bytes && bytes <= (256ull << 20)) pending.insert({lo, lo + bytes, kind, info});
}

// At each draw: the colour targets with a non-zero write mask and the depth/stencil buffers with writes enabled.
void OnDraw() {
    static const u8 bpp[32] = {0, 1, 2, 2, 4, 4, 4, 4, 4, 4, 4, 8, 8, 0, 16, 0, 2, 2, 2, 2, 4, 4, 8};
    for (u32 i = 0; i < 8; ++i) {
        const u32* cb = &ctxr[0x318 + i * 0xF];
        const u32 format = cb[4] >> 2 & 0x1F, mask = ctxr[0x08E] >> (i * 4) & 0xF;
        if (!format || !mask) continue;
        const u64 pixels = (u64(cb[2] & 0x3FFFFF) + 1) * 64, layers = (cb[3] >> 13 & 0x7FF) - (cb[3] & 0x7FF) + 1;
        Add(u64(cb[0]) << 8, pixels * (bpp[format] ? bpp[format] : 4) * (1u << (cb[5] >> 12 & 7)) * layers, Colour, cb[4]);
    }
    const u32 depth_control = ctxr[0x200], z_info = ctxr[0x010];
    const u64 pixels = (u64(ctxr[0x017] & 0x3FFFFF) + 1) * 64, samples = 1u << (z_info >> 2 & 3);
    if ((z_info & 3) && (depth_control & 4)) Add(u64(ctxr[0x014]) << 8, pixels * ((z_info & 3) == 1 ? 2 : 4) * samples, Depth, z_info);
    if ((ctxr[0x011] & 1) && (depth_control & 1)) Add(u64(ctxr[0x015]) << 8, pixels * samples, Stencil, ctxr[0x011]);
}

#ifdef _WIN32
LONG CALLBACK OnFault(EXCEPTION_POINTERS* e) {
    const auto* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const u64 a = r->ExceptionInformation[1], rip = e->ContextRecord->Rip;
    const u32 access = u32(r->ExceptionInformation[0]);
    std::shared_lock guard(lock);
    auto it = FindPage(pages, a);
    if (it == pages.end()) {
        // Arm() reopened the page between the fault and this handler: retry if the access is now allowed.
        MEMORY_BASIC_INFORMATION m;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const bool allowed = VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) && !(m.Protect & PAGE_NOACCESS) &&
                             (r->ExceptionInformation[0] != 1 || (m.Protect & writable));
        return FindPage(reopened, a) != reopened.end() && allowed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    {
        std::scoped_lock h(hits_lock);
        RecordHit(surfaces, a, rip, access);
    }
    ++faults;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(a & ~0xFFFull), 0x1000, it->old, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}
#else
bool OnFault(void* context, void* address) {
    if (changing_here) return false;
    if (page_users.load() & changing) return true;
    ReadPages reader;
    if (reader.previous & changing) return true;
    const u64 addr = reinterpret_cast<u64>(address), rip = reinterpret_cast<u64>(Common::GetRip(context));
    const u32 access = Common::IsWriteError(context) ? 1 : Common::IsExecuteError(context) ? 8 : 0;
    const int permission = access == 1 ? PROT_WRITE : access == 8 ? PROT_EXEC : PROT_READ;
    auto p = FindPage(pages, addr);
    if (p != pages.end()) {
        std::atomic_ref state(page_states[p->first + (addr - p->lo) / page_size]);
        u8 seen = Armed;
        if (state.compare_exchange_strong(seen, Opening)) {
            const int saved_errno = errno;
            const bool restored = Protect(addr & ~(page_size - 1), page_size, p->old);
            errno = saved_errno;
            if (!restored) {
                state.store(Armed);
                ++protect_failures;
                return false;
            }
            touches[touch_count.fetch_add(1, std::memory_order_relaxed)] = {addr, rip, access};
            state.store(Open);
            ++faults;
            return (p->old & permission) != 0;
        }
        if (seen == Opening) return true;
    } else {
        p = FindPage(reopened, addr);
        if (p == reopened.end()) return false;
    }
    // The fault was delivered after another handler or Arm restored this page.
    static thread_local u64 last_addr = ~u64{}, last_rip{}, last_arm{};
    const u64 arm = arms.load(std::memory_order_relaxed);
    if (!(p->old & permission) || (last_addr == addr && last_rip == rip && last_arm == arm)) return false;
    last_addr = addr;
    last_rip = rip;
    last_arm = arm;
    return true;
}

void Disarm() {
    {
        ChangePages change;
        for (const auto& p : pages) {
            // A touched page already belongs to the guest write tracker again.
            u64 start = p.lo;
            for (u64 addr = p.lo; addr < p.hi; addr += page_size) {
                if (page_states[p.first + (addr - p.lo) / page_size] == Armed) continue;
                if (start < addr && !Protect(start, addr - start, p.old)) ++protect_failures;
                start = addr + page_size;
            }
            if (start < p.hi && !Protect(start, p.hi - start, p.old)) ++protect_failures;
        }
        reopened.swap(pages);
        pages.clear();
    }
    if (const size_t count = touch_count.exchange(0, std::memory_order_relaxed)) {
        Batch batch{std::move(surfaces), {touches.begin(), touches.begin() + count}};
        {
            std::scoped_lock guard(hits_lock);
            batches.push_back(std::move(batch));
        }
        batch_ready.notify_one();
    }
}
#endif

void WriteReportFile(const std::filesystem::path& path) {
    std::vector<std::pair<std::tuple<u64, u32, u32, u32>, Hit>> rows;
#ifdef _WIN32
    { std::scoped_lock h(hits_lock); rows.assign(hits.begin(), hits.end()); }
#else
    rows.assign(hits.begin(), hits.end());
#endif
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second.count > y.second.count; });
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"w");
#else
    FILE* f = std::fopen(path.c_str(), "w");
#endif
    if (!f) return;
    std::fprintf(f, "submits armed %" PRIu64 ", faults %" PRIu64 ", protect failures %" PRIu64 "\nrip module kind access info count first_address\n",
                 arms.load(), faults.load(), protect_failures.load());
    for (auto& [key, hit] : rows) {
        auto [rip, kind, write, info] = key;
#ifdef _WIN32
        HMODULE module{};
        wchar_t name[MAX_PATH] = L"guest";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(rip), &module))
            GetModuleFileNameW(module, name, MAX_PATH);
        const wchar_t* base = std::wcsrchr(name, L'\\');
        const auto* module_name = base ? base + 1 : name;
#else
        Dl_info module{};
        const auto name = dladdr(reinterpret_cast<void*>(rip), &module) && module.dli_fname
            ? std::filesystem::path(module.dli_fname).filename().string() : "guest";
        const auto* module_name = name.c_str();
#endif
        std::fprintf(f, "0x%" PRIx64
#ifdef _WIN32
                     " %ls"
#else
                     " %s"
#endif
                     " %s %s 0x%x %" PRIu64 " 0x%" PRIx64 "\n", rip, module_name, kind < 4 ? kKind[kind] : "neighbour",
                     write == 1 ? "write" : write == 8 ? "exec" : "read", info, hit.count, hit.addr);
    }
    std::fclose(f);
}

void Report(const std::stop_token& token) {
#ifdef _WIN32
    std::mutex wait_lock;
    std::condition_variable_any cv;
    while (!token.stop_requested()) {
        std::unique_lock lk(wait_lock);
        cv.wait_for(lk, token, std::chrono::seconds(2), [] { return false; });
        WriteReportFile(report_path);
    }
#else
    std::vector<Batch> received;
    for (;;) {
        {
            std::unique_lock lock(hits_lock);
            if (!batch_ready.wait(lock, token, [] { return !batches.empty(); })) break;
            received.swap(batches);
        }
        for (const auto& batch : received)
            for (const auto& touch : batch.touches)
                RecordHit(batch.surfaces, touch.addr, touch.rip, touch.access);
        received.clear();
    }
    WriteReportFile(report_path);
#endif
}
} // namespace

#ifndef _WIN32
void Install() {
    Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 0);
}
#endif

void Start() {
    report_path = PortDirectory() / "s4-gpu-written.txt";
#ifdef _WIN32
    AddVectoredExceptionHandler(1, OnFault);
#endif
    report_thread = std::jthread(Report);
}

void Stop() {
    if (report_thread.joinable()) {
#ifndef _WIN32
        {
            std::scoped_lock guard(arm_mutex);
            stopped = true;
            Disarm();
        }
#endif
        report_thread.request_stop();
        report_thread.join();
    }
    if (!report_path.empty()) {
        DumpArchive::AdoptFile("s4-gpu-written.txt", report_path);
        report_path.clear();
    }
}

void Packet(u32 header, const u32* p, u32 payload) {
    const u32 op = header >> 8 & 0xff;
    if (op == 0x69 && p[0] < 0x400) // SET_CONTEXT_REG
        for (u32 k = 1; k < payload && p[0] + k - 1 < 0x400; ++k) ctxr[p[0] + k - 1] = p[k];
    if (op == 0x27 || op == 0x2D || op == 0x35) OnDraw();
    if (op == 0x46 && payload >= 3) Add((u64(p[1]) | u64(p[2] & 0xFFFF) << 32) & ~7ull, 16, Event, p[0] & 0x3F);
}

void Arm() {
#ifdef _WIN32
    std::unique_lock guard(lock);
    for (auto& p : pages) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(p.lo), p.hi - p.lo, p.old, &old);
    }
    reopened.swap(pages);
    pages.clear();
#else
    std::scoped_lock guard(arm_mutex);
    if (stopped) { pending.clear(); return; }
    Disarm();
#endif
    surfaces.clear();
    for (auto& [lo, hi, kind, info] : pending) surfaces.push_back({lo, hi, Kind(kind), info});
    pending.clear();
    std::vector<std::pair<u64, u64>> spans;
    spans.reserve(surfaces.size());
    for (auto& s : surfaces) spans.push_back({s.lo & ~0xFFFull, (s.hi + 0xFFF) & ~0xFFFull});
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<u64, u64>> merged;
    for (auto [lo, hi] : spans)
        if (!merged.empty() && lo <= merged.back().second) merged.back().second = std::max(merged.back().second, hi);
        else merged.push_back({lo, hi});
    // Guest memory is a set of separate views: protect each committed region piece on its own.
#ifdef _WIN32
    for (auto [lo, hi] : merged)
        for (u64 a = lo; a < hi;) {
            MEMORY_BASIC_INFORMATION m;
            if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m))) break;
            const u64 end = std::min(hi, u64(m.BaseAddress) + m.RegionSize);
            DWORD old;
            if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                VirtualProtect(reinterpret_cast<void*>(a), end - a, PAGE_NOACCESS, &old) ? pages.push_back({a, end, old}) : void(++protect_failures);
            a = end;
        }
    ++arms;
#else
    const auto maps = HostMappings();
    std::vector<Page> next;
    size_t page_count = 0;
    auto it = maps.begin();
    for (auto [lo, hi] : merged) {
        for (; it != maps.end() && it->hi <= lo; ++it) {}
        for (auto m = it; m != maps.end() && m->lo < hi; ++m) {
            if (m->old == no_access) continue;
            const auto begin = std::max(lo, m->lo), end = std::min(hi, m->hi);
            next.push_back({begin, end, m->old, page_count});
            page_count += (end - begin) / page_size;
        }
    }
    touches.resize(page_count);
    page_states.assign(page_count, Armed);
    ChangePages change;
    std::erase_if(next, [](const Page& p) {
        if (Protect(p.lo, p.hi - p.lo, no_access)) return false;
        ++protect_failures;
        return true;
    });
    pages.swap(next);
    ++arms;
#endif
}
} // namespace Gow3::S4
