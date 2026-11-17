// etlprof: CPU sample and context switch analysis of a kernel ETL (WPR CPU profile) for one process.
// Events are read with ProcessTrace, frames symbolized once with DbgHelp, aggregation runs on all cores.
#define NOMINMAX
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <dbghelp.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dbghelp.lib")

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Counts = std::unordered_map<u64, u64>;
constexpr u32 None = ~0u;
constexpr u64 KernelBase = 0xFFFF800000000000ull;

constexpr GUID kPerfInfo{0xce1dbfb4, 0x137e, 0x4da6, {0x87, 0xb0, 0x3f, 0x59, 0xaa, 0x10, 0x2c, 0xbc}};
constexpr GUID kStackWalk{0xdef2fe46, 0x7bd6, 0x4b80, {0xbd, 0x94, 0xf5, 0x7f, 0xe2, 0x0d, 0x0c, 0xe3}};
constexpr GUID kThread{0x3d6fa8d1, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kProcess{0x3d6fa8d0, 0xfe05, 0x11d0, {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
constexpr GUID kImage{0x2cb15d1d, 0x5fc1, 0x11d2, {0xab, 0xe1, 0x00, 0xa0, 0xc9, 0x11, 0xf5, 0x18}};
constexpr GUID kImageId{0xb3e675d7, 0x2554, 0x4f18, {0x83, 0x0b, 0x27, 0x62, 0x73, 0x25, 0x60, 0xde}};
// The game's frame loop starts (src/patches/hooks.cpp RunFrame, recorded with tools\gow3.wprp): event 1, frame number u64.
constexpr GUID kGow3Frames{0xe5b7a118, 0xeaa4, 0x4971, {0xbc, 0xa2, 0x41, 0x33, 0x93, 0xac, 0xfa, 0x70}};
constexpr const char* kProviders[] = {"PerfInfo", "StackWalk", "Thread", "Image", "Process"};

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

static std::wstring WStr(const u8* p, const u8* end) {
    std::wstring s;
    for (; p + 1 < end; p += 2) {
        const wchar_t c = Rd<wchar_t>(p);
        if (!c) break;
        s += c;
    }
    return s;
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((u8)c);
    return s;
}

struct Sample { u64 ts, ip; u32 tid, cpu; };
struct Switch { u64 ts; u32 in, out, cpu; u8 reason, state; };
struct Ready { u64 ts; u32 cpu, tid; };
struct Part { u64 ts; u32 tid, off, n, next; };
struct Mark { u64 ts, n; u32 pid; };
struct Module {
    u64 base, size;
    std::wstring path;
    std::string name;
    u32 stamp;
    u16 id = 0;
    bool loaded = false;
    bool symbolsOk = false;
};

struct Trace {
    std::vector<Sample> samples;
    std::vector<Switch> switches;
    std::vector<Ready> readies;
    std::vector<Part> parts;
    std::vector<u64> frames;
    std::vector<Mark> marks;  // game frame starts
    std::unordered_map<u64, std::vector<u32>> pendingKeys;
    std::unordered_map<u32, u32> threadPid;
    std::unordered_map<u32, std::string> threadName;
    std::unordered_map<u32, u64> threadStart;
    std::unordered_map<u32, std::string> procName;
    std::unordered_map<u32, std::vector<Module>> modules;
    u64 first = ~0ull, last = 0, events = 0, counts[5][256]{};
    std::vector<u64> cpuFirst = std::vector<u64>(256, ~0ull);
    u32 interval = 0;
    std::unordered_map<unsigned long, u64> unknown;
    std::unordered_map<u64, u32> imageStamp; // base -> TimeDateStamp

    u32 AddFrames(const u8* p, u32 n) {
        const u32 off = (u32)frames.size();
        frames.resize(off + n);
        std::memcpy(frames.data() + off, p, n * 8ull);
        return off;
    }
};

static void WINAPI OnEvent(PEVENT_RECORD e) {
    auto& t = *static_cast<Trace*>(e->UserContext);
    const auto& h = e->EventHeader;
    const u8* d = static_cast<const u8*>(e->UserData);
    const u32 len = e->UserDataLength;
    const u8* end = d + len;
    const u8 op = h.EventDescriptor.Opcode;
    const u64 ts = (u64)h.TimeStamp.QuadPart;
    const GUID& g = h.ProviderId;
    t.events++;
    if (g == kPerfInfo) {
        t.counts[0][op]++;
        if (op == 46 && len >= 12) {
            t.samples.push_back({ts, Rd<u64>(d), Rd<u32>(d + 8), GetEventProcessorIndex(e)});
        } else if ((op == 73 || op == 74) && len >= 8 && Rd<u32>(d) == 0) {
            t.interval = Rd<u32>(d + 4);
        }
    } else if (g == kStackWalk) {
        t.counts[1][op]++;
        if (op == 32 && len >= 16) {
            const u32 n = (len - 16) / 8;
            t.parts.push_back({Rd<u64>(d), Rd<u32>(d + 12), t.AddFrames(d + 16, n), n, None});
        } else if ((op == 37 || op == 38) && len >= 24) {
            // Compressed stack reference; frames arrive with the key definition (delete or rundown).
            t.pendingKeys[Rd<u64>(d + 16)].push_back((u32)t.parts.size());
            t.parts.push_back({Rd<u64>(d), Rd<u32>(d + 12), 0, 0, None});
        } else if ((op == 34 || op == 35 || op == 36) && len >= 8) {
            const auto it = t.pendingKeys.find(Rd<u64>(d));
            if (it != t.pendingKeys.end()) {
                const u32 n = (len - 8) / 8, off = t.AddFrames(d + 8, n);
                for (const u32 i : it->second) t.parts[i].off = off, t.parts[i].n = n;
                t.pendingKeys.erase(it);
            }
        }
    } else if (g == kThread) {
        t.counts[2][op]++;
        if (op == 36 && len >= 16) {
            t.switches.push_back({ts, Rd<u32>(d), Rd<u32>(d + 4), GetEventProcessorIndex(e), d[12], d[14]});
        } else if (op == 50 && len >= 4) {
            t.readies.push_back({ts, GetEventProcessorIndex(e), Rd<u32>(d)});
        } else if ((op == 1 || op == 3 || op == 4) && len >= 72) {
            const u32 tid = Rd<u32>(d + 4);
            t.threadPid[tid] = Rd<u32>(d);
            t.threadStart[tid] = Rd<u64>(d + 48);
            if (auto n = WStr(d + 72, end); !n.empty()) t.threadName[tid] = Utf8(n);
        } else if (op == 72 && len >= 10) {
            const u32 tid = Rd<u32>(d + 4);
            t.threadPid.try_emplace(tid, Rd<u32>(d));
            if (auto n = WStr(d + 8, end); !n.empty()) t.threadName[tid] = Utf8(n);
        }
    } else if (g == kImage) {
        t.counts[3][op]++;
        if ((op == 10 || op == 3 || op == 4) && len >= 58) {
            Module m{Rd<u64>(d), Rd<u64>(d + 8), WStr(d + 56, end), {}, Rd<u32>(d + 24)};
            const size_t slash = m.path.find_last_of(L'\\');
            m.name = Lower(Utf8(m.path.substr(slash == std::wstring::npos ? 0 : slash + 1)));
            t.modules[Rd<u32>(d + 16)].push_back(std::move(m));
        }
    } else if (g == kProcess) {
        t.counts[4][op]++;
        if ((op == 1 || op == 3 || op == 4) && len >= 40) {
            const u32 pid = Rd<u32>(d + 8);
            u32 off = h.EventDescriptor.Version >= 4 ? 36 : 32;
            if (Rd<u32>(d + off) == 0) {
                off += 4;
            } else {
                off += 16; // TOKEN_USER, then SID with 8 byte header and 4 bytes per sub-authority
                off = off + 2 <= len ? off + 8 + 4 * d[off + 1] : len;
            }
            if (off < len) t.procName[pid] = Lower(std::string((const char*)d + off, strnlen((const char*)d + off, len - off)));
        }
    } else if (g == kImageId) {
        if (op == 0 && len >= 24) t.imageStamp[Rd<u64>(d)] = Rd<u32>(d + 20);
    } else if (g == kGow3Frames) {
        if (h.EventDescriptor.Id == 1 && len >= 8) t.marks.push_back({ts, Rd<u64>(d), h.ProcessId});
        return;  // a user-mode session: outside the kernel streams' per-CPU window
    } else {
        t.unknown[g.Data1]++;
        return;
    }
    if (ts) {
        t.last = std::max(t.last, ts);
        u64& f = t.cpuFirst[GetEventProcessorIndex(e) & 255];
        f = std::min(f, ts);
    }
}

struct StackKey {
    u64 ts;
    u32 tid;
    bool operator==(const StackKey&) const = default;
};
struct StackKeyHash {
    size_t operator()(const StackKey& k) const { return (k.ts * 0x9E3779B97F4A7C15ull) ^ k.tid; }
};

// Stack fragments of one event (kernel part, then the deferred user part) chained by (timestamp, thread).
// User frames unwind with their image's unwind data. Code outside every image of the process (guest code, trampolines) has
// none: the walker took it for a leaf, so the frames after the first such one are not its callers and are dropped.
struct Stacks {
    std::vector<Part>& parts;
    const std::vector<u64>& frames;
    std::unordered_map<StackKey, std::pair<u32, u32>, StackKeyHash> map;
    std::vector<std::pair<u64, u64>> images;  // [base, end) of the process's modules, sorted

    Stacks(std::vector<Part>& p, const std::vector<u64>& f, const std::vector<Module>& modules) : parts(p), frames(f) {
        map.reserve(parts.size());
        for (u32 i = 0; i < parts.size(); i++) {
            auto [it, fresh] = map.try_emplace({parts[i].ts, parts[i].tid}, i, i);
            if (!fresh) parts[it->second.second].next = i, it->second.second = i;
        }
        for (const auto& m : modules) images.push_back({m.base, m.base + m.size});
        std::sort(images.begin(), images.end());
    }
    bool Image(u64 a) const {
        const auto it = std::upper_bound(images.begin(), images.end(), a, [](u64 x, const auto& r) { return x < r.first; });
        return it != images.begin() && a < (it - 1)->second;
    }
    bool Get(u64 ts, u32 tid, std::vector<u64>& out) const {
        const auto it = map.find({ts, tid});
        if (it == map.end()) return false;
        for (u32 i = it->second.first; i != None; i = parts[i].next) {
            const size_t at = out.size();
            out.insert(out.end(), frames.begin() + parts[i].off, frames.begin() + parts[i].off + parts[i].n);
            for (size_t k = at; k < out.size(); k++)
                if (out[k] < KernelBase && !Image(out[k])) return out.resize(k + 1), true;
        }
        return true;
    }
};

// -b auto: the 64 KiB-aligned load base that puts the most samples outside the process's images on instruction starts of the
// guest map (<map>.starts, one bit per RVA).
static u64 GuestBase(const std::wstring& map, const std::vector<Sample>& samples, const Stacks& stacks) {
    std::ifstream input{std::filesystem::path(map + L".starts"), std::ios::binary};
    const std::vector<char> bits{std::istreambuf_iterator<char>(input), {}};
    if (bits.empty()) throw std::runtime_error("Cannot read the guest map's instruction starts (<map>.starts)");
    std::unordered_map<u64, u32> score;
    u32 outside = 0;
    for (const auto& s : samples) {
        if (s.ip >= KernelBase || stacks.Image(s.ip)) continue;
        outside++;
        for (u64 rva = s.ip & 0xffff; rva >> 3 < bits.size() && rva <= s.ip; rva += 0x10000)
            if (bits[rva >> 3] >> (rva & 7) & 1) score[s.ip - rva]++;
    }
    std::pair<u32, u64> best{}, next{};
    for (const auto& [base, n] : score)
        if (std::pair{n, base} > best) next = best, best = {n, base};
        else if (std::pair{n, base} > next) next = {n, base};
    if (best.first < 2 * next.first + 100) throw std::runtime_error("No clear guest load base in the samples");
    printf("guest base %llx: %u of %u samples outside images on instruction starts (next %llx: %u)\n",
           (unsigned long long)best.second, best.first, outside, (unsigned long long)next.second, next.first);
    return best.second;
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
    if (nt.starts_with(L"\\??\\")) return nt.substr(4);
    if (_wcsnicmp(nt.c_str(), L"\\SystemRoot\\", 12) == 0) {
        wchar_t windir[MAX_PATH];
        GetWindowsDirectoryW(windir, MAX_PATH);
        return std::wstring(windir) + nt.substr(11);
    }
    for (const auto& [device, drive] : devices)
        if (_wcsnicmp(nt.c_str(), device.c_str(), device.size()) == 0) return drive + nt.substr(device.size());
    return nt;
}

static bool Boring(const std::string& n) {
    static const char* words[] = {"std::", "_Cnd_", "_Mtx_", "_Thrd_", "invoke", "RunThread", "thread_start", "Concurrency::", "__crt"};
    for (const char* w : words)
        if (n.find(w) != std::string::npos) return true;
    return false;
}

// One DbgHelp session for the target process modules and kernel modules; serial, cached by address.
struct Symbols {
    enum : u8 { Gow = 1, Interesting = 2, Guest = 4 };
    HANDLE h = reinterpret_cast<HANDLE>(0x6e71);
    std::vector<Module> user, kernel;
    std::vector<std::string> names, modNames{"<guest>", "<kernel>"};
    std::vector<u16> mod;
    std::vector<u8> flags;
    std::unordered_map<std::string, u32> ids;
    std::unordered_map<u64, u32> addr;
    std::string gowName;
    u32 guestShift = 0; // -G: guest addresses in buckets of 1 << shift bytes instead of one "<guest>"
    struct GuestRange { u64 begin, end, entry; std::string name; };
    std::vector<GuestRange> guest;
    std::string guestModule, guestHash;
    u64 guestBase = 0, guestImageBase = 0;
    u16 guestId = 0;
    SYMBOL_INFOW* info = static_cast<SYMBOL_INFOW*>(calloc(1, sizeof(SYMBOL_INFOW) + 2048 * sizeof(wchar_t)));

    static std::vector<Module> Sorted(std::vector<Module> v) {
        std::sort(v.begin(), v.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
        v.erase(std::unique(v.begin(), v.end(), [](const Module& a, const Module& b) { return a.base == b.base; }), v.end());
        return v;
    }
    Symbols(std::vector<Module> u, std::vector<Module> k, const std::wstring& path, std::string gow) : gowName(std::move(gow)) {
        user = Sorted(std::move(u));
        kernel = Sorted(std::move(k));
        for (auto* list : {&user, &kernel})
            for (auto& m : *list) m.id = (u16)modNames.size(), modNames.push_back(m.name);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        SymInitializeW(h, path.c_str(), FALSE);
        info->SizeOfStruct = sizeof(SYMBOL_INFOW);
        info->MaxNameLen = 2048;
    }
    u32 Intern(std::string n, u16 m) {
        const auto [it, fresh] = ids.try_emplace(n, (u32)names.size());
        if (fresh) {
            const bool gow = modNames[m] == gowName;
            const bool g = guestId && m == guestId;
            flags.push_back((gow ? Gow : 0) | (g ? Guest : 0) | (g || (gow && !Boring(n)) ? Interesting : 0));
            mod.push_back(m);
            names.push_back(std::move(n));
        }
        return it->second;
    }
    void LoadGuest(const std::wstring& file, u64 base) {
        std::ifstream input{std::filesystem::path(file)};
        if (!input) throw std::runtime_error("Cannot open guest map");
        std::string line;
        if (!std::getline(input, line) || line != "# guest-symbols-v1") throw std::runtime_error("Invalid guest map version");
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            if (line.starts_with("# module\t")) guestModule = line.substr(9);
            else if (line.starts_with("# sha256\t")) guestHash = line.substr(9);
            else if (line.starts_with("# image_base\t")) guestImageBase = std::stoull(line.substr(13), nullptr, 16);
            else if (line[0] != '#') {
                unsigned long long lo, hi, entry;
                int offset = 0;
                if (sscanf_s(line.c_str(), "%llx\t%llx\t%llx\t%n", &lo, &hi, &entry, &offset) != 3 || !offset || lo >= hi)
                    throw std::runtime_error("Invalid guest range");
                if (!guest.empty() && guest.back().end > lo) throw std::runtime_error("Unsorted/overlapping guest ranges");
                guest.push_back({lo, hi, entry, line.substr(offset)});
            }
        }
        if (guest.empty() || guestModule.empty() || guestHash.size() != 64 || !base || base > KernelBase - guest.back().end)
            throw std::runtime_error("Incomplete guest map or invalid live base");
        guestBase = base;
        guestId = (u16)modNames.size();
        modNames.push_back(guestModule);
    }
    std::string Display(u32 id) const {
        return flags[id] & Gow ? names[id].substr(gowName.size() + 1) : names[id];
    }
    Module* Find(u64 a) {
        auto& list = a >= KernelBase ? kernel : user;
        const auto it = std::upper_bound(list.begin(), list.end(), a, [](u64 x, const Module& m) { return x < m.base; });
        return it == list.begin() || a >= (it - 1)->base + (it - 1)->size ? nullptr : &*(it - 1);
    }
    void Resolve(u64 a) {
        Module* m = Find(a);
        if (!m) {
            if (!guest.empty() && a >= guestBase) {
                const u64 rva = a - guestBase;
                auto it = std::upper_bound(guest.begin(), guest.end(), rva, [](u64 r, const GuestRange& g) { return r < g.begin; });
                if (it != guest.begin() && rva < (--it)->end) {
                    addr[a] = Intern(std::format("{}!{} [{:x}]", guestModule, it->name, guestImageBase + it->entry), guestId);
                    return;
                }
            }
            char bucket[32];
            if (a < KernelBase && guestShift) snprintf(bucket, sizeof bucket, "<guest>!%llx", (unsigned long long)(a >> guestShift << guestShift));
            addr[a] = Intern(a >= KernelBase ? "<kernel>" : guestShift ? bucket : "<guest>", a >= KernelBase);
            return;
        }
        if (!m->loaded) {
            m->loaded = true;
            SymLoadModuleExW(h, nullptr, DosPath(m->path).c_str(), nullptr, m->base, (DWORD)m->size, nullptr, 0);
            DWORD64 ignored = 0;
            SymFromAddrW(h, a, &ignored, info); // Complete a deferred load before checking its image timestamp.
            IMAGEHLP_MODULEW64 mi{sizeof mi};
            m->symbolsOk = SymGetModuleInfoW64(h, m->base, &mi) && (!m->stamp || mi.TimeDateStamp == m->stamp);
        }
        DWORD64 disp = 0;
        addr[a] = Intern(m->symbolsOk && SymFromAddrW(h, a, &disp, info) ? m->name + "!" + Utf8({info->Name, info->NameLen}) :
                         std::format("{}+{:#x}", m->name, a - m->base), m->id);
    }
    u32 Id(u64 a) const { return addr.find(a)->second; }
    std::string Describe(const Module& m) {
        IMAGEHLP_MODULEW64 mi{sizeof mi};
        if (!m.loaded || !SymGetModuleInfoW64(h, m.base, &mi)) return m.name + ": not loaded";
        static const char* types[] = {"none", "coff", "codeview", "pdb", "export", "deferred", "sym", "dia", "virtual"};
        char buf[256];
        snprintf(buf, sizeof buf, "%s: symbols=%s, trace stamp %08x, file stamp %08lx%s", m.name.c_str(),
                 mi.SymType < 9 ? types[mi.SymType] : "?", m.stamp, mi.TimeDateStamp,
                 m.stamp == mi.TimeDateStamp ? "" : "  MISMATCH (names rejected; module offsets only)");
        return buf;
    }
};

template <class F> static void ParallelFor(u32 jobs, size_t n, F&& f) {
    std::vector<std::thread> threads;
    for (u32 j = 0; j < jobs; j++) threads.emplace_back([&, j] { f(j, n * j / jobs, n * (j + 1) / jobs); });
    for (auto& t : threads) t.join();
}

static void Merge(Counts& into, const Counts& from) {
    for (const auto& [k, v] : from) into[k] += v;
}

struct Agg {
    Counts thr, leafMod, leafFn, incl, inclMod, caller, root;
    u64 withStack = 0;
    void Merge(const Agg& o) {
        for (auto [a, b] : {std::pair{&thr, &o.thr}, {&leafMod, &o.leafMod}, {&leafFn, &o.leafFn}, {&incl, &o.incl},
                            {&inclMod, &o.inclMod}, {&caller, &o.caller}, {&root, &o.root}})
            ::Merge(*a, *b);
        withStack += o.withStack;
    }
};

constexpr double kBuckets[] = {50, 100, 200, 300, 400, 500, 600, 800, 1000, 1500, 2000, 4000, 8000, 16000}; // us
struct WaitAgg {
    u64 wait = 0, lat = 0, n = 0, hist[std::size(kBuckets) + 1]{};
    void Add(u64 w, u64 l, double us = -1) {
        wait += w, lat += l, n++;
        if (us >= 0) hist[std::upper_bound(std::begin(kBuckets), std::end(kBuckets), us) - std::begin(kBuckets)]++;
    }
};
struct ThreadTime { u64 run = 0, wait = 0, lat = 0, preempt = 0, switches = 0, cpuRun = 0; };
struct Waits {
    std::unordered_map<u32, ThreadTime> thr;
    std::map<std::pair<u32, std::string>, WaitAgg> where, by;
    std::map<std::pair<u32, u32>, WaitAgg> pairs;
    u64 conc[17]{}, gconc[17]{}, outStacks = 0, inStacks = 0, waits = 0, readyStacks = 0, readyTarget = 0;
};

static const char* Reason(u8 r) {
    static const char* names[] = {"Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended",
        "UserRequest", "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution", "WrSuspended",
        "WrUserRequest", "WrSpare0", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut",
        "WrRendezvous", "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl", "WrCalloutStack",
        "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt", "WrPreempted",
        "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId", "WrDeferredPreempt",
        "WrPhysicalFault"};
    return r < std::size(names) ? names[r] : "?";
}

static std::wstring Arg(int argc, wchar_t** argv, const wchar_t* flag, std::wstring def) {
    for (int i = 2; i + 1 < argc; i++)
        if (!wcscmp(argv[i], flag)) return argv[i + 1];
    return def;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: etlprof <trace.etl> [-p gow3.exe] [-m gow3.exe] [-s symdir] [-n top] [-j jobs] [-g name|name] [-G shift] [-c leaf] [-d timeline] [-a samples.tsv] [-e guest.tsv -b live_image_base_hex|auto]\n");
        return 2;
    }
    static char out[1 << 20];
    setvbuf(stdout, out, _IOFBF, sizeof out);
    const std::string procName = Lower(Utf8(Arg(argc, argv, L"-p", L"gow3.exe")));
    const std::string gowName = Lower(Utf8(Arg(argc, argv, L"-m", L"gow3.exe")));
    wchar_t exe[MAX_PATH]{};  // the repo's build by default: this tool runs from tools\etlprof
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring symDir = Arg(argc, argv, L"-s", (std::filesystem::path(exe).parent_path().parent_path().parent_path() / L"build-fast\\RelWithDebInfo\\bin").wstring());
    const size_t top = std::stoul(Arg(argc, argv, L"-n", L"15"));
    std::vector<std::string> group;
    for (std::string g = Utf8(Arg(argc, argv, L"-g", L"GOW3 Vulkan worker|GOW3 render coordinator")) + "|"; !g.empty();) {
        const size_t bar = g.find('|');
        if (bar) group.push_back(g.substr(0, bar));
        g.erase(0, bar + 1);
    }
    const u32 jobs = std::max(2u, (u32)std::stoul(Arg(argc, argv, L"-j", std::to_wstring(std::thread::hardware_concurrency()))));
    const auto clock0 = std::chrono::steady_clock::now();
    const auto lap = [&](const char* what) {
        fprintf(stderr, "%-10s %.2f s\n", what, std::chrono::duration<double>(std::chrono::steady_clock::now() - clock0).count());
    };

    Trace t;
    EVENT_TRACE_LOGFILEW lf{};
    lf.LogFileName = argv[1];
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = OnEvent;
    lf.Context = &t;
    TRACEHANDLE th = OpenTraceW(&lf);
    if (th == INVALID_PROCESSTRACE_HANDLE) {
        fprintf(stderr, "OpenTrace failed: %lu\n", GetLastError());
        return 1;
    }
    double freq = (double)lf.LogfileHeader.PerfFreq.QuadPart;
    if (lf.LogfileHeader.ReservedFlags == 2) freq = 1e7;
    if (lf.LogfileHeader.ReservedFlags == 3) freq = lf.LogfileHeader.CpuSpeedInMHz * 1e6;
    if (lf.LogfileHeader.PointerSize != 8) fprintf(stderr, "warning: pointer size %lu, layouts assume 64-bit\n", lf.LogfileHeader.PointerSize);
    const ULONG status = ProcessTrace(&th, 1, nullptr, nullptr);
    CloseTrace(th);
    if (status != ERROR_SUCCESS) fprintf(stderr, "ProcessTrace returned %lu\n", status);
    lap("parse");
    if (t.samples.empty() || t.switches.empty() || t.readies.empty()) {
        fprintf(stderr, "trace has %zu samples, %zu context switches, %zu ready thread events\n", t.samples.size(), t.switches.size(), t.readies.size());
        if (t.switches.empty()) return 1;
    }
    // Analyze only where every stream is complete: circular buffers start per CPU, and WPR stops
    // context switch logging seconds before sampling.
    const u64 traceFirst = *std::min_element(t.cpuFirst.begin(), t.cpuFirst.end()), traceLast = t.last;
    // A trace without samples (profiling hardware held by another tool) still has the waits.
    t.first = std::max(t.switches.front().ts, t.readies.empty() ? 0 : t.readies.front().ts);
    if (!t.samples.empty()) t.first = std::max(t.first, t.samples.front().ts);
    for (const u64 f : t.cpuFirst)
        if (f != ~0ull) t.first = std::max(t.first, f);
    t.last = std::min(t.switches.back().ts, t.readies.empty() ? ~0ull : t.readies.back().ts);
    if (!t.samples.empty()) t.last = std::min(t.last, t.samples.back().ts);
    const auto inWindow = [&](auto& v) { return std::erase_if(v, [&](const auto& x) { return x.ts < t.first || x.ts > t.last; }); };
    const size_t dropped = inWindow(t.samples) + inWindow(t.switches) + inWindow(t.readies);
    inWindow(t.marks);
    fprintf(stderr, "window: dropped %.1f ms at start, %.1f ms at end (%zu events)\n", (t.first - traceFirst) / freq * 1e3,
            (traceLast - t.last) / freq * 1e3, dropped);

    // Target process: name from Process events (or its exe image), most sampled instance.
    std::unordered_map<u32, u64> pidSamples;
    for (const auto& s : t.samples)
        if (const auto it = t.threadPid.find(s.tid); it != t.threadPid.end()) pidSamples[it->second]++;
    for (const auto& [pid, mods] : t.modules)
        for (const auto& m : mods)
            if (m.name == procName) t.procName.try_emplace(pid, procName);
    u32 pid = None;
    for (const auto& [p, name] : t.procName)
        if (name == procName && (pid == None || pidSamples[p] > pidSamples[pid])) pid = p;
    if (pid == None) {
        fprintf(stderr, "process %s not found; %llu events, processes:", procName.c_str(), t.events);
        for (const auto& [p, name] : t.procName) fprintf(stderr, " %u=%s", p, name.c_str());
        fprintf(stderr, "\nevents:");
        for (int p = 0; p < 5; p++)
            for (int o = 0; o < 256; o++)
                if (t.counts[p][o]) fprintf(stderr, " %s/%d=%llu", kProviders[p], o, t.counts[p][o]);
        for (const auto& [g, n] : t.unknown) fprintf(stderr, " %08lx=%llu", g, n);
        fprintf(stderr, "\n");
        return 1;
    }
    const auto isTarget = [&](u32 tid) {
        const auto it = t.threadPid.find(tid);
        return tid && it != t.threadPid.end() && it->second == pid;
    };
    std::vector<Sample> samples;
    for (const auto& s : t.samples)
        if (isTarget(s.tid)) samples.push_back(s);
    std::erase_if(t.marks, [&](const Mark& m) { return m.pid != pid; });
    std::sort(t.marks.begin(), t.marks.end(), [](const Mark& x, const Mark& y) { return x.ts < y.ts; });

    Stacks stacks(t.parts, t.frames, t.modules[pid]);
    // Readier of each ReadyThread: the thread running on that CPU when it was logged.
    std::vector<u32> readyBy(t.readies.size());
    {
        std::vector<u32> cur(256, 0);
        for (size_t i = 0, j = 0; j < t.readies.size();) {
            if (i < t.switches.size() && t.switches[i].ts < t.readies[j].ts) cur[t.switches[i].cpu & 255] = t.switches[i].in, i++;
            else readyBy[j] = cur[t.readies[j].cpu & 255], j++;
        }
    }
    lap("index");

    // Unique addresses of everything we will name: samples, wait stacks, ready stacks, thread starts.
    std::vector<std::vector<u64>> found(jobs);
    ParallelFor(jobs, samples.size() + t.switches.size() + t.readies.size(), [&](u32 j, size_t b, size_t e) {
        auto& v = found[j];
        for (size_t i = b; i < e; i++) {
            if (i < samples.size()) {
                v.push_back(samples[i].ip);
                stacks.Get(samples[i].ts, samples[i].tid, v);
            } else if (i < samples.size() + t.switches.size()) {
                const auto& c = t.switches[i - samples.size()];
                if (isTarget(c.out)) stacks.Get(c.ts, c.out, v);
                if (isTarget(c.in)) stacks.Get(c.ts, c.in, v);
            } else if (const size_t k = i - samples.size() - t.switches.size(); isTarget(t.readies[k].tid)) {
                stacks.Get(t.readies[k].ts, readyBy[k], v);
            }
        }
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    });
    for (const auto& [tid, a] : t.threadStart)
        if (isTarget(tid)) found[0].push_back(a);
    for (auto* list : {&t.modules[pid], &t.modules[0]})
        for (auto& m : *list)
            if (const auto it = t.imageStamp.find(m.base); !m.stamp && it != t.imageStamp.end()) m.stamp = it->second;
    Symbols sym(t.modules[pid], t.modules[0], symDir, gowName);
    sym.guestShift = (u32)std::stoul(Arg(argc, argv, L"-G", L"0"));
    const auto guestMap = Arg(argc, argv, L"-e", L""), guestBase = Arg(argc, argv, L"-b", L"");
    try {
        if (guestMap.empty() != guestBase.empty()) throw std::runtime_error("Guest map (-e) and live image base (-b) must be supplied together");
        if (!guestMap.empty())
            sym.LoadGuest(guestMap, guestBase == L"auto" ? GuestBase(guestMap, samples, stacks) : std::stoull(guestBase, nullptr, 16));
        if (sym.guestShift >= 64) throw std::runtime_error("Guest bucket shift must be below 64");
    } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    for (const auto& v : found)
        for (const u64 a : v)
            if (!sym.addr.contains(a)) sym.Resolve(a);
    lap("symbolize");

    const auto path = [&](const std::vector<u64>& fr, size_t depth) {
        std::string s;
        size_t n = 0;
        for (const u64 f : fr) {
            const u32 id = sym.Id(f);
            if (!(sym.flags[id] & Symbols::Interesting) || s.find(sym.names[id]) != std::string::npos) continue;
            s += (n ? " < " : "") + sym.Display(id);
            if (++n == depth) break;
        }
        if (n) return s;
        for (const u64 f : fr)
            if (f < KernelBase) return sym.names[sym.Id(f)];
        return std::string(fr.empty() ? "<no stack>" : "<kernel only>");
    };

    // Module sequence of a (kernel) stack, deepest first, with consecutive repeats folded.
    const auto modules = [&](const std::vector<u64>& fr) {
        std::string s, last;
        for (const u64 f : fr) {
            const std::string& m = sym.modNames[sym.mod[sym.Id(f)]];
            if (m == last) continue;
            s += (s.empty() ? "" : " < ") + m;
            last = m;
        }
        return s.empty() ? std::string("<no stack>") : s;
    };

    // Sample aggregation on jobs-1 threads, wait analysis on the last one.
    std::vector<Agg> aggs(jobs - 1);
    Waits w;
    std::thread waiter([&] {
        struct St { u64 in = 0, out = 0, ready = 0; u32 by = None; bool running = false, off = false; u8 reason = 0, state = 0; };
        std::unordered_map<u32, St> st;
        std::vector<u64> fr;
        u64 lastTs = t.first;
        u32 running = 0, gRunning = 0;
        std::unordered_map<u32, bool> inGroup;
        const auto grouped = [&](u32 tid) {
            const auto [it, added] = inGroup.try_emplace(tid, false);
            if (const auto n = t.threadName.find(tid); added && n != t.threadName.end())
                it->second = std::ranges::any_of(group, [&](const std::string& g) { return n->second.find(g) != std::string::npos; });
            return it->second;
        };
        std::vector<std::pair<u32, u64>> cpuIn(256);
        for (size_t i = 0, j = 0; i < t.switches.size();) {
            if (j < t.readies.size() && t.readies[j].ts < t.switches[i].ts) {
                const auto& r = t.readies[j++];
                if (!isTarget(r.tid)) continue;
                auto& s = st[r.tid];
                if (s.off && !s.ready) s.ready = r.ts, s.by = readyBy[j - 1];
                continue;
            }
            const auto& c = t.switches[i++];
            if (cpuIn[c.cpu & 255].second && isTarget(cpuIn[c.cpu & 255].first)) w.thr[cpuIn[c.cpu & 255].first].cpuRun += c.ts - cpuIn[c.cpu & 255].second;
            cpuIn[c.cpu & 255] = {c.in, c.ts};
            w.conc[std::min(running, 16u)] += c.ts - lastTs;
            w.gconc[std::min(gRunning, 16u)] += c.ts - lastTs;
            lastTs = c.ts;
            if (isTarget(c.out)) {
                auto& s = st[c.out];
                if (s.running) w.thr[c.out].run += c.ts - s.in, running--, gRunning -= grouped(c.out);
                s = {0, c.ts, 0, None, false, true, c.reason, c.state};
            }
            if (!isTarget(c.in)) continue;
            auto& s = st[c.in];
            auto& tt = w.thr[c.in];
            tt.switches++;
            if (s.off && s.state == 5) {
                const u64 rdy = s.ready ? s.ready : c.ts;
                tt.wait += rdy - s.out, tt.lat += c.ts - rdy, w.waits++;
                fr.clear();
                const bool atOut = stacks.Get(s.out, c.in, fr);
                if (atOut) w.outStacks++;
                else if (stacks.Get(c.ts, c.in, fr)) w.inStacks++;
                w.where[{c.in, std::string(Reason(s.reason)) + (atOut ? " | " : " | [switch-in fallback] ") + path(fr, 3)}].Add(rdy - s.out, c.ts - rdy);
                std::string by = s.by == None ? "<not readied>" : "T" + std::to_string(s.by);
                if (s.by != None && isTarget(s.by)) {
                    fr.clear();
                    w.readyTarget++;
                    if (stacks.Get(s.ready, s.by, fr)) w.readyStacks++;
                    by += " @ " + path(fr, 2);
                    w.pairs[{s.by, c.in}].Add(rdy - s.out, c.ts - rdy);
                } else if (s.by != None) {
                    const auto p = t.threadPid.find(s.by);
                    by = s.by == 0 ? "idle/DPC" : (p == t.threadPid.end() ? "?" : t.procName[p->second]) + ":" + std::to_string(s.by);
                    fr.clear();
                    stacks.Get(s.ready, s.by, fr);
                    by += " @ " + modules(fr);
                }
                w.by[{c.in, by}].Add(rdy - s.out, c.ts - rdy, (rdy - s.out) / freq * 1e6);
            } else if (s.off) {
                tt.preempt += c.ts - s.out;
            }
            running += !s.running, gRunning += !s.running && grouped(c.in);
            s.running = true, s.off = false, s.in = c.ts;
        }
    });
    ParallelFor(jobs - 1, samples.size(), [&](u32 j, size_t b, size_t e) {
        auto& a = aggs[j];
        std::vector<u64> fr;
        std::vector<u32> seen;
        for (size_t i = b; i < e; i++) {
            const auto& s = samples[i];
            fr.clear();
            seen.clear();
            a.withStack += stacks.Get(s.ts, s.tid, fr);
            const u32 leaf = sym.Id(s.ip);
            const u64 tk = (u64)s.tid << 32;
            a.thr[s.tid]++;
            a.leafMod[tk | sym.mod[leaf]]++;
            a.leafFn[tk | leaf]++;
            u32 caller = None, root = None;
            for (const u64 f : fr) {
                const u32 id = sym.Id(f);
                if (!(sym.flags[id] & Symbols::Interesting)) continue;
                if (caller == None) caller = id;
                root = id;
                if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
                seen.push_back(id);
                a.incl[tk | id]++;
                a.inclMod[(u64)id << 32 | sym.mod[leaf]]++;
            }
            if (!(sym.flags[leaf] & (Symbols::Gow | Symbols::Guest)) && caller != None) a.caller[(u64)leaf << 32 | caller]++;
            if (root != None) a.root[tk | root]++;
        }
    });
    for (size_t j = 1; j < aggs.size(); j++) aggs[0].Merge(aggs[j]);
    waiter.join();
    const Agg& a = aggs[0];
    lap("aggregate");

    if (const auto dump = Arg(argc, argv, L"-a", L""); !dump.empty()) {
        FILE* f = nullptr;
        _wfopen_s(&f, dump.c_str(), L"wb");
        if (!f) { fprintf(stderr, "Cannot open sample output\n"); return 1; }
        fprintf(f, "time_us\ttid\tcpu\tip\tsymbol\n");
        for (const auto& s : samples)
            fprintf(f, "%.1f\t%u\t%u\t%llx\t%s\n", (s.ts - t.first) / freq * 1e6, s.tid, s.cpu, s.ip, sym.names[sym.Id(s.ip)].c_str());
        fclose(f);
    }

    // -d file: raw timeline of the process for ad hoc analysis, one event per line, times in us from the window start:
    // S us cpu out state reason in in-process | R us cpu tid readier | P us cpu tid leaf | F us frame (game frame start).
    if (const std::wstring dump = Arg(argc, argv, L"-d", L""); !dump.empty()) {
        FILE* f = _wfopen(dump.c_str(), L"wb");
        const auto us = [&](u64 ts) { return (double)(long long)(ts - t.first) / freq * 1e6; };
        const auto proc = [&](u32 tid) {
            const auto p = t.threadPid.find(tid);
            return !tid ? "idle" : isTarget(tid) ? "-" : p == t.threadPid.end() ? "?" : t.procName[p->second].c_str();
        };
        for (const auto& c : t.switches)
            if (isTarget(c.in) || isTarget(c.out)) fprintf(f, "S %.1f %u %u %u %u %u %s\n", us(c.ts), c.cpu, c.out, c.state, c.reason, c.in, proc(c.in));
        for (size_t j = 0; j < t.readies.size(); j++)
            if (isTarget(t.readies[j].tid)) fprintf(f, "R %.1f %u %u %u\n", us(t.readies[j].ts), t.readies[j].cpu, t.readies[j].tid, readyBy[j]);
        for (const auto& s : samples) fprintf(f, "P %.1f %u %u %s\n", us(s.ts), s.cpu, s.tid, sym.names[sym.Id(s.ip)].c_str());
        for (const auto& m : t.marks) fprintf(f, "F %.1f %llu\n", us(m.ts), (unsigned long long)m.n);
        fclose(f);
    }

    // Report.
    const double dur = (t.last - t.first) / freq * 1e3;
    const double interval = t.interval ? t.interval / 1e4 : 1.0; // ms per sample
    const auto ms = [&](u64 ticks) { return ticks / freq * 1e3; };
    const auto clip = [](const std::string& s, size_t n = 120) { return s.size() > n ? s.substr(0, n - 3) + "..." : s; };
    const auto topOf = [](const Counts& c, u64 hi, bool byHi = true) {
        std::vector<std::pair<u64, u32>> v;
        for (const auto& [k, n] : c)
            if (!byHi || (k >> 32) == hi) v.push_back({n, (u32)k});
        std::sort(v.begin(), v.end(), std::greater<>());
        return v;
    };
    std::unordered_map<u32, std::string> label;
    for (const auto& [tid, n] : a.thr) {
        if (const auto it = t.threadName.find(tid); it != t.threadName.end()) label[tid] = it->second;
        else if (const auto r = topOf(a.root, tid); !r.empty()) label[tid] = "~" + sym.Display(r[0].second);
        else if (const auto s = t.threadStart.find(tid); s != t.threadStart.end()) label[tid] = "start " + sym.names[sym.Id(s->second)];
    }

    printf("etlprof %s\nprocess %s pid %u, duration %.1f ms, %llu events, sample interval %.3f ms\n", Utf8(argv[1]).c_str(),
           procName.c_str(), pid, dur, t.events, interval);
    // Frames: the game's frame loop starts in the window; the thread table adds run ms per frame.
    const double frames = t.marks.size() > 1 ? double(t.marks.size() - 1) * dur / ms(t.marks.back().ts - t.marks.front().ts) : 0;
    if (frames) {
        std::vector<double> ft;
        for (size_t i = 1; i < t.marks.size(); i++) ft.push_back(ms(t.marks[i].ts - t.marks[i - 1].ts));
        std::sort(ft.begin(), ft.end());
        const auto q = [&](double p) { return ft[size_t(p * double(ft.size() - 1))]; };
        printf("frames: %zu starts (game frames %llu-%llu), %.1f fps; frame ms median %.3f p90 %.3f p99 %.3f max %.3f\n",
               t.marks.size(), (unsigned long long)t.marks.front().n, (unsigned long long)t.marks.back().n, frames / dur * 1e3,
               q(0.5), q(0.9), q(0.99), ft.back());
    } else {
        printf("frames: no GOW3 frame markers (capture with tools\\gow3.wprp beside the CPU profile)\n");
    }
    printf("samples: %zu in process of %zu (%.1f%% with stack); switches %zu, readies %zu; %zu unique frames, %zu symbols\n",
           samples.size(), t.samples.size(), 100.0 * a.withStack / std::max<size_t>(1, samples.size()), t.switches.size(),
           t.readies.size(), sym.addr.size(), sym.names.size());
    printf("wait stacks: %llu waits, %llu at switch-out, %llu at switch-in; %llu readied by own threads, %llu with ready stack\n",
           w.waits, w.outStacks, w.inStacks, w.readyTarget, w.readyStacks);
    for (const auto& m : sym.user)
        if (m.name == gowName || m.name == procName) printf("%s\n", sym.Describe(m).c_str());
    if (!sym.guest.empty())
        printf("guest map: %s, %zu exact ranges, live image base %llx, Ghidra image base %llx, SHA256 %s\n",
               sym.guestModule.c_str(), sym.guest.size(), sym.guestBase, sym.guestImageBase, sym.guestHash.c_str());
    printf("Wait stacks marked switch-in fallback do not identify the blocking call. Preempted time includes runnable/yield intervals.\n");
    printf("events:");
    for (int p = 0; p < 5; p++)
        for (int o = 0; o < 256; o++)
            if (t.counts[p][o]) printf(" %s/%d=%llu", kProviders[p], o, t.counts[p][o]);
    printf("\n");

    std::vector<std::pair<u64, u32>> threads;
    for (const auto& [tid, tt] : w.thr) threads.push_back({a.thr.contains(tid) ? a.thr.at(tid) : 0, tid});
    for (const auto& [tid, n] : a.thr)
        if (!w.thr.contains(tid)) threads.push_back({n, tid});
    std::sort(threads.begin(), threads.end(), std::greater<>());
    u64 totalRun = 0;
    printf("\n== Threads: samples, cpu%% of one core (samples), run/wait/ready-latency/preempted ms (switches)%s, label\n",
           frames ? ", run ms/frame" : "");
    for (const auto& [n, tid] : threads) {
        const ThreadTime tt = w.thr.contains(tid) ? w.thr.at(tid) : ThreadTime{};
        totalRun += tt.run;
        if (n < 2 && ms(tt.run) < 1) continue;
        printf("%6u %6llu %5.1f%%  cpurun %7.1f run %7.1f wait %7.1f lat %6.1f pre %6.1f (%6llu)%s  %s\n", tid, n, 100 * n * interval / dur, ms(tt.cpuRun),
               ms(tt.run), ms(tt.wait), ms(tt.lat), ms(tt.preempt), tt.switches,
               frames ? std::format(" {:6.3f}/f", ms(tt.run) / frames).c_str() : "", clip(label[tid], 90).c_str());
    }
    printf("process run time %.1f ms = %.2f cores\n", ms(totalRun), ms(totalRun) / dur);
    printf("\n== Concurrency: time with k process threads running\n");
    for (int k = 0; k <= 16; k++)
        if (w.conc[k]) printf("  %2d%s: %7.1f ms %5.1f%%\n", k, k == 16 ? "+" : "", ms(w.conc[k]), 100 * ms(w.conc[k]) / dur);
    printf("\n== Group concurrency: time with k threads named");
    for (const auto& g : group) printf(" [%s]", g.c_str());
    printf(" running\n");
    for (int k = 0; k <= 16; k++)
        if (w.gconc[k]) printf("  %2d%s: %7.1f ms %5.1f%%\n", k, k == 16 ? "+" : "", ms(w.gconc[k]), 100 * ms(w.gconc[k]) / dur);

    printf("\n== Per thread: leaf module, leaf function, inclusive host/guest function (samples)\n");
    for (const auto& [n, tid] : threads) {
        if (n * 200 < samples.size()) continue;
        printf("\n-- %u %s: %llu samples\n", tid, clip(label[tid], 100).c_str(), n);
        printf("  modules:");
        for (const auto& [c, m] : topOf(a.leafMod, tid)) printf(" %s=%llu", sym.modNames[m].c_str(), c);
        printf("\n");
        const auto lf = topOf(a.leafFn, tid), in = topOf(a.incl, tid);
        for (size_t i = 0; i < std::min(top, lf.size()); i++) printf("  leaf %6llu %s\n", lf[i].first, clip(sym.names[lf[i].second]).c_str());
        for (size_t i = 0; i < std::min(top, in.size()); i++) printf("  incl %6llu %s\n", in[i].first, clip(sym.names[in[i].second]).c_str());
    }

    printf("\n== Leaf outside host/guest code <- first host/guest caller (all threads)\n");
    {
        std::vector<std::pair<u64, u64>> v;
        for (const auto& [k, n] : a.caller) v.push_back({n, k});
        std::sort(v.begin(), v.end(), std::greater<>());
        for (size_t i = 0; i < std::min(top * 3, v.size()); i++)
            printf("  %6llu %s <- %s\n", v[i].first, clip(sym.names[(u32)(v[i].second >> 32)], 70).c_str(),
                   clip(sym.names[(u32)v[i].second], 90).c_str());
    }

    printf("\n== Inclusive host/guest function: leaf module split (all threads)\n");
    {
        Counts incl;
        for (const auto& [k, n] : a.incl) incl[(u32)k] += n;
        const auto v = topOf(incl, 0, false);
        for (size_t i = 0; i < std::min(top * 2, v.size()); i++) {
            printf("  %6llu %s\n         ", v[i].first, clip(sym.names[v[i].second]).c_str());
            for (const auto& [c, m] : topOf(a.inclMod, v[i].second))
                if (c * 100 >= v[i].first) printf(" %s=%llu", sym.modNames[m].c_str(), c);
            printf("\n");
        }
    }

    printf("\n== Waits per thread: where it blocked, who readied it (wait ms / ready latency ms / count)\n");
    for (const auto& [n, tid] : threads) {
        const ThreadTime tt = w.thr.contains(tid) ? w.thr.at(tid) : ThreadTime{};
        if (ms(tt.wait) < dur / 100) continue;
        printf("\n-- %u %s: run %.1f ms, wait %.1f ms, latency %.1f ms\n", tid, clip(label[tid], 90).c_str(), ms(tt.run),
               ms(tt.wait), ms(tt.lat));
        for (auto* table : {&w.where, &w.by}) {
            std::vector<std::pair<u64, const std::pair<const std::pair<u32, std::string>, WaitAgg>*>> v;
            for (auto it = table->lower_bound({tid, {}}); it != table->end() && it->first.first == tid; ++it) v.push_back({it->second.wait, &*it});
            std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            for (size_t i = 0; i < std::min<size_t>(table == &w.where ? 10 : 8, v.size()); i++) {
                const auto& [key, wa] = *v[i].second;
                printf("  %s %8.1f %6.1f %6llu  %s\n", table == &w.where ? "at" : "by", ms(wa.wait), ms(wa.lat), wa.n, clip(key.second, 200).c_str());
                if (table == &w.by && wa.n >= 100) {
                    printf("     wait us:");
                    for (size_t b = 0; b <= std::size(kBuckets); b++)
                        if (wa.hist[b]) printf(" %s%.0f=%llu", b == std::size(kBuckets) ? ">=" : "<", kBuckets[std::min(b, std::size(kBuckets) - 1)], wa.hist[b]);
                    printf("\n");
                }
            }
        }
    }

    printf("\n== Wake pairs inside the process: readier -> woken (wait ms ended / latency ms / count)\n");
    {
        std::vector<std::pair<u64, std::pair<u32, u32>>> v;
        for (const auto& [k, wa] : w.pairs) v.push_back({wa.wait, k});
        std::sort(v.begin(), v.end(), std::greater<>());
        for (size_t i = 0; i < std::min(top * 3, v.size()); i++) {
            const auto& [by, tid] = v[i].second;
            const auto& wa = w.pairs.at(v[i].second);
            printf("  %6u -> %6u %8.1f %6.1f %6llu  %s -> %s\n", by, tid, ms(wa.wait), ms(wa.lat), wa.n, clip(label[by], 50).c_str(),
                   clip(label[tid], 50).c_str());
        }
    }
    // -c name: call paths (leaf first, interesting frames only) of the samples whose leaf function contains name.
    if (const std::string want = Utf8(Arg(argc, argv, L"-c", L"")); !want.empty()) {
        printf("\n== Call paths of leaf *%s* (samples)\n", want.c_str());
        std::map<std::string, u64> paths, sites, ips;  // sites: the first caller frame outside the leaf, as module+offset
        std::vector<u64> fr;
        for (const auto& s : samples) {
            if (sym.names[sym.Id(s.ip)].find(want) == std::string::npos) continue;
            if (const Module* m = sym.Find(s.ip)) ips[std::format("{}+{:#x}", m->name, s.ip - m->base)]++;
            else if (sym.flags[sym.Id(s.ip)] & Symbols::Guest) ips[std::format("{}+{:#x}", sym.guestModule, s.ip - sym.guestBase)]++;
            fr.clear(), stacks.Get(s.ts, s.tid, fr);
            std::string p;
            u32 depth = 0;
            bool site = false;
            for (const u64 f : fr) {
                const u32 id = sym.Id(f);
                if ((sym.flags[id] & Symbols::Interesting) && depth++ < 8) p += (p.empty() ? "" : " <- ") + sym.names[id];
                if (const Module* m = sym.Find(f); !site && m && sym.names[id].find(want) == std::string::npos && f != s.ip)
                    site = true, sites[std::format("{}+{:#x}", m->name, f - m->base)]++;
            }
            paths[p.empty() ? "<no stack>" : p]++;
        }
        std::vector<std::pair<u64, const std::string*>> at;
        for (const auto& [p, n] : sites) at.push_back({n, &p});
        std::sort(at.begin(), at.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < std::min(top, at.size()); i++) printf("  %6llu called from %s\n", at[i].first, at[i].second->c_str());
        for (const auto& [p, n] : ips) printf("  %6llu at %s\n", n, p.c_str());  // every sampled address in the leaf
        std::vector<std::pair<u64, const std::string*>> v;
        for (const auto& [p, n] : paths) v.push_back({n, &p});
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < std::min(top, v.size()); i++) printf("  %6llu %s\n", v[i].first, v[i].second->c_str());
    }
    fflush(stdout);
    lap("report");
    return 0;
}
