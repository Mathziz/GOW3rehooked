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
#include <windows.h>
#include "debug/dump_archive.h"
#include "main/host.h"

namespace Gow3::S4 {
namespace {
enum Kind : u32 { Colour, Depth, Stencil, Event };
const char* const kKind[] = {"colour", "depth", "stencil", "event"};
struct Surface { u64 lo, hi; Kind kind; u32 info; };
struct Page { u64 lo, hi; DWORD old; };
std::vector<u32> ctxr(0x400);
std::set<std::tuple<u64, u64, u32, u32>> pending;  // GPU thread: this submit's surfaces
std::shared_mutex lock;
std::vector<Surface> surfaces;  // armed surfaces, sorted by lo
std::vector<Page> pages;        // armed page ranges, sorted by lo
std::vector<Page> reopened;     // the previous submit's ranges: a fault racing Arm() lands here
struct Hit { u64 count, addr; };
std::mutex hits_lock;
std::map<std::tuple<u64, u32, u32, u32>, Hit> hits;  // (rip, kind or 9 = neighbour page, write, info) -> hit
std::atomic<u64> arms, faults, protect_failures;
std::jthread report_thread;
std::filesystem::path report_path;

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

LONG CALLBACK OnFault(EXCEPTION_POINTERS* e) {
    const auto* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const u64 a = r->ExceptionInformation[1];
    std::shared_lock guard(lock);
    const auto find = [a](const std::vector<Page>& v) {
        auto it = std::upper_bound(v.begin(), v.end(), a, [](u64 x, const Page& p) { return x < p.lo; });
        return it != v.begin() && a < std::prev(it)->hi ? std::prev(it) : v.end();
    };
    auto it = find(pages);
    if (it == pages.end()) {
        // Arm() reopened the page between the fault and this handler: retry if the access is now allowed.
        MEMORY_BASIC_INFORMATION m;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const bool allowed = VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) && !(m.Protect & PAGE_NOACCESS) &&
                             (r->ExceptionInformation[0] != 1 || (m.Protect & writable));
        return find(reopened) != reopened.end() && allowed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    u32 kind = 9, info = 0;
    for (auto& s : surfaces)
        if (a >= s.lo && a < s.hi) { kind = s.kind, info = s.info; break; }
    {
        std::scoped_lock h(hits_lock);
        auto& hit = hits[{u64(e->ContextRecord->Rip), kind, u32(r->ExceptionInformation[0]), info}];
        if (!hit.count++) hit.addr = a;
    }
    ++faults;
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(a & ~0xFFFull), 0x1000, it->old, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

void WriteReportFile(const std::filesystem::path& path) {
    std::vector<std::pair<std::tuple<u64, u32, u32, u32>, Hit>> rows;
    { std::scoped_lock h(hits_lock); rows.assign(hits.begin(), hits.end()); }
    std::sort(rows.begin(), rows.end(), [](auto& x, auto& y) { return x.second.count > y.second.count; });
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    std::fprintf(f, "submits armed %llu, faults %llu, protect failures %llu\nrip module kind access info count first_address\n",
                 arms.load(), faults.load(), protect_failures.load());
    for (auto& [key, hit] : rows) {
        auto [rip, kind, write, info] = key;
        HMODULE module{};
        wchar_t name[MAX_PATH] = L"guest";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(rip), &module))
            GetModuleFileNameW(module, name, MAX_PATH);
        const wchar_t* base = std::wcsrchr(name, L'\\');
        std::fprintf(f, "0x%llx %ls %s %s 0x%x %llu 0x%llx\n", rip, base ? base + 1 : name, kind < 4 ? kKind[kind] : "neighbour",
                     write == 1 ? "write" : write == 8 ? "exec" : "read", info, hit.count, hit.addr);
    }
    std::fclose(f);
}

void Report(const std::stop_token& token) {
    std::mutex wait_lock;
    std::condition_variable_any cv;
    while (!token.stop_requested()) {
        std::unique_lock lk(wait_lock);
        cv.wait_for(lk, token, std::chrono::seconds(2), [] { return false; });
        WriteReportFile(report_path);
    }
}
} // namespace

void Start() {
    report_path = PortDirectory() / "s4-gpu-written.txt";
    AddVectoredExceptionHandler(1, OnFault);
    report_thread = std::jthread(Report);
}

void Stop() {
    if (report_thread.joinable()) {
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
    std::unique_lock guard(lock);
    for (auto& p : pages) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(p.lo), p.hi - p.lo, p.old, &old);
    }
    reopened.swap(pages);
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
    pages.clear();
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
}
} // namespace Gow3::S4
