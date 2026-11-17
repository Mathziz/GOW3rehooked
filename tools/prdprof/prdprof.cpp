// prdprof: AMD IBS analysis of one process from a raw uProf capture (AMDuProfCLI collect --config ibs): threads,
// functions, instructions and data lines, with guest (eboot) code named from the Ghidra map. uProf's own translation
// drops every sample outside a PE module; this reads the raw records (.prd) and the process/module table (.ti) itself.
// prdprof --snapshot <pid> <dir>: thread names and memory map of the live process, for the analysis (capture.ps1).
// Formats are reverse-engineered from uProf 5.3 (skill uprof-ibs); IBS register bits per the Family 19h PPR.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
constexpr u64 KernelBase = 0xFFFF800000000000ull;
// uProf's ibs config samples PMCx076 every 1M cycles; the interval is not in the files (two always-busy test threads
// gave 4390 samples/s each, i.e. 4.39 GHz at 1M).
constexpr u64 kPmcPeriod = 1000000;

template <class T> static T Rd(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

static std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((u8)c);
    return s;
}

static std::wstring Wide(const u8* p, u32 n) {
    std::wstring s(n, 0);
    std::memcpy(s.data(), p, 2ull * n);
    return s;
}

static std::string FileName(const std::wstring& path) { return Lower(Utf8(path.substr(path.find_last_of(L'\\') + 1))); }

static std::vector<u8> Load(const fs::path& p) {
    std::ifstream in{p, std::ios::binary | std::ios::ate};
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::vector<u8> v((size_t)in.tellg());
    in.seekg(0).read(reinterpret_cast<char*>(v.data()), (std::streamsize)v.size());
    return v;
}

static std::wstring DosPath(const std::wstring& nt) {
    static const auto devices = [] {
        std::vector<std::pair<std::wstring, std::wstring>> v;
        for (wchar_t c = L'A'; c <= L'Z'; c++) {
            const wchar_t drive[3] = {c, L':', 0};
            wchar_t target[MAX_PATH];
            if (QueryDosDeviceW(drive, target, MAX_PATH)) v.push_back({std::wstring(target) + L"\\", std::wstring(drive) + L"\\"});
        }
        return v;
    }();
    for (const auto& [device, drive] : devices)
        if (_wcsnicmp(nt.c_str(), device.c_str(), device.size()) == 0) return drive + nt.substr(device.size());
    return nt;
}

// --snapshot: threads.tsv (tid, name) and regions.tsv (VirtualQueryEx: base, size, allocation base, state, type,
// protection, mapped file) of a live process.
static int Snapshot(u32 pid, const fs::path& dir) {
    const HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return fprintf(stderr, "OpenProcess(%u) failed: %lu\n", pid, GetLastError()), 1;
    fs::create_directories(dir);
    FILE* f = _wfopen((dir / "threads.tsv").c_str(), L"wb");
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te};
    u32 threads = 0, regions = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        std::string name;
        if (const HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
            PWSTR d = nullptr;
            if (SUCCEEDED(GetThreadDescription(t, &d)) && d) name = Utf8(d), LocalFree(d);
            CloseHandle(t);
        }
        fprintf(f, "%lu\t%s\n", te.th32ThreadID, name.c_str()), threads++;
    }
    CloseHandle(snap);
    fclose(f);
    f = _wfopen((dir / "regions.tsv").c_str(), L"wb");
    MEMORY_BASIC_INFORMATION m;
    wchar_t path[1024];
    for (u64 a = 0; VirtualQueryEx(p, reinterpret_cast<void*>(a), &m, sizeof m); a = (u64)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_FREE) continue;
        std::string file;
        if (m.Type != MEM_PRIVATE && GetMappedFileNameW(p, m.BaseAddress, path, (DWORD)std::size(path))) file = Utf8(DosPath(path));
        fprintf(f, "%llx\t%llx\t%llx\t%lx\t%lx\t%lx\t%s\n", (u64)m.BaseAddress, (u64)m.RegionSize, (u64)m.AllocationBase, m.State,
                m.Type, m.Protect, file.c_str());
        regions++;
    }
    fclose(f);
    CloseHandle(p);
    printf("snapshot: %u threads, %u regions -> %s\n", threads, regions, dir.string().c_str());
    return 0;
}

struct Module {
    u64 base, size;
    std::wstring path;
    std::string name;
    bool loaded = false, ok = false;
};

// .ti: a process table (u32 count, then records: u32 pid, +0x18 u32 name length, +0x1c UTF-16 name), found by its first
// record "System Idle Process"; module records (u32 pid, +8 u64 base, +0x20 u64 size, +0x30 u32 path length, +0x34
// UTF-16 path), found by their shape. Kernel modules have base 0 without elevation.
struct Ti {
    std::unordered_map<u32, std::string> proc;
    std::unordered_map<u32, std::vector<Module>> mods;
};

static bool Printable(const u8* p, u32 n) {
    for (u32 i = 0; i < n; i++)
        if (Rd<u16>(p + 2 * i) < 0x20) return false;
    return true;
}

static Ti ReadTi(const fs::path& file) {
    const auto d = Load(file);
    const u8* b = d.data();
    const size_t n = d.size();
    const std::wstring_view idle = L"System Idle Process";
    size_t at = 0;
    for (size_t o = 0x20; !at && o + idle.size() * 2 <= n; o += 2)
        if (!std::memcmp(b + o, idle.data(), idle.size() * 2)) at = o - 0x1c;
    if (!at) throw std::runtime_error("no process table in " + file.string());
    Ti t;
    for (size_t o = at, count = Rd<u32>(b + at - 4); count-- && o + 0x1c <= n;) {
        const u32 len = Rd<u32>(b + o + 0x18);
        if (o + 0x1c + 2ull * len > n || !Printable(b + o + 0x1c, len)) break;
        t.proc[Rd<u32>(b + o)] = FileName(Wide(b + o + 0x1c, len));
        o += 0x1c + 2ull * len;
    }
    for (size_t o = at; o + 0x34 <= n; o += 2) {
        const u32 len = Rd<u32>(b + o + 0x30), pid = Rd<u32>(b + o);
        const u64 base = Rd<u64>(b + o + 8), size = Rd<u64>(b + o + 0x20);
        const u8* s = b + o + 0x34;
        if (len < 4 || len > 1024 || o + 0x34 + 2ull * len > n || !base || base >= KernelBase || !size || size >> 32 ||
            Rd<u16>(s + 2) != ':' || Rd<u16>(s + 4) != '\\' || !Printable(s, len) || !t.proc.contains(pid))
            continue;
        Module m{base, size, Wide(s, len)};
        m.name = FileName(m.path);
        t.mods[pid].push_back(std::move(m));
        o += 0x34 + 2ull * len - 2;
    }
    return t;
}

// .prd: records of u8 type, u8 0, u16 length, u32 CPU, chained by length; type 0x45 = sample with +8 tick (100 ns),
// +16 event config (the programmed IbsOpCtl / IbsFetchCtl / PerfEvtSel), +24 pid, +32 tid, +40 interrupt RIP, then the
// event's registers. IBS Op (112 B): +48 IbsOpCtl,
// +56 IbsOpData, +64 IbsOpRip, +72 IbsOpData2, +80 IbsOpData3, +88 IbsDcLinAd, +96 IbsDcPhysAd, +104 IbsBrTarget.
// IBS Fetch (72/80 B): +48 IbsFetchCtl, +56 IbsFetchLinAd. 48 B: PMCx076 (cycles not halted) sample, interrupt RIP
// only. Type 0x4a (40 B, at the end, +4 = 0xffff000b): the configuration of each event.
struct Op { u64 tick, rip, irip, data, data2, data3, lin, pid; u32 tid; };
struct Fetch { u64 tick, ctl, lin, pid; u32 tid; };
struct Pmc { u64 rip, pid; u32 tid; };
struct Prd {
    std::vector<Op> ops;
    std::vector<Fetch> fetches;
    std::vector<Pmc> pmcs;
    std::unordered_map<u64, u64> pidOps;
    std::vector<u64> cpuOps = std::vector<u64>(1024), cpuPmc = std::vector<u64>(1024);  // all processes
    u64 configs = 0, other = 0, resyncs = 0, skipped = 0, first = ~0ull, last = 0, opPeriod = 0, fetchPeriod = 0;
};

static bool Header(const u8* p, const u8* end) {
    if (p + 16 > end || (p[0] != 0x45 && p[0] != 0x4a) || p[1] || (p[0] == 0x45 && Rd<u32>(p + 4) >= 1024)) return false;
    const u16 n = Rd<u16>(p + 2);
    return n >= 16 && !(n & 7) && p + n <= end;
}

static bool Chain(const u8* p, const u8* end, int k) {
    for (; k--; p += Rd<u16>(p + 2))
        if (!Header(p, end)) return p == end;
    return true;
}

static Prd ReadPrd(const fs::path& file) {
    const auto d = Load(file);
    const u8 *b = d.data(), *end = b + d.size();
    size_t o = 0;
    while (o + 16 <= d.size() && !Chain(b + o, end, 16)) o += 8;
    Prd p;
    while (o + 16 <= d.size()) {
        const u8* r = b + o;
        if (!Header(r, end)) {
            const size_t from = o;
            for (o += 8; o + 16 <= d.size() && !Chain(b + o, end, 8);) o += 8;
            p.resyncs++, p.skipped += o - from;
            continue;
        }
        const u16 len = Rd<u16>(r + 2);
        o += len;
        if (r[0] != 0x45) { p.configs++; continue; }
        const u64 tick = Rd<u64>(r + 8), config = Rd<u64>(r + 16), pid = Rd<u64>(r + 24);
        const u32 tid = (u32)Rd<u64>(r + 32), cpu = Rd<u32>(r + 4);
        p.first = std::min(p.first, tick), p.last = std::max(p.last, tick);
        p.cpuOps[cpu] += len == 112, p.cpuPmc[cpu] += len == 48;
        if (len == 112) {
            const u64 data = Rd<u64>(r + 56);
            p.opPeriod = ((config & 0xffff) | (config >> 20 & 0x7f) << 16) << 4;
            p.ops.push_back({tick, data >> 38 & 1 ? 0 : Rd<u64>(r + 64), Rd<u64>(r + 40), data, Rd<u64>(r + 72), Rd<u64>(r + 80), Rd<u64>(r + 88), pid, tid});
            p.pidOps[pid]++;
        } else if (len == 72 || len == 80) {
            p.fetchPeriod = (config & 0xffff) << 4;
            p.fetches.push_back({tick, Rd<u64>(r + 48), Rd<u64>(r + 56), pid, tid});
        } else if (len == 48) {
            p.pmcs.push_back({Rd<u64>(r + 40), pid, tid});
        } else {
            p.other++;
        }
    }
    return p;
}

// Per-group metrics. IbsOpData: [15:0] completion to retire, [31:16] tag to retire cycles, 35 taken, 36 mispredicted,
// 37 branch retired, 38 RIP invalid. IbsOpData3: 0 load, 1 store, 2 DTLB L1 miss, 3 DTLB L2 miss, 7 DC miss,
// 15 locked, 17 linear address valid, [47:32] DC miss latency. IbsOpData2 [2:0]: source of an L2-missing load
// (1 L3/other core of the CCX, 2 other CCX, 3 DRAM, 5 far CCX, 6 DRAM long latency, 7 IO). IbsFetchCtl: [47:32] fetch
// latency, 50 complete, 51 IC miss, 55 ITLB L1 miss, 56 ITLB L2 miss.
enum { Cyc, Ops, T2r, Ld, St, L1m, L2m, Dram, Lat, LatN, Tlb1, Tlb2, Br, Misp, Lock, Fe, Icm, Itlb, Flat, FlatN, NM };
struct M {
    u64 v[NM]{};
    void operator+=(const M& o) {
        for (int i = 0; i < NM; i++) v[i] += o.v[i];
    }
};

static u8 Source(u64 data2) { return u8(data2 & 7); }
static bool Mem(const Op& o) { return o.data3 & 3; }
static bool LoadMiss(const Op& o) { return (o.data3 & 1) && (o.data3 >> 7 & 1); }
static u32 MissLat(const Op& o) { return LoadMiss(o) ? u32(o.data3 >> 32 & 0xffff) : 0; }

static void AddOp(M& m, const Op& o) {
    const u64 d = o.data, d3 = o.data3;
    m.v[Ops]++, m.v[T2r] += d >> 16 & 0xffff;
    if (d >> 37 & 1) m.v[Br]++, m.v[Misp] += d >> 36 & 1;
    if (!Mem(o)) return;
    m.v[Ld] += d3 & 1, m.v[St] += d3 >> 1 & 1, m.v[Lock] += d3 >> 15 & 1, m.v[Tlb1] += d3 >> 2 & 1, m.v[Tlb2] += d3 >> 3 & 1;
    m.v[L1m] += d3 >> 7 & 1;
    if (!LoadMiss(o)) return;
    const u8 s = Source(o.data2);
    m.v[L2m] += s != 0, m.v[Dram] += s == 3 || s == 6, m.v[Lat] += MissLat(o), m.v[LatN]++;
}

static void AddFetch(M& m, const Fetch& f) {
    const u64 c = f.ctl;
    m.v[Fe]++, m.v[Itlb] += (c >> 55 | c >> 56) & 1;
    if (c >> 51 & 1) m.v[Icm]++, m.v[Flat] += c >> 50 & 1 ? c >> 32 & 0xffff : 0, m.v[FlatN] += c >> 50 & 1;
}

struct Region { u64 base, size, alloc; u32 state, type; std::string path; };

struct Syms {
    HANDLE h = reinterpret_cast<HANDLE>(0x7072);
    std::vector<Module> mods;
    struct Range { u64 begin, end, entry; std::string name; };
    struct Seg { u64 begin, end; std::string name; };
    std::vector<Range> guest;
    std::vector<Seg> segs;
    std::vector<Region> regions;
    std::string guestModule = "eboot";
    u64 guestBase = 0, imageBase = 0;
    std::vector<std::string> names;  // per function id
    std::vector<u16> mod;             // function id -> module id
    std::vector<std::string> modNames{"<invalid rip>", "<kernel>", "<anon>", "<guest>"};
    std::unordered_map<std::string, u32> ids;
    std::unordered_map<u64, u32> addr;
    SYMBOL_INFOW* info = static_cast<SYMBOL_INFOW*>(calloc(1, sizeof(SYMBOL_INFOW) + 2048 * sizeof(wchar_t)));

    Syms(std::vector<Module> m, const std::wstring& path) : mods(std::move(m)) {
        std::sort(mods.begin(), mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
        mods.erase(std::unique(mods.begin(), mods.end(), [](const Module& a, const Module& b) { return a.base == b.base; }), mods.end());
        for (const auto& x : mods) modNames.push_back(x.name);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_LOAD_LINES);
        SymInitializeW(h, path.c_str(), FALSE);
        info->SizeOfStruct = sizeof(SYMBOL_INFOW);
        info->MaxNameLen = 2048;
    }
    void LoadGuest(const fs::path& file) {
        std::ifstream input{file};
        std::string line;
        if (!input || !std::getline(input, line) || line != "# guest-symbols-v1") throw std::runtime_error("cannot read guest map " + file.string());
        while (std::getline(input, line)) {
            char name[256];
            unsigned long long lo, hi, entry;
            int offset = 0;
            if (line.starts_with("# module\t")) guestModule = line.substr(9);
            else if (line.starts_with("# image_base\t")) imageBase = std::stoull(line.substr(13), nullptr, 16);
            else if (line.starts_with("# segment\t") && sscanf_s(line.c_str() + 10, "%255s %llx %llx", name, (unsigned)sizeof name, &lo, &hi) == 3)
                segs.push_back({lo, hi, name});
            else if (!line.empty() && line[0] != '#' && sscanf_s(line.c_str(), "%llx\t%llx\t%llx\t%n", &lo, &hi, &entry, &offset) == 3 && offset)
                guest.push_back({lo, hi, entry, line.substr(offset)});
        }
        if (guest.empty()) throw std::runtime_error("empty guest map " + file.string());
        modNames[3] = guestModule;
    }
    Module* Find(u64 a) {
        const auto it = std::upper_bound(mods.begin(), mods.end(), a, [](u64 x, const Module& m) { return x < m.base; });
        return it == mods.begin() || a >= (it - 1)->base + (it - 1)->size ? nullptr : &*(it - 1);
    }
    const Range* Guest(u64 a) const {
        if (!guestBase || a < guestBase) return nullptr;
        const u64 rva = a - guestBase;
        const auto it = std::upper_bound(guest.begin(), guest.end(), rva, [](u64 r, const Range& g) { return r < g.begin; });
        return it != guest.begin() && rva < (it - 1)->end ? &*(it - 1) : nullptr;
    }
    const Seg* Segment(u64 a) const {
        for (const auto& s : segs)
            if (guestBase && a >= guestBase + s.begin && a < guestBase + s.end) return &s;
        return nullptr;
    }
    u64 Va(u64 a) const { return a - guestBase + imageBase; }
    u32 Intern(std::string n, u16 m) {
        const auto [it, fresh] = ids.try_emplace(std::move(n), (u32)names.size());
        if (fresh) names.push_back(it->first), mod.push_back(m);
        return it->second;
    }
    bool Host(Module& m, u64 a, DWORD64& disp) {
        if (!m.loaded) m.loaded = true, m.ok = SymLoadModuleExW(h, nullptr, m.path.c_str(), nullptr, m.base, (DWORD)m.size, nullptr, 0) != 0;
        return m.ok && SymFromAddrW(h, a, &disp, info);
    }
    // Function id of a code address (cached).
    u32 Fn(u64 a) {
        if (const auto it = addr.find(a); it != addr.end()) return it->second;
        u32 id;
        DWORD64 disp = 0;
        if (!a) id = Intern("<invalid rip>", 0);
        else if (a >= KernelBase) id = Intern("<kernel>", 1);
        else if (const Range* g = Guest(a)) id = Intern(std::format("{}!{} [{:x}]", guestModule, g->name, imageBase + g->entry), 3);
        else if (const Seg* s = Segment(a)) id = Intern(std::format("{}!<{} outside functions>", guestModule, s->name), 3);
        else if (Module* m = Find(a)) id = Intern(Host(*m, a, disp) ? m->name + "!" + Utf8({info->Name, info->NameLen}) : std::format("{}+{:#x}", m->name, a - m->base), u16(4 + (m - mods.data())));
        else id = Intern(std::format("<anon>!{:x}", a >> 20 << 20), 2);
        return addr[a] = id;
    }
    // Instruction address: Ghidra VA for the guest, module offset and source line for host code.
    std::string Code(u64 a) {
        if (Guest(a) || Segment(a)) return std::format("VA {:x}", Va(a));
        Module* m = Find(a);
        if (!m) return std::format("{:x}", a);
        DWORD64 disp = 0;
        IMAGEHLP_LINEW64 l{sizeof l};
        DWORD col = 0;
        const bool line = Host(*m, a, disp) && SymGetLineFromAddrW64(h, a, &col, &l);
        return std::format("{}+{:x}", m->name, a - m->base) + (line ? std::format(" {}:{}", Utf8(fs::path(l.FileName).filename().wstring()), l.LineNumber) : "");
    }
    const Region* RegionOf(u64 a) const {
        const auto it = std::upper_bound(regions.begin(), regions.end(), a, [](u64 x, const Region& r) { return x < r.base; });
        return it == regions.begin() || a >= (it - 1)->base + (it - 1)->size ? nullptr : &*(it - 1);
    }
    // Data address: eboot segment VA, host module symbol, else the allocation it lies in (snapshot).
    std::string Data(u64 a) {
        if (a >= KernelBase) return "<kernel>";
        if (const Seg* s = Segment(a)) return std::format("{} {:x}", s->name, Va(a));
        DWORD64 disp = 0;
        if (Module* m = Find(a))
            return Host(*m, a, disp) && disp < std::max<u64>(info->Size, 1) ? std::format("{}!{}+{:#x}", m->name, Utf8({info->Name, info->NameLen}), disp)
                                                                           : std::format("{}+{:#x}", m->name, a - m->base);
        if (const Region* r = RegionOf(a)) {
            const char* type = r->type == MEM_PRIVATE ? "private" : r->type == MEM_MAPPED ? "mapped" : "image";
            return std::format("{} {:x}+{:x}{}{}", type, r->alloc, a - r->alloc, r->path.empty() ? "" : " ", r->path);
        }
        return std::format("{:x}", a);
    }
};

// -b auto: the 64 KiB-aligned load base that puts the most user samples outside modules on instruction starts of the
// guest map (<map>.starts, one bit per RVA). IBS RIPs are exact, so the right base takes nearly all guest samples.
static u64 GuestBase(const fs::path& map, const std::unordered_map<u64, u64>& ips, std::string& note) {
    std::ifstream input{fs::path(map.wstring() + L".starts"), std::ios::binary};
    const std::vector<char> bits{std::istreambuf_iterator<char>(input), {}};
    if (bits.empty()) throw std::runtime_error("cannot read the guest map's instruction starts (<map>.starts)");
    std::unordered_map<u64, u64> score;
    u64 outside = 0;
    for (const auto& [ip, n] : ips) {
        outside += n;
        for (u64 rva = ip & 0xffff; rva >> 3 < bits.size() && rva <= ip; rva += 0x10000)
            if (bits[rva >> 3] >> (rva & 7) & 1) score[ip - rva] += n;
    }
    std::pair<u64, u64> best{}, next{};
    for (const auto& [base, n] : score)
        if (std::pair{n, base} > best) next = best, best = {n, base};
        else if (std::pair{n, base} > next) next = {n, base};
    note = std::format("{} of {} user samples outside modules on instruction starts (next base {:x}: {})", best.first, outside, next.second, next.first);
    if (best.first < 2 * next.first + 100) throw std::runtime_error("no clear guest load base: " + note);
    return best.second;
}

static std::wstring Arg(int argc, wchar_t** argv, const wchar_t* flag, std::wstring def) {
    for (int i = 2; i + 1 < argc; i++)
        if (!wcscmp(argv[i], flag)) return argv[i + 1];
    return def;
}

template <class K> static std::vector<std::pair<K, const M*>> Top(const std::unordered_map<K, M>& c, int field, size_t n, auto&& keep) {
    std::vector<std::pair<K, const M*>> v;
    for (const auto& [k, m] : c)
        if (m.v[field] && keep(k)) v.push_back({k, &m});
    const size_t k = std::min(n, v.size());
    std::partial_sort(v.begin(), v.begin() + k, v.end(), [&](const auto& a, const auto& b) { return a.second->v[field] > b.second->v[field]; });
    v.resize(k);
    return v;
}

static const char* kHead = "     cyc  cyc%     ops  ops%   t2r  ld%  st%  L1m%    L2m   DRAM   lat  Σlat/k  dtlb  misp  br%  fetch  icm%  ";

// One row; percentages of `of` (the process, or the thread in per-thread tables).
static void Print(const M& m, const M& of, std::string_view name) {
    const auto pct = [](u64 a, u64 b) { return b ? 100.0 * (double)a / (double)b : 0.0; };
    const auto avg = [](u64 a, u64 b) { return b ? (double)a / (double)b : 0.0; };
    printf("%8llu %5.1f %7llu %5.1f %5.0f %4.0f %4.0f %5.1f %6llu %6llu %5.0f %7.0f %5llu %5llu %4.1f %6llu %5.1f  %.*s\n", m.v[Cyc],
           pct(m.v[Cyc], of.v[Cyc]), m.v[Ops], pct(m.v[Ops], of.v[Ops]), avg(m.v[T2r], m.v[Ops]), pct(m.v[Ld], m.v[Ops]), pct(m.v[St], m.v[Ops]),
           pct(m.v[L1m], m.v[Ld] + m.v[St]),
           m.v[L2m], m.v[Dram], avg(m.v[Lat], m.v[LatN]), (double)m.v[Lat] / 1e3, m.v[Tlb1], m.v[Misp], pct(m.v[Misp], m.v[Br]), m.v[Fe],
           pct(m.v[Icm], m.v[Fe]), (int)std::min<size_t>(name.size(), 150), name.data());
}

struct Line { u64 ld = 0, st = 0, miss = 0, lat = 0, dram = 0, readers = 0, writers = 0; };

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && !wcscmp(argv[1], L"--snapshot")) return Snapshot((u32)std::stoul(argv[2]), argv[3]);
    if (argc < 2) {
        fprintf(stderr, "usage: prdprof <session dir|file.prd> [-p gow3.exe|pid] [-e guest.tsv] [-b auto|hex] [-s symdir] [-n top] [-g GHz] [-f function] [-a samples.tsv]\n"
                        "       prdprof --snapshot <pid> <session dir>\n");
        return 2;
    }
    static char out[1 << 20];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    const auto clock0 = std::chrono::steady_clock::now();
    const auto lap = [&](const char* what) {
        fprintf(stderr, "%-10s %.2f s\n", what, std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count());
    };
    try {
        fs::path prd = argv[1], session = argv[1];
        if (fs::is_directory(prd)) {
            prd.clear();
            for (const auto& e : fs::directory_iterator(session / "cpu"))
                if (e.path().extension() == ".prd" && (prd.empty() || e.file_size() > fs::file_size(prd))) prd = e.path();
            if (prd.empty()) throw std::runtime_error("no cpu\\*.prd in " + session.string());
        } else {
            session = prd.parent_path().parent_path();
        }
        const size_t top = std::stoul(Arg(argc, argv, L"-n", L"25"));
        const double ghz = std::stod(Arg(argc, argv, L"-g", L"4.4"));
        const Ti ti = ReadTi(fs::path(prd).replace_extension(".ti"));
        const Prd p = ReadPrd(prd);
        lap("parse");

        // Target process: a pid, or the most sampled process with that image name.
        const std::string want = Lower(Utf8(Arg(argc, argv, L"-p", L"gow3.exe")));
        u64 pid = ~0ull;
        if (want.find_first_not_of("0123456789") == std::string::npos) pid = std::stoull(want);
        else
            for (const auto& [id, n] : p.pidOps)
                if (const auto it = ti.proc.find((u32)id); it != ti.proc.end() && it->second == want && (pid == ~0ull || n > p.pidOps.at(pid))) pid = id;
        if (!p.pidOps.contains(pid)) {
            fprintf(stderr, "no op samples of %s; processes by samples:", want.c_str());
            std::vector<std::pair<u64, u64>> v;
            for (const auto& [id, n] : p.pidOps) v.push_back({n, id});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < std::min<size_t>(10, v.size()); i++)
                fprintf(stderr, " %llu=%s:%llu", v[i].second, ti.proc.contains((u32)v[i].second) ? ti.proc.at((u32)v[i].second).c_str() : "?", v[i].first);
            fprintf(stderr, "\n");
            return 1;
        }
        std::vector<Op> ops;
        std::vector<Fetch> fetches;
        std::vector<Pmc> pmcs;
        for (const auto& o : p.ops)
            if (o.pid == pid) ops.push_back(o);
        for (const auto& f : p.fetches)
            if (f.pid == pid) fetches.push_back(f);
        for (const auto& c : p.pmcs)
            if (c.pid == pid) pmcs.push_back(c);

        wchar_t exe[MAX_PATH]{};  // the repo's build by default: this tool runs from tools\prdprof
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path build = fs::path(exe).parent_path().parent_path().parent_path() / L"build-fast";
        Syms sym(ti.mods.contains((u32)pid) ? ti.mods.at((u32)pid) : std::vector<Module>{},
                 Arg(argc, argv, L"-s", (build / L"RelWithDebInfo\\bin").wstring()));
        if (std::ifstream regions{session / "regions.tsv"}) {
            std::string line;
            while (std::getline(regions, line)) {
                Region r{};
                int off = 0;
                if (sscanf_s(line.c_str(), "%llx\t%llx\t%llx\t%x\t%x\t%*x\t%n", &r.base, &r.size, &r.alloc, &r.state, &r.type, &off) >= 5) {
                    r.path = off ? line.substr(off) : "";
                    if (const size_t slash = r.path.find_last_of('\\'); slash != std::string::npos) r.path.erase(0, slash + 1);
                    sym.regions.push_back(std::move(r));
                }
            }
        }
        std::unordered_map<u32, std::string> threadName;
        if (std::ifstream threads{session / "threads.tsv"}) {
            std::string line;
            while (std::getline(threads, line))
                if (const size_t tab = line.find('\t'); tab != std::string::npos) threadName[(u32)std::stoul(line.substr(0, tab))] = line.substr(tab + 1);
        }
        // Guest map: -b auto finds the live base (a process without the eboot gets a warning, not an error).
        const fs::path map = Arg(argc, argv, L"-e", (build / L"guest-symbols\\eboot.tsv").wstring());
        std::string guestNote;
        if (const std::wstring base = Arg(argc, argv, L"-b", L"auto"); base != L"none") {
            sym.LoadGuest(map);
            std::unordered_map<u64, u64> ips;
            for (const auto& o : ops)
                if (o.rip && o.rip < KernelBase && !sym.Find(o.rip)) ips[o.rip]++;
            if (base != L"auto") sym.guestBase = std::stoull(base, nullptr, 16), guestNote = "given with -b";
            else try {
                sym.guestBase = GuestBase(map, ips, guestNote);
            } catch (const std::exception& e) {
                guestNote = std::string("WARNING: ") + e.what() + "; guest code is not named";
            }
        }
        lap("load");

        // Aggregation (a few hundred thousand samples: one pass). Time is the PMC cycle samples: IBS op samples go missing
        // while the pipeline is stalled (a DRAM-bound test thread got half its share), so they only give the op metrics.
        // When the PMC was not sampled on every busy CPU (its counters held by another tool), time falls back to the ops.
        // Ops without a valid IbsOpRip (microcode) count for the function at their interrupt RIP.
        std::string noPmc;
        for (u32 c = 0; c < 1024; c++)
            if (p.cpuOps[c] * 100 >= p.ops.size() && p.cpuPmc[c] * 5 < p.cpuOps[c]) noPmc += std::format(" {}", c);
        const bool pmcTime = noPmc.empty() && !pmcs.empty();
        const u64 timePeriod = pmcTime ? kPmcPeriod : p.opPeriod;
        std::unordered_map<u32, M> fn, thr;
        std::unordered_map<u64, M> thrFn, ins;
        std::unordered_map<u16, M> mod;
        std::unordered_map<u64, Line> lines;
        std::unordered_map<u32, u32> tidBit;
        M tot;
        for (const auto& c : pmcs) {
            const u32 f = sym.Fn(c.rip);
            if (pmcTime)
                for (M* m : {&fn[f], &thr[c.tid], &thrFn[(u64)c.tid << 32 | f], &mod[sym.mod[f]], &tot}) m->v[Cyc]++;
        }
        for (const auto& o : ops) {
            const u32 f = sym.Fn(o.rip ? o.rip : o.irip);
            const u64 bit = 1ull << std::min<u32>(63, tidBit.try_emplace(o.tid, (u32)tidBit.size()).first->second);
            M* const at[] = {&fn[f], &thr[o.tid], &thrFn[(u64)o.tid << 32 | f], &mod[sym.mod[f]], &tot};
            for (M* m : at) AddOp(*m, o), m->v[Cyc] += !pmcTime;
            AddOp(ins[o.rip], o);
            if (!Mem(o) || !(o.data3 >> 17 & 1)) continue;
            Line& l = lines[o.lin >> 6];
            const bool ld = o.data3 & 1;
            l.ld += ld, l.st += !ld, (ld ? l.readers : l.writers) |= bit;
            l.miss += o.data3 >> 7 & 1, l.lat += MissLat(o), l.dram += LoadMiss(o) && (Source(o.data2) == 3 || Source(o.data2) == 6);
        }
        for (const auto& fe : fetches) {
            const u32 f = sym.Fn(fe.lin);
            AddFetch(fn[f], fe), AddFetch(thr[fe.tid], fe), AddFetch(thrFn[(u64)fe.tid << 32 | f], fe), AddFetch(mod[sym.mod[f]], fe);
        }
        lap("aggregate");

        // Report.
        const u64 total = ops.size();
        const double dur = (double)(p.last - p.first) / 1e7, coreCycles = dur * ghz * 1e9;
        const auto clip = [](const std::string& s, size_t n = 110) { return s.size() > n ? s.substr(0, n - 3) + "..." : s; };
        const auto label = [&](u32 tid) { return threadName.contains(tid) && !threadName[tid].empty() ? threadName[tid] : "?"; };
        const auto all = [](auto) { return true; };
        printf("prdprof %s\n", prd.string().c_str());
        printf("capture %.2f s: %zu IBS op, %zu IBS fetch, %llu PMC samples, %llu configs, %llu other; op period %llu cycles, fetch period %llu fetches%s\n",
               dur, p.ops.size(), p.fetches.size(), p.pmcs.size(), p.configs, p.other, p.opPeriod, p.fetchPeriod,
               p.resyncs ? std::format("; {} resyncs skipped {} bytes", p.resyncs, p.skipped).c_str() : "");
        printf("process %s pid %llu: %zu PMC, %llu op (%.1f%% of all), %zu fetch samples; %.2f cores; %zu threads, %zu modules; %s, %s\n",
               ti.proc.contains((u32)pid) ? ti.proc.at((u32)pid).c_str() : "?", pid, pmcs.size(), total,
               100.0 * (double)total / (double)std::max<size_t>(1, p.ops.size()), fetches.size(), (double)(tot.v[Cyc] * timePeriod) / coreCycles,
               thr.size(), sym.mods.size(), threadName.empty() ? "no threads.tsv (no thread names)" : "thread names from threads.tsv",
               sym.regions.empty() ? "no regions.tsv" : std::format("{} regions from regions.tsv", sym.regions.size()).c_str());
        if (sym.guestBase)
            printf("guest map %s: %zu ranges, %zu segments; live base %llx (Ghidra image base %llx): %s\n", map.string().c_str(), sym.guest.size(),
                   sym.segs.size(), sym.guestBase, sym.imageBase, guestNote.c_str());
        else if (!guestNote.empty())
            printf("%s\n", guestNote.c_str());
        const auto stamp = fs::last_write_time(prd);
        for (const auto& m : sym.mods)
            if (std::error_code ec; mod.contains(u16(4 + (&m - sym.mods.data()))) && fs::last_write_time(m.path, ec) > stamp && !ec)
                printf("WARNING: %s changed after the capture: its names and lines are of the new build\n", m.name.c_str());
        if (!pmcTime)
            printf("WARNING: no PMC samples on busy CPUs%s (counters held by another tool?): cyc = IBS op samples, which undercount stalled code\n",
                   noPmc.c_str());
        printf("cyc: time, as %s every %llu cycles; cores/core%% assume %.1f GHz. "
               "ops: IBS op samples (exact RIP; undercounted where the pipeline stalls); t2r: mean tag-to-retire cycles; ",
               pmcTime ? "PMCx076 samples (unhalted cycles; interrupt RIP, with skid)" : "IBS op samples", timePeriod, ghz);
        printf(
               "L1m%%: DC misses per load/store; L2m/DRAM: loads served past L2 / by DRAM; lat: mean load-miss latency; "
               "Σlat/k: summed load-miss latency (kcycles, the memory-stall weight); dtlb: L1 DTLB misses; misp/br%%: mispredicted "
               "branches; fetch/icm%%: IBS fetch samples and their I-cache misses.\n");

        printf("\n== Modules\n%s\n", kHead);
        for (const auto& [m, x] : Top(mod, Cyc, 64, all)) Print(*x, tot, sym.modNames[m]);
        for (const auto& [m, x] : mod)
            if (!x.v[Cyc] && (x.v[Ops] || x.v[Fe])) Print(x, tot, sym.modNames[m]);

        printf("\n== Threads: core%%, then metrics, label, top function\n");
        const auto thrTop = Top(thr, Cyc, 1000, all);
        for (const auto& [tid, x] : thrTop) {
            u32 best = 0;
            u64 n = 0;
            for (const auto& [k, y] : thrFn)
                if ((u32)(k >> 32) == tid && y.v[Cyc] > n) n = y.v[Cyc], best = (u32)k;
            printf("%6u %5.1f%% ", tid, 100.0 * (double)(x->v[Cyc] * timePeriod) / coreCycles);
            Print(*x, tot, clip(label(tid), 40) + "  | " + clip(sym.names[best], 70));
        }

        const auto fnTable = [&](const char* title, int field, size_t n) {
            printf("\n== Functions by %s\n%s\n", title, kHead);
            for (const auto& [f, x] : Top(fn, field, n, all)) Print(*x, tot, sym.names[f]);
        };
        fnTable("time (cycle samples)", Cyc, top);
        fnTable("load-miss latency (memory stall)", Lat, top);
        fnTable("DRAM loads", Dram, top / 2);
        fnTable("mispredicted branches", Misp, top / 2);
        fnTable("instruction-fetch misses", Icm, top / 2);

        printf("\n== Per thread (>= 2%% of the process time): top functions, %% of the thread\n");
        for (const auto& [tid, x] : thrTop) {
            if (x->v[Cyc] * 50 < tot.v[Cyc]) continue;
            printf("\n-- %u %s: %.1f%% of a core\n%s\n", tid, label(tid).c_str(), 100.0 * (double)(x->v[Cyc] * timePeriod) / coreCycles, kHead);
            for (const auto& [k, y] : Top(thrFn, Cyc, 12, [&](u64 k) { return (u32)(k >> 32) == tid; })) Print(*y, *x, sym.names[(u32)k]);
        }

        const auto insTable = [&](const char* title, int field) {
            printf("\n== Instructions by %s\n%s\n", title, kHead);
            for (const auto& [a, x] : Top(ins, field, top, all)) Print(*x, tot, clip(sym.Code(a), 60) + "  " + sym.names[sym.Fn(a)]);
        };
        insTable("IBS op samples", Ops);
        insTable("load-miss latency", Lat);

        // Data: 64-byte lines and 4 KiB pages by load-miss latency; lines stored to by one thread and accessed by another.
        std::vector<std::pair<u64, const Line*>> lv;
        for (const auto& [k, l] : lines) lv.push_back({k, &l});
        const auto lineTable = [&](const char* title, auto&& key, auto&& keep, size_t n) {
            std::vector<std::pair<u64, const Line*>> v;
            for (const auto& x : lv)
                if (keep(*x.second)) v.push_back(x);
            const size_t k = std::min(n, v.size());
            std::partial_sort(v.begin(), v.begin() + k, v.end(), [&](const auto& a, const auto& b) { return key(*a.second) > key(*b.second); });
            printf("\n== Data lines by %s\n      ld     st   miss   DRAM   Σlat/k  readers writers  line\n", title);
            for (size_t i = 0; i < k; i++) {
                const Line& l = *v[i].second;
                printf("%8llu %6llu %6llu %6llu %8.0f %8d %7d  %s\n", l.ld, l.st, l.miss, l.dram, (double)l.lat / 1e3, std::popcount(l.readers),
                       std::popcount(l.writers), clip(sym.Data(v[i].first << 6), 120).c_str());
            }
        };
        lineTable("load-miss latency", [](const Line& l) { return l.lat; }, [](const Line& l) { return l.lat > 0; }, top);
        lineTable("misses, written by one thread and used by another", [](const Line& l) { return l.miss; },
                  [](const Line& l) { return l.writers && std::popcount(l.readers | l.writers) > 1; }, top / 2);
        {
            std::unordered_map<u64, Line> pages;
            for (const auto& [k, l] : lines) {
                Line& g = pages[k >> 6];
                g.ld += l.ld, g.st += l.st, g.miss += l.miss, g.lat += l.lat, g.dram += l.dram, g.readers |= l.readers, g.writers |= l.writers;
            }
            std::vector<std::pair<u64, const Line*>> v;
            for (const auto& [k, l] : pages)
                if (l.lat) v.push_back({k, &l});
            const size_t k = std::min(top, v.size());
            std::partial_sort(v.begin(), v.begin() + k, v.end(), [](const auto& a, const auto& b) { return a.second->lat > b.second->lat; });
            printf("\n== Data pages (4 KiB) by load-miss latency\n      ld     st   miss   DRAM   Σlat/k  readers writers  page\n");
            for (size_t i = 0; i < k; i++) {
                const Line& l = *v[i].second;
                printf("%8llu %6llu %6llu %6llu %8.0f %8d %7d  %s\n", l.ld, l.st, l.miss, l.dram, (double)l.lat / 1e3, std::popcount(l.readers),
                       std::popcount(l.writers), clip(sym.Data(v[i].first << 12), 120).c_str());
            }
            if (!sym.regions.empty()) {
                std::map<u64, Line> alloc;
                for (const auto& [k, l] : pages) {
                    const Region* r = sym.RegionOf(k << 12);
                    Line& g = alloc[r ? r->alloc : k << 12 >= KernelBase];
                    g.ld += l.ld, g.st += l.st, g.miss += l.miss, g.lat += l.lat, g.dram += l.dram, g.readers |= l.readers, g.writers |= l.writers;
                }
                std::vector<std::pair<u64, const Line*>> w;
                for (const auto& [a, l] : alloc) w.push_back({a, &l});
                std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second->lat > b.second->lat; });
                printf("\n== Allocations (snapshot) by load-miss latency\n      ld     st   miss   DRAM   Σlat/k  readers writers  allocation\n");
                for (size_t i = 0; i < std::min(top, w.size()); i++) {
                    const Line& l = *w[i].second;
                    printf("%8llu %6llu %6llu %6llu %8.0f %8d %7d  %s\n", l.ld, l.st, l.miss, l.dram, (double)l.lat / 1e3, std::popcount(l.readers),
                           std::popcount(l.writers), w[i].first > 1 ? clip(sym.Data(w[i].first), 120).c_str() : w[i].first ? "<kernel>" : "<outside the snapshot>");
                }
            }
        }

        // -f name: every sampled instruction of the matching functions, with the data it touched most.
        if (const std::string f = Utf8(Arg(argc, argv, L"-f", L"")); !f.empty()) {
            printf("\n== Instructions of functions matching *%s*\n%s\n", f.c_str(), kHead);
            std::unordered_map<u64, std::unordered_map<u64, u64>> data;
            for (const auto& o : ops)
                if (sym.names[sym.Fn(o.rip)].find(f) != std::string::npos && Mem(o) && (o.data3 >> 17 & 1)) data[o.rip][o.lin >> 6]++;
            std::vector<std::pair<u64, const M*>> v;
            for (const auto& [a, x] : ins)
                if (sym.names[sym.Fn(a)].find(f) != std::string::npos) v.push_back({a, &x});
            std::sort(v.begin(), v.end());
            for (const auto& [a, x] : v) {
                std::string where = sym.Code(a);
                if (const auto it = data.find(a); it != data.end()) {
                    const auto best = std::max_element(it->second.begin(), it->second.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
                    where += std::format("  [{} lines, top {}x {}]", it->second.size(), best->second, sym.Data(best->first << 6));
                }
                Print(*x, tot, where);
            }
        }

        // -a file: every op sample of the process, for ad hoc analysis.
        if (const std::wstring dump = Arg(argc, argv, L"-a", L""); !dump.empty()) {
            FILE* f = _wfopen(dump.c_str(), L"wb");
            if (!f) throw std::runtime_error("cannot write the sample dump");
            fprintf(f, "time_us\ttid\trip\tirip\tdata\tdata2\tdata3\tlin\tfunction\n");
            for (const auto& o : ops)
                fprintf(f, "%.1f\t%u\t%llx\t%llx\t%llx\t%llx\t%llx\t%llx\t%s\n", (double)(o.tick - p.first) / 10, o.tid, o.rip, o.irip, o.data,
                        o.data2, o.data3, o.lin, sym.names[sym.Fn(o.rip)].c_str());
            fclose(f);
        }
    } catch (const std::exception& e) {
        fflush(stdout);
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    fflush(stdout);
    lap("report");
    return 0;
}
