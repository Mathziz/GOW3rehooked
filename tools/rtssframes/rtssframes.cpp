// Frame times of a running game as RivaTuner Statistics Server measures them (its present hook, the overlay's own numbers),
// read from RTSS's shared memory: no debug mode or instrumentation in the game. Polls the app entry's frame time ring for
// <seconds> (after waiting [delay] seconds), writes every frame time (us, one per line) to <out>, then prints the distribution
// and the local spikes.
//   rtssframes <process name> <seconds> <out> [delay]
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "RTSSSharedMemory.h"

namespace {
double Median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
}

int main(int argc, char** argv) {
    if (argc < 4) return std::fprintf(stderr, "usage: rtssframes <process name> <seconds> <out> [delay]\n"), 1;
    const std::string want = argv[1];
    const double seconds = std::atof(argv[2]);
    if (argc > 4) Sleep(DWORD(std::atof(argv[4]) * 1000));
    const HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, "RTSSSharedMemoryV2");
    if (!map) return std::fprintf(stderr, "RTSS shared memory not found (is RTSS running?)\n"), 1;
    const auto* mem = static_cast<const RTSS_SHARED_MEMORY*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
    if (!mem || mem->dwSignature != 'RTSS' || mem->dwVersion < 0x00020005) return std::fprintf(stderr, "unexpected RTSS shared memory\n"), 1;
    const auto entry = [&]() -> const RTSS_SHARED_MEMORY::RTSS_SHARED_MEMORY_APP_ENTRY* {
        for (DWORD i = 0; i < mem->dwAppArrSize; ++i) {
            const auto* e = reinterpret_cast<const RTSS_SHARED_MEMORY::RTSS_SHARED_MEMORY_APP_ENTRY*>(
                reinterpret_cast<const char*>(mem) + mem->dwAppArrOffset + i * mem->dwAppEntrySize);
            if (!e->dwProcessID) continue;
            const char* slash = std::strrchr(e->szName, '\\');
            if (!_stricmp(slash ? slash + 1 : e->szName, want.c_str())) return e;
        }
        return nullptr;
    };
    const RTSS_SHARED_MEMORY::RTSS_SHARED_MEMORY_APP_ENTRY* app = nullptr;
    for (int i = 0; i < 100 && !(app = entry()); ++i) Sleep(100);
    if (!app) return std::fprintf(stderr, "%s not in RTSS's applications\n", want.c_str()), 1;
    std::vector<double> ft;  // ms
    DWORD pos = app->dwStatFrameTimeBufPos;
    unsigned lost = 0;
    const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds * 1000);
    while (GetTickCount64() < end) {
        Sleep(50);
        const DWORD now = app->dwStatFrameTimeBufPos;
        const DWORD n = (now - pos) % 1024;
        if (n > 900) ++lost;  // the ring may have wrapped: frames lost
        for (DWORD k = 0; k < n; ++k) ft.push_back(app->dwStatFrameTimeBuf[(pos + k) % 1024] * 1e-3);
        pos = now;
    }
    if (FILE* f = std::fopen(argv[3], "w")) {
        for (double x : ft) std::fprintf(f, "%.0f\n", x * 1e3);
        std::fclose(f);
    }
    if (ft.size() < 100) return std::fprintf(stderr, "only %zu frames (is the ring advancing?)\n", ft.size()), 1;
    // Local spikes: frames well above the median of the 30 before (what an overlay graph shows as teeth).
    size_t spikes[3]{};
    std::vector<double> win;
    for (size_t i = 30; i < ft.size(); ++i) {
        win.assign(ft.begin() + (i - 30), ft.begin() + i);
        const double m = Median(win);
        spikes[0] += ft[i] > 1.25 * m, spikes[1] += ft[i] > 1.5 * m, spikes[2] += ft[i] > 2 * m;
    }
    std::vector<double> v = ft;
    std::sort(v.begin(), v.end());
    const auto p = [&](double q) { return v[size_t(q * double(v.size() - 1))]; };
    const auto low = [&](size_t k) { double s = 0; for (size_t i = 0; i < k; ++i) s += v[v.size() - 1 - i]; return 1000 * double(k) / s; };
    double sum = 0;
    for (double x : v) sum += x;
    std::printf("rtss     %6zu, avg %.0f fps, 1%% low %.0f, 0.1%% low %.0f; ms p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.2f; over", v.size(),
                1000 * double(v.size()) / sum, low(std::max<size_t>(1, v.size() / 100)), low(std::max<size_t>(1, v.size() / 1000)), p(.5), p(.9), p(.99), p(.999), v.back());
    for (double e : {3.0, 4.0, 5.0, 6.0, 8.0, 12.0}) std::printf(" %g:%zu", e, size_t(v.end() - std::upper_bound(v.begin(), v.end(), e)));
    std::printf("; spikes over the 30 before x1.25:%zu x1.5:%zu x2:%zu%s\n", spikes[0], spikes[1], spikes[2], lost ? " (ring wrapped: frames lost)" : "");
    return 0;
}
