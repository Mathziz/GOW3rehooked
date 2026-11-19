// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fstream>
#include <set>
#include <thread>
#include <sched.h>
#include <sys/resource.h>
#endif
#include "debug/debug.h"

// The processors this process may run on and what its threads make of them: read once, before main, and logged by
// entry.cpp. Nothing else counts processors. The game's side is fixed, Game:Main and its five job workers (a console core
// each); our pools are sized here, and the threads that carry a frame get a seat: the logical processor the scheduler
// tries first for them, never a restriction.
namespace Gow3::Cpu {
struct Plan {
    unsigned logical = 0, cores = 0, fast = 0;  // logical processors, physical cores, cores of the highest efficiency class
    bool limited = false;                        // by the process affinity (a launcher's, or `start /affinity` to try a smaller CPU)
    unsigned recorders = 0;                      // recording threads, the GPU thread included
    unsigned shader_threads = 0;                 // shader compilation and the scans of a shader build
    unsigned decode_threads = 0;                 // a cinematic's frame threads
    bool priority = true;                        // Game:Main and the GPU thread above the pools (GOW3_CPU_PRIORITY=0: as the rest)
#ifdef _WIN32
    std::vector<PROCESSOR_NUMBER> seats;         // by Role (GOW3_CPU_SEATS=0: none)
#endif
};

inline const Plan plan = [] {
    Plan p;
#ifdef _WIN32
    struct Processor { PROCESSOR_NUMBER number; unsigned core, speed; };
    std::vector<Processor> all;
    DWORD_PTR mine = 0, system = 0;  // both zero when the process spans processor groups
    USHORT groups = 1, group = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &mine, &system);
    GetProcessGroupAffinity(GetCurrentProcess(), &groups, &group);
    p.limited = mine && mine != system;
    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
    std::vector<char> sets(size);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), size, &size, GetCurrentProcess(), 0)) size = 0;
    for (ULONG at = 0; at < size;) {
        const auto& set = *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(sets.data() + at);
        at += set.Size;
        const auto& c = set.CpuSet;
        if (set.Type != CpuSetInformation || (c.Allocated && !c.AllocatedToTargetProcess) ||
            (p.limited && (c.Group != group || !(mine >> c.LogicalProcessorIndex & 1))))
            continue;
        all.push_back({{c.Group, c.LogicalProcessorIndex}, unsigned(c.Group) << 8 | c.CoreIndex, c.EfficiencyClass});
    }
    p.logical = unsigned(all.size());
    if (all.empty()) p.logical = p.cores = p.fast = std::max(1ul, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    // The fastest cores first, a core's processors together.
    std::ranges::sort(all, [](const Processor& a, const Processor& b) {
        return std::tie(b.speed, a.core, a.number.Number) < std::tie(a.speed, b.core, b.number.Number);
    });
    std::vector<std::pair<unsigned, PROCESSOR_NUMBER>> second;  // a core's other processors, with the core's position
    for (size_t i = 0; i < all.size(); ++i) {
        if (i && all[i].core == all[i - 1].core) second.push_back({p.cores - 1, all[i].number});
        else p.seats.push_back(all[i].number), ++p.cores, p.fast += all[i].speed == all[0].speed;
    }
    // Seats in Role order: a core each while they last (Game:Main, the GPU thread, the job workers, the recorders), then
    // the second processors, each beside a thread of the other side while it can be: the GPU thread's goes first when
    // a job worker is left without a core (six cores or fewer), then the workers' and the recorders' go to the recorders,
    // then the GPU thread's if still free, and Game:Main's last.
    std::ranges::stable_sort(second, {}, [&](const auto& s) { return s.first == 0 ? ~0u : s.first == 1 ? (p.cores > 6 ? ~0u - 1 : 0u) : s.first; });
    for (const auto& s : second) p.seats.push_back(s.second);
#else
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
        std::set<std::pair<int, int>> cores;
        for (unsigned i = 0; i < CPU_SETSIZE; ++i) {
            if (!CPU_ISSET(i, &allowed)) continue;
            const auto topology = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
            int package, core;
            if ((std::ifstream(topology + "physical_package_id") >> package) &&
                (std::ifstream(topology + "core_id") >> core) && package >= 0 && core >= 0)
                cores.emplace(package, core);
            else
                ++p.cores;
            ++p.logical;
        }
        p.cores += unsigned(cores.size());
        p.limited = p.logical < std::thread::hardware_concurrency();
    }
    if (!p.logical) p.logical = p.cores = std::max(1u, std::thread::hardware_concurrency());
    p.fast = p.cores;
#endif

    // Recording: a thread per core. Each one past six costs the GPU ~0.06 ms at 4K (its blocks end and reopen passes)
    // for a recording time that only falls as 1/n, so the count stops at eight. GOW3_RECORD_THREADS forces it (1: serial).
    const char* forced = DebugOption("GOW3_RECORD_THREADS");
    p.recorders = forced ? std::clamp(std::atoi(forced), 1, 32) : std::clamp(p.cores, 2u, 8u);
    p.shader_threads = p.logical;  // independent programs: every processor
    // A frame thread costs a picture of latency, which the game's 24-30 slot queue has to absorb.
    p.decode_threads = std::clamp(p.logical, 2u, 16u);
    const auto off = [](const char* name) { const char* e = std::getenv(name); return e && !std::strcmp(e, "0"); };
    p.priority = !off("GOW3_CPU_PRIORITY");
#ifdef _WIN32
    if (off("GOW3_CPU_SEATS")) p.seats.clear();
#endif
    return p;
}();

// A thread's seat: Game:Main, the GPU thread, the job workers (index 0..4), the recording threads (index 0..). Once the
// seats run out, a thread shares one of the job workers' (from the last one back, then forth): Game:Main's and the GPU
// thread's stay their own while there is another.
enum class Role : unsigned { Main = 0, GpuThread = 1, Worker = 2, Recorder = 7 };
#ifdef _WIN32
inline PROCESSOR_NUMBER Seat(Role role, unsigned index = 0) {
    const unsigned n = unsigned(plan.seats.size()), s = unsigned(role) + index;
    if (s < n) return plan.seats[s];
    const unsigned lo = std::min(2u, n - 1), hi = std::min(6u, n - 1), count = hi - lo + 1, k = (s - n) % (2 * count);
    return plan.seats[k < count ? hi - k : lo + k - count];
}

// The calling thread takes its role, once, as it starts. The two serial chains of a frame (Game:Main and the GPU thread)
// run above the pools that serve them, as Game:Main does on the console (priority 281 against its workers' 700).
inline void Place(Role role, unsigned index = 0) {
    if (plan.priority && (role == Role::Main || role == Role::GpuThread)) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    if (plan.seats.empty()) return;
    PROCESSOR_NUMBER seat = Seat(role, index);
    SetThreadIdealProcessorEx(GetCurrentThread(), &seat, nullptr);
}

inline std::string Describe() {
    const auto seat = [](Role role, unsigned index = 0) {
        const PROCESSOR_NUMBER n = Seat(role, index);
        return n.Group ? std::format("{}.{}", n.Group, n.Number) : std::to_string(n.Number);
    };
    std::string seats = "none";
    if (!plan.seats.empty()) {
        seats = "Game:Main " + seat(Role::Main) + ", GPU thread " + seat(Role::GpuThread) + ", workers";
        for (unsigned i = 0; i < 5; ++i) seats += ' ' + seat(Role::Worker, i);
        seats += ", recorders";
        for (unsigned i = 0; i + 1 < plan.recorders; ++i) seats += ' ' + seat(Role::Recorder, i);
    }
    return std::format("CPU: {} cores ({} fast), {} logical processors{}; {} recording threads, {} shader threads, {} video decode threads; "
                       "Game:Main and GPU thread priority {}; seats (logical processor): {}",
                       plan.cores, plan.fast, plan.logical, plan.limited ? " (process affinity)" : "", plan.recorders, plan.shader_threads,
                       plan.decode_threads, plan.priority ? "above normal" : "normal", seats);
}
#else
inline void Place(Role role, unsigned = 0) {
    if (plan.priority && (role == Role::Main || role == Role::GpuThread))
        setpriority(PRIO_PROCESS, 0, -5); // best effort under the user's RLIMIT_NICE
}

inline std::string Describe() {
    return std::format("CPU: {} cores, {} logical processors{}; {} recording threads, {} shader threads, {} video decode threads",
                       plan.cores, plan.logical, plan.limited ? " (process affinity)" : "", plan.recorders,
                       plan.shader_threads, plan.decode_threads);
}
#endif
} // namespace Gow3::Cpu
