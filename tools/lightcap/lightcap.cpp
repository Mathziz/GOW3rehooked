// lightcap <exe> <eboot base hex> <out file> <seconds> [<ranges file> <out dir> [scan [pattern files]]]: reads a running gow3.exe or
// shadps4.exe from outside, so one reader compares both. Guest addresses are host addresses in both processes. Prints the lights a
// query at the sphere accepts; ranges (label hexaddr hexsize) are hashed every 250 ms and each new content saved; scan searches all
// memory every 5 s for T# blocks shaped like the vine's and for the first 256 bytes of each pattern file.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
static HANDLE proc;
template <class T> static bool Rd(u64 a, T& v) { SIZE_T n = 0; return a && ReadProcessMemory(proc, (void*)a, &v, sizeof(T), &n) && n == sizeof(T); }
static bool RdN(u64 a, void* d, size_t s) { SIZE_T n = 0; return a && ReadProcessMemory(proc, (void*)a, d, s, &n) && n == s; }
static DWORD Find(const char* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32 e{sizeof(e)}; DWORD pid = 0;
    for (BOOL ok = Process32First(s, &e); ok; ok = Process32Next(s, &e)) if (!_stricmp(e.szExeFile, name)) pid = e.th32ProcessID;
    CloseHandle(s); return pid;
}
struct Range { std::string label; u64 addr; size_t size; u64 last = 0; u32 saved = 0; };
static std::vector<Range> ranges;
static std::string out_dir;
static void SampleRanges(FILE* f, ULONGLONG t) {
    std::vector<u8> b;
    for (Range& r : ranges) {
        if (r.label.starts_with("late") && (t < 8000 || r.saved)) continue;
        b.resize(r.size);
        if (!RdN(r.addr, b.data(), r.size)) {
            if (r.last != 1) fprintf(f, "R %llu ms %s %llx unreadable\n", t, r.label.c_str(), (unsigned long long)r.addr), r.last = 1;
            continue;
        }
        u64 h = 0xcbf29ce484222325ull;
        for (u8 x : b) h = (h ^ x) * 0x100000001b3ull;
        if (h == r.last) continue;
        r.last = h;
        fprintf(f, "R %llu ms %s %llx fnv %llx\n", t, r.label.c_str(), (unsigned long long)r.addr, (unsigned long long)h);
        if (r.saved < 12) {
            const std::string name = out_dir + "\\" + r.label + "." + std::to_string(r.saved++) + ".bin";
            if (FILE* o = fopen(name.c_str(), "wb")) { fwrite(b.data(), 1, b.size(), o); fclose(o); }
        }
    }
    fflush(f);
}
// Whole-memory search: blocks shaped like the vine's T# table (t2, t4, t0, t6 == t2, t8), and the port's shader bytes.
static std::vector<std::pair<std::string, std::vector<u8>>> patterns;
static std::map<std::string, u32> blocks;
static void Save(const std::string& name, u64 addr, size_t size) {
    std::vector<u8> b(size);
    if (!RdN(addr, b.data(), size)) return;
    if (FILE* o = fopen((out_dir + "\\" + name).c_str(), "wb")) { fwrite(b.data(), 1, size, o); fclose(o); }
}
static void ScanVine(FILE* f, ULONGLONG t) {
    constexpr size_t chunk = 16u << 20, tail = 0x400;
    std::vector<u8> b(chunk + tail);
    u64 total = 0;
    for (u64 a = 0x100000000ull; a < 0x1000000000ull;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQueryEx(proc, (void*)a, &m, sizeof(m))) break;
        const u64 next = (u64)m.BaseAddress + m.RegionSize;
        if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (u64 p = a; p < next; p += chunk) {
                const size_t want = (size_t)std::min<u64>(chunk + tail, next - p);
                SIZE_T got = 0;
                if (!ReadProcessMemory(proc, (void*)p, b.data(), want, &got) || got < 0x100) continue;
                total += got;
                const u32* d = (const u32*)b.data();
                const size_t n = got / 4;
                for (size_t i = 16; i + 40 < n; ++i) {
                    if (d[i] != 0x96d60fac || d[i - 1] != 0x001fc07f || d[i - 2] != 0x02500000) continue;
                    const u32* t4 = d + i - 3; const u32* t2 = t4 - 8; const u32* t0 = t4 + 8; const u32* t6 = t4 + 16; const u32* t8 = t4 + 24;
                    if (t2[1] != 0x02300000 || t2[3] != 0x96d80fac || t0[1] != 0x02500000 || t8[1] != 0x02300000 || memcmp(t2, t6, 32)) continue;
                    const std::string key = std::to_string(t2[0]) + "." + std::to_string(t4[0]) + "." + std::to_string(t0[0]) + "." + std::to_string(t8[0]);
                    const u64 at = p + (const u8*)t2 - b.data();
                    if (blocks.count(key) || blocks.size() >= 48) continue;
                    const u32 k = (u32)blocks.size();
                    blocks[key] = k;
                    fprintf(f, "B %llu ms blk%u at %llx t2 %08x t4 %08x t0 %08x t8 %08x\n", t, k, (unsigned long long)at, t2[0], t4[0], t0[0], t8[0]);
                    const std::string s = "blk" + std::to_string(k);
                    Save(s + "_ctx.bin", at - 0x1000, 0x2000);
                    const u32 base[4] = {t2[0], t4[0], t0[0], t8[0]};
                    const char* names[4] = {"t2", "t4", "t0", "t8"};
                    for (u32 j = 0; j < 4; ++j) Save(s + "_" + names[j] + ".bin", (u64)base[j] << 8, 0x40000);
                }
                for (auto& [name, pat] : patterns)
                    for (size_t i = 0; i + pat.size() <= got; i += 4)
                        if (!memcmp(b.data() + i, pat.data(), pat.size())) {
                            const u64 at = p + i;
                            fprintf(f, "P %llu ms %s at %llx\n", t, name.c_str(), (unsigned long long)at);
                            Save("found_" + name + "_" + std::to_string(at) + ".bin", at, 0x4000);
                        }
            }
        a = next;
    }
    fprintf(f, "S %llu ms scanned %llu MB, blocks %zu\n", t, (unsigned long long)(total >> 20), blocks.size());
    fflush(f);
}
struct Light { float p[4], d[4], c[4]; u32 flags, mask, z0, z1; u64 obj, pad; };
static_assert(sizeof(Light) == 0x50);
int main(int argc, char** argv) {
    if (argc < 5) return puts("lightcap <exe> <base hex> <out> <seconds> [<ranges> <dir> [scan [patterns]]]"), 1;
    const u64 base = strtoull(argv[2], nullptr, 16);
    const int seconds = atoi(argv[4]);
    FILE* f = fopen(argv[3], "w");
    if (argc > 5) {
        out_dir = argv[6];
        for (int i = 8; i < argc; ++i) if (FILE* q = fopen(argv[i], "rb")) { std::vector<u8> v(256); v.resize(fread(v.data(), 1, 256, q)); fclose(q); std::string n = argv[i]; n = n.substr(n.find_last_of("\/") + 1); if (v.size() == 256) patterns.push_back({n, v}); }
        CreateDirectoryA(out_dir.c_str(), nullptr);
        char label[64]; unsigned long long a, s;
        FILE* r = fopen(argv[5], "r");
        while (r && fscanf(r, "%63s %llx %llx", label, &a, &s) == 3) ranges.push_back({label, a, (size_t)s});
        if (r) fclose(r);
    }
    DWORD pid = 0;
    for (int i = 0; i < 1200 && !(pid = Find(argv[1])); ++i) Sleep(100);
    if (!pid) return puts("process not found"), 2;
    proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return printf("OpenProcess %lu\n", GetLastError()), 3;
    printf("pid %lu base %llx\n", pid, (unsigned long long)base);
    float sx = 244.2f, sy = -41.1f, sz = 115.2f, sr = 4.f;  // LIGHTCAP_SPHERE="x y z r": lights that start the sampling
    if (const char* s = getenv("LIGHTCAP_SPHERE")) sscanf(s, "%f %f %f %f", &sx, &sy, &sz, &sr);
    ULONGLONG t0 = 0, last_full = 0, last_range = 0, next_scan = 6000; u64 last_sig = 0; std::vector<Light> L; u32 samples = 0;
    for (;;) {
        DWORD code = 0;
        if (!GetExitCodeProcess(proc, &code) || code != STILL_ACTIVE) break;
        Sleep(10);
        u64 mgr = 0; u32 count = 0, special = 0; u64 arr = 0;
        if (!Rd(base + 0x15c4810, mgr) || !Rd(mgr + 0x174, count) || !Rd(mgr + 0x178, special) || !Rd(mgr + 0x180, arr)) continue;
        if (!count || count > 4096 || !arr) continue;
        L.resize(count);
        if (!RdN(arr, L.data(), count * sizeof(Light))) continue;
        std::vector<u32> hit; u64 sig = count;
        for (u32 i = 0; i < count; ++i) {
            const Light& l = L[i];
            const float dx = l.p[0] - sx, dy = l.p[1] - sy, dz = l.p[2] - sz, reach = l.p[3] + sr;
            if (dx * dx + dy * dy + dz * dz >= reach * reach || !(l.mask & 6) || !std::isfinite(l.c[3])) continue;
            hit.push_back(i);
            sig = sig * 1000003 + l.obj + u64(int(l.c[3] * 4.f));
        }
        if (hit.empty()) continue;
        const ULONGLONG now = GetTickCount64();
        if (!t0) t0 = now;
        if (seconds && now - t0 > ULONGLONG(seconds) * 1000) break;
        if (now - last_range >= 250) { last_range = now; SampleRanges(f, now - t0); }
        if (argc > 7 && now - t0 >= next_scan) { next_scan += 5000; ScanVine(f, now - t0); }
        float dt = 0; Rd(base + 0x3cdcb0, dt);
        ++samples;
        const bool full = now - last_full >= 1000;
        if (sig == last_sig && !full) continue;
        last_sig = sig;
        if (full) last_full = now;
        fprintf(f, "T %llu ms count %u special %u accepted %zu dt %.5f\n", now - t0, count, special, hit.size(), dt);
        for (u32 i : hit) {
            const Light& l = L[i];
            u64 owner = 0, table = 0, bytes = 0; short idx = -1; u8 en = 255;
            Rd(l.obj + 0x28, owner); Rd(l.obj + 0x80, idx);
            if (owner && idx >= 0 && Rd(owner + 0x180, table) && table && Rd(table + 0x50, bytes) && bytes) Rd(bytes + idx, en);
            fprintf(f, "  L %u obj %llx owner %llx idx %d en %d pos %.1f %.1f %.1f r %.1f flags %x mask %x col %.3f %.3f %.3f i %.3f\n", i, (unsigned long long)l.obj,
                    (unsigned long long)owner, idx, en, l.p[0], l.p[1], l.p[2], l.p[3], l.flags, l.mask, l.c[0], l.c[1], l.c[2], l.c[3]);
        }
        fflush(f);
    }
    printf("samples %u\n", samples);
    fclose(f);
    return 0;
}
