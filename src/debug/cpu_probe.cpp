// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/cpu_probe.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include "debug/dump_archive.h"
#include "gpu/vk_gpu.h"
#include "orbis/common/thread.h"
#include "orbis/core/linker.h"
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#else
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "main/host.h"
#endif

namespace Gow3::CpuProbe {
namespace {
constexpr int kDepth = 14;
struct Sample { u64 t, phase, rip, ret[kDepth]; u32 thread, ran; };  // phase: TEMP blocks mode, its parallel recording's start
constexpr u64 kRing = 1 << 18;
std::unique_ptr<Sample[]> ring;
std::atomic<u64> count{0};
#ifdef _WIN32
std::atomic<HANDLE> threads[kThreads]{};
#else
enum CaptureState : u8 { Idle, Requested, Ready };
struct Capture {
    std::atomic<pid_t> tid{};
    std::atomic<CaptureState> state{};
    clockid_t clock{};
    Sample sample{};
};
Capture threads[kThreads];
thread_local u32 capture_thread = kThreads;
int capture_signal;
using UnwInitLocal2Fn = int (*)(unw_cursor_t*, unw_context_t*, int);
using UnwStepFn = int (*)(unw_cursor_t*);
using UnwGetRegFn = int (*)(unw_cursor_t*, unw_regnum_t, unw_word_t*);
UnwInitLocal2Fn p_unw_init_local2 = nullptr;
UnwStepFn p_unw_step = nullptr;
UnwGetRegFn p_unw_get_reg = nullptr;
#define GOW3_STR1(x) #x
#define GOW3_STR(x) GOW3_STR1(x)
void LoadLibunwind() {
    if (p_unw_init_local2) return;
    void* lib = dlopen("libunwind.so.8", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libunwind.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return;
    auto init_fn = reinterpret_cast<UnwInitLocal2Fn>(dlsym(lib, GOW3_STR(unw_init_local2)));
    auto step_fn = reinterpret_cast<UnwStepFn>(dlsym(lib, GOW3_STR(unw_step)));
    auto get_reg_fn = reinterpret_cast<UnwGetRegFn>(dlsym(lib, GOW3_STR(unw_get_reg)));
    if (init_fn && step_fn && get_reg_fn) {
        p_unw_init_local2 = init_fn;
        p_unw_step = step_fn;
        p_unw_get_reg = get_reg_fn;
    }
}
#endif
std::string names[kThreads];
std::vector<std::pair<u64, u64>> host;  // loaded modules: only their code is unwound (no lookup of guest or JIT code)
std::jthread sampler;
struct Extra { u64 n, make_ns, makes, check_ns, checks; };  // per GPU frame: Gpu::temp_* since the frame before
std::unique_ptr<Extra[]> extra;
std::atomic<u64> extras{0};

bool Host(u64 a) { return std::any_of(host.begin(), host.end(), [a](auto r) { return a >= r.first && a < r.second; }); }

#ifdef _WIN32
u64 ImageEnd(u64 b) {
    return b + reinterpret_cast<const IMAGE_NT_HEADERS*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew)->OptionalHeader.SizeOfImage;
}
// A frame without unwind data that is not a leaf gives a wrong RSP: the walk stops at the fault it may cause.
void Walk(CONTEXT c, u64* ret) {
    __try {
        for (int i = 0; i < kDepth && Host(c.Rip); ++i) {
            DWORD64 base;
            if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr)) {
                void* data;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
            } else {
                c.Rip = *reinterpret_cast<const u64*>(c.Rsp), c.Rsp += 8;  // leaf function
            }
            ret[i] = c.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void Modules() {  // with no thread suspended
    HMODULE m[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), m, sizeof(m), &bytes)) return;
    host.clear();
    for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i) host.push_back({u64(m[i]), ImageEnd(u64(m[i]))});
}
#else
void Modules() {
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void*) {
        u64 end = info->dlpi_addr;
        for (unsigned i = 0; i < info->dlpi_phnum; ++i)
            if (const auto& phdr = info->dlpi_phdr[i]; phdr.p_type == PT_LOAD)
                end = std::max(end, u64(info->dlpi_addr + phdr.p_vaddr + phdr.p_memsz));
        host.emplace_back(info->dlpi_addr, end);
        return 0;
    }, nullptr);
}

u64 ImageEnd(u64 base) {
    for (const auto& [begin, end] : host)
        if (begin == base) return end;
    return base;
}

struct Module { u64 base; std::string path; };
Module FindModule(u64 address) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fname)
        return {reinterpret_cast<u64>(info.dli_fbase), info.dli_fname};
    return {0, {}};
}
#endif

// GOW3_TEMP_PROBE=slow: only the submitting game thread, and only once its frame has run for kSlowNs: a long frame's own
// samples, with no cost (and no suspended thread) in the normal ones.
bool slow_only = false, blocks_only = false;  // set by Start, before the sampler runs
constexpr u64 kSlowNs = 5'000'000, kSlowBlocksNs = 3'000'000;
std::atomic<u64> frame_start{0};  // DiagClock() when the game thread last submitted
std::atomic<u64> blocks_start{0};  // TEMP: DiagClock() when the current parallel recording began, 0 outside it

#ifdef _WIN32
void Loop(const std::stop_token& token) {
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ULONG64 cycles[kThreads]{};
    u64 loops = 0;
    for (LARGE_INTEGER due{.QuadPart = slow_only ? -10000 : -5000}; !token.stop_requested() && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);) {
        WaitForSingleObject(timer, INFINITE);
        if (loops++ % 2000 == 0) Modules();
        const u64 phase = blocks_start.load(std::memory_order_relaxed);
        if (blocks_only && (!phase || DiagClock() - phase < kSlowBlocksNs)) continue;
        for (u32 k = blocks_only ? 1 : 0; k < (blocks_only ? kThreads : 2); ++k) {
            if (slow_only && (k == 1 || DiagClock() - frame_start.load(std::memory_order_relaxed) < kSlowNs)) continue;
            const HANDLE h = threads[k].load(std::memory_order_acquire);
            if (!h || SuspendThread(h) == DWORD(-1)) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            ULONG64 cyc = 0;
            QueryThreadCycleTime(h, &cyc);
            if (GetThreadContext(h, &c)) {
                Sample& x = ring[count.load(std::memory_order_relaxed) % kRing];
                x = {DiagClock(), phase, c.Rip, {}, k, cyc != cycles[k]};
                Walk(c, x.ret);
                count.fetch_add(1, std::memory_order_release);
            }
            ResumeThread(h);
            cycles[k] = cyc;
        }
    }
}
#else
void SampleThread(int, siginfo_t*, void* context) {
    if (capture_thread >= kThreads) return;
    auto& thread = threads[capture_thread];
    if (thread.state.load(std::memory_order_acquire) != Requested) return;
    auto& sample = thread.sample;
    sample = {DiagClock(), blocks_start.load(std::memory_order_relaxed),
              reinterpret_cast<u64>(Common::GetRip(context)), {}, capture_thread, 0};
    unw_cursor_t cursor;
    if (p_unw_init_local2 && Host(sample.rip) &&
        p_unw_init_local2(&cursor, static_cast<unw_context_t*>(context), UNW_INIT_SIGNAL_FRAME) >= 0) {
        u64 ip = sample.rip;
        for (int i = 0; i < kDepth && Host(ip) && p_unw_step(&cursor) > 0; ++i) {
            unw_word_t value;
            if (p_unw_get_reg(&cursor, UNW_REG_IP, &value) != 0) break;
            sample.ret[i] = ip = value;
        }
    }
    thread.state.store(Ready, std::memory_order_release);
}

void Loop(const std::stop_token& token) {
    u64 last_cpu[kThreads]{};
    const auto pid = getpid();
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(slow_only ? std::chrono::milliseconds(1) : std::chrono::microseconds(500));
        const u64 now = DiagClock(), phase = blocks_start.load(std::memory_order_relaxed);
        for (u32 k = blocks_only ? 1 : 0; k < (blocks_only ? kThreads : 2); ++k) {
            auto& thread = threads[k];
            const pid_t tid = thread.tid.load(std::memory_order_acquire);
            if (!tid) continue;
            if (thread.state.load(std::memory_order_acquire) == Ready) {
                auto& sample = thread.sample;
                timespec time{};
                clock_gettime(thread.clock, &time);
                const u64 cpu = u64(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
                sample.ran = cpu != std::exchange(last_cpu[k], cpu);
                const auto n = count.load(std::memory_order_relaxed);
                ring[n % kRing] = sample;
                count.store(n + 1, std::memory_order_release);
                thread.state.store(Idle, std::memory_order_release);
            }
            if ((blocks_only && (!phase || now - phase < kSlowBlocksNs)) ||
                (slow_only && (k == 1 || now - frame_start.load(std::memory_order_relaxed) < kSlowNs)))
                continue;
            auto state = Idle;
            if (thread.state.compare_exchange_strong(state, Requested, std::memory_order_acq_rel) &&
                syscall(SYS_tgkill, pid, tid, capture_signal))
                thread.state.store(Idle, std::memory_order_release);
        }
    }
}
#endif

// TEMP blocks mode: each long parallel recording's samples with their host stacks (DbgHelp, at exit), then the most sampled
// leaf + first frame of ours.
void WriteBlocks(const std::string& path, u64 from, u64 to) {
    std::string out;
#ifdef _WIN32
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    const u64 ours = u64(GetModuleHandleW(nullptr));
#else
    const u64 ours = FindModule(reinterpret_cast<u64>(&WriteBlocks)).base;
#endif
    const u64 ours_end = ImageEnd(ours);
    std::map<u64, std::string> cache;
    const auto name = [&](u64 a) -> const std::string& {
        auto [it, added] = cache.try_emplace(a);
        if (!added) return it->second;
#ifdef _WIN32
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO), sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_MODULE64 mod{sizeof(mod)};
        const bool known = SymGetModuleInfo64(proc, a, &mod);
        if (SymFromAddr(proc, a, &disp, sym)) it->second = std::format("{}!{}", known ? mod.ModuleName : "?", std::string_view(sym->Name).substr(0, 100));
        else it->second = std::format("{}+{:x}", known ? mod.ModuleName : "?", known ? a - mod.BaseOfImage : a);
        IMAGEHLP_LINE64 line{sizeof(line)};
        DWORD ld = 0;
        if (SymGetLineFromAddr64(proc, a, &ld, &line)) {
            const char* file = std::strrchr(line.FileName, '\\');
            it->second += std::format(" ({}:{})", file ? file + 1 : line.FileName, line.LineNumber);
        }
#else
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(a), &info) && info.dli_fname) {
            const auto file = std::filesystem::path(info.dli_fname).filename().string();
            it->second = info.dli_sname ? std::format("{}!{}", file, info.dli_sname)
                                       : std::format("{}+{:x}", file, a - reinterpret_cast<u64>(info.dli_fbase));
        } else {
            it->second = std::format("?+{:x}", a);
        }
#endif
        return it->second;
    };
    std::map<std::string, u64> where;
    u64 phase = ~0ull, phases = 0;
    for (u64 i = from; i < to; ++i) {
        const Sample& x = ring[i % kRing];
        if (x.phase != phase) phase = x.phase, ++phases, out += std::format("\n== parallel recording at {} ns\n", phase);
        std::string st = name(x.rip), mine = (x.rip >= ours && x.rip < ours_end) ? name(x.rip) : "";
        for (int j = 0; j < kDepth && x.ret[j]; ++j) {
            st += " <- " + name(x.ret[j]);
            if (mine.empty() && x.ret[j] >= ours && x.ret[j] < ours_end) mine = name(x.ret[j]);
        }
        out += std::format("  +{:6.2f} ms T{:<2} {:<18} {} {}\n", double(x.t - x.phase) * 1e-6, x.thread, names[x.thread], x.ran ? "ran " : "idle", st);
        ++where[std::format("{} {} | {}", x.ran ? "ran " : "idle", name(x.rip), mine)];
    }
    std::vector<std::pair<std::string, u64>> top(where.begin(), where.end());
    std::ranges::sort(top, [](auto& a, auto& b) { return a.second > b.second; });
    out += std::format("\n{} samples in {} long parallel recordings; by leaf | first frame of ours:\n", to - from, phases);
    for (size_t i = 0; i < std::min<size_t>(top.size(), 60); ++i) out += std::format("  {:6}  {}\n", top[i].second, top[i].first);
#ifdef _WIN32
    SymCleanup(proc);
#endif
    DumpArchive::Add(path + ".blocks.txt", out);
}
} // namespace

void Start() {
    if (!enabled) return;
    const char* mode = DebugOption("GOW3_TEMP_PROBE");
    blocks_only = mode && !std::strcmp(mode, "blocks");
    slow_only = !blocks_only && (!mode || std::strcmp(mode, "1"));
#ifndef _WIN32
    Modules();
    LoadLibunwind();
    capture_signal = SIGRTMIN + 2;
    struct sigaction action{};
    action.sa_sigaction = SampleThread;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(capture_signal, &action, nullptr) != 0) {
        Log(HostError("Install the CPU probe signal handler"));
        return;
    }
#endif
    ring.reset(new Sample[kRing]), extra.reset(new Extra[kRing]);
    sampler = std::jthread(Loop);
}

void SubmitStarted() {
    if (enabled) frame_start.store(DiagClock(), std::memory_order_relaxed);
}

void Frame(u64 n) {
    if (!enabled) return;
#ifndef _WIN32
    if (!extra) return;
#endif
    const u64 i = extras.load(std::memory_order_relaxed);
    extra[i % kRing] = {n, Gpu::temp_make_ns.exchange(0), Gpu::temp_makes.exchange(0), Gpu::temp_check_ns.exchange(0), Gpu::temp_checks.exchange(0)};
    extras.store(i + 1, std::memory_order_release);
}

void BlocksBegin() {
    if (enabled) blocks_start.store(DiagClock(), std::memory_order_relaxed);
}
void BlocksEnd() {
    if (enabled) blocks_start.store(0, std::memory_order_relaxed);
}

void Register(u32 k) {
    if (!enabled || k >= kThreads) return;
#ifdef _WIN32
    if (threads[k].load(std::memory_order_relaxed)) return;
    names[k] = std::string(Common::GetCurrentThreadNameView());
    HANDLE h{};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
    threads[k].store(h, std::memory_order_release);
#else
    auto& thread = threads[k];
    if (!ring || thread.tid.load(std::memory_order_relaxed) || pthread_getcpuclockid(pthread_self(), &thread.clock)) return;
    names[k] = Common::GetCurrentThreadNameView();
    capture_thread = k;
    thread.tid.store(gettid(), std::memory_order_release);
#endif
}

void Write(const std::string& path) {
    if (!enabled) return;
#ifndef _WIN32
    if (!sampler.joinable()) return;
#endif
    sampler.request_stop(), sampler.join();
    const u64 end = count.load(std::memory_order_acquire), n = std::min(end, kRing);
    std::vector<u8> probe_bin;
    probe_bin.reserve(8 + n * sizeof(Sample));
    const auto append_raw = [](std::vector<u8>& dst, const void* p, size_t sz) {
        const auto* b = static_cast<const u8*>(p);
        dst.insert(dst.end(), b, b + sz);
    };
    append_raw(probe_bin, &n, 8);
    std::set<u64> bases;
    const auto* eboot = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    std::string probe_txt = std::format("threads {}|{}\neboot {:x}\n", names[0], names[1], eboot ? eboot->GetBaseAddress() : 0ull);
    for (u64 i = end - n; i < end; ++i) {
        const Sample& x = ring[i % kRing];
        append_raw(probe_bin, &x, sizeof(x));
        for (int j = -1; j < kDepth; ++j) {
            const u64 a = j < 0 ? x.rip : x.ret[j];
#ifdef _WIN32
            HMODULE m{};
            if (!a || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) ||
                !bases.insert(u64(m)).second) continue;
            char name[MAX_PATH]{};
            GetModuleFileNameA(m, name, MAX_PATH);
            probe_txt += std::format("module {:x} {:x} {}\n", u64(m), ImageEnd(u64(m)), name);
#else
            if (!a) continue;
            const auto module = FindModule(a);
            if (module.path.empty() || !bases.insert(module.base).second) continue;
            probe_txt += std::format("module {:x} {:x} {}\n", module.base, ImageEnd(module.base), module.path);
#endif
        }
    }
    DumpArchive::Add(path + ".probe", probe_bin);
    DumpArchive::Add(path + ".probe.txt", probe_txt);
    if (blocks_only) WriteBlocks(path, end - n, end);
    if (slow_only && eboot) {  // where the long frames ran: samples by eboot-relative address (exact, then 256-byte blocks), most first
        std::map<u64, u64> exact, block;
        std::map<std::pair<std::string, u64>, u64> host_hits;  // outside the eboot: module and offset, to resolve with its symbols
        u64 in_eboot = 0;
        for (u64 i = end - n; i < end; ++i) {
            const u64 rip = ring[i % kRing].rip, rel = rip - eboot->GetBaseAddress();
            if (rel < (1ull << 28)) {
                ++in_eboot, ++exact[rel], ++block[rel & ~0xFFull];
                continue;
            }
#ifdef _WIN32
            HMODULE m{};
            char name[MAX_PATH]{};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip, &m))
                GetModuleFileNameA(m, name, MAX_PATH);
            ++host_hits[{name[0] ? std::string(name) : "?", name[0] ? rip - u64(m) : rip}];
#else
            const auto module = FindModule(rip);
            ++host_hits[{module.path.empty() ? "?" : module.path, rip - module.base}];
#endif
        }
        std::string slow = std::format("{} samples of the game thread in frames longer than {} ms, {} in the eboot (addresses are offsets from its base; Ghidra VA = offset + image base)\n",
                                       n, kSlowNs / 1'000'000, in_eboot);
        for (auto* m : {&block, &exact}) {
            std::vector<std::pair<u64, u64>> top(m->begin(), m->end());
            std::ranges::sort(top, [](auto& a, auto& b) { return a.second > b.second; });
            slow += std::format("\n{}:\n", m == &block ? "256-byte blocks" : "exact addresses");
            for (size_t i = 0; i < std::min<size_t>(top.size(), 40); ++i) slow += std::format("  {:6}  {:x}\n", top[i].second, top[i].first);
        }
        std::vector<std::pair<std::pair<std::string, u64>, u64>> top(host_hits.begin(), host_hits.end());
        std::ranges::sort(top, [](auto& a, auto& b) { return a.second > b.second; });
        slow += "\noutside the eboot (module, offset):\n";
        for (size_t i = 0; i < std::min<size_t>(top.size(), 40); ++i)
            slow += std::format("  {:6}  {:x}  {}\n", top[i].second, top[i].first.second, top[i].first.first);
        DumpArchive::Add(path + ".slow.txt", slow);
    }
    const u64 e = extras.load(std::memory_order_acquire), m = std::min(e, kRing);
    std::vector<u8> frames_bin;
    frames_bin.reserve(8 + m * sizeof(Extra));
    append_raw(frames_bin, &m, 8);
    for (u64 i = e - m; i < e; ++i) append_raw(frames_bin, &extra[i % kRing], sizeof(Extra));
    DumpArchive::Add(path + ".probe.frames", frames_bin);
}
} // namespace Gow3::CpuProbe
