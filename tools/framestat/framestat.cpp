// Frame recorder analysis (logs\frames-*.bin, written when the game window closes in debug runs).
// Finds the display drops (flip intervals well above the running median) and, for each, what grew in the frames that
// made it against the frames before: the game's frame, the GPU thread's phases and stalls, the GPU, the presentation;
// the programs its frames compiled and what made each a new variant, and the run's most compiled programs.
//   framestat <frames.bin> [ratio 1.5] [extra ms 2.5: above one 480 Hz vblank, the flips' normal jitter]
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;

struct GameThread {  // as Recorder::GameThread (src/debug/frame_recorder.h)
    float cpu_ms, host_ms, longest_ms, fault_ms;
    u32 longest;
};
struct Frame {  // as Recorder::Frame (src/debug/frame_recorder.h)
    u64 n, submit, captured, start, rendered, processed;
    GameThread game;
    float ms[16];  // BeginFrame, chain, setup, walk, blocks, tail, xfer submit, fence, readback, submit,
                   // GPU (frame before) xfer, copy, render, readback copy, MB staged, texture uploads
    float compile_wait_ms, bind_ms, evict_ms, label_wait_ms;
    u32 compiles, compile_waits, binds, evicts, label_waits, faults, queries, draws;
    u32 reaped;  // evicted textures' objects destroyed on their thread, and the longest of them
    float reap_max_ms;
};
struct Flip {
    u64 at;
    int64_t late_ns;
    u32 pending, stale_us, flip_us, event_us;  // stale: already overdue when the vblank thread planned its sleep; its previous pass's work
};
struct Present {
    u64 start, acquired, end;
};
// The layout before `fault_ms` and the vblank thread's breakdown.
struct GameThreadV1 {
    float cpu_ms, host_ms, longest_ms;
    u32 longest;
};
struct FrameV1 {
    u64 n, submit, captured, start, rendered, processed;
    GameThreadV1 game;
    float ms[16];
    float compile_wait_ms, bind_ms, evict_ms, label_wait_ms;
    u32 compiles, compile_waits, binds, evicts, label_waits, faults, queries, draws;
    u32 reaped;
    float reap_max_ms;
};
struct FlipV1 {
    u64 at;
    int64_t late_ns;
    u32 pending, pad;
};
// ms .. reap_max_ms, without the struct's trailing padding.
constexpr size_t kTail = offsetof(Frame, reap_max_ms) + sizeof(float) - offsetof(Frame, ms);
static_assert(kTail == offsetof(FrameV1, reap_max_ms) + sizeof(float) - offsetof(FrameV1, ms));
struct Compile {  // as Recorder::Compile (Gpu::Stats::Compile)
    u64 n, prog, key;
    u32 stage, changed, slot, us, was[2], now[2];
};
const char* kChange[10]{"first", "code", "fetch", "rsrc1", "rsrc2", "rect", "slots", "spec", "ps", "threads"};

void PrintCompile(const Compile& c) {
    std::printf("      frame %llu %cs %llx %.1f ms%s:", (unsigned long long)c.n, "vpc"[c.stage % 3], (unsigned long long)c.prog, c.us * 1e-3,
                c.changed >> 31 ? " prefetched" : "");
    for (int b = 0; b < 10; ++b)
        if (c.changed >> b & 1) std::printf(" %s", kChange[b]);
    const auto spec = [](const u32* w) {
        std::printf("dim %u cls %u unnorm %u degamma %u srgb %u vfmt %u fmt %u/%u sel %03x part of image %ux%u", w[0] & 15, w[0] >> 4 & 15, w[0] >> 8 & 1,
                    w[0] >> 9 & 1, w[0] >> 10 & 1, w[0] >> 11 & 1, w[0] >> 12 & 15, w[0] >> 16 & 15, w[0] >> 20, w[1] & 0xFFFF, w[1] >> 16);
    };
    if (c.changed & 128) std::printf(" slot %u (", c.slot), spec(c.was), std::printf(") -> ("), spec(c.now), std::printf(")");
    std::printf("\n");
}

// Per-frame metrics: times (ms) the frame may lose in, then counts.
enum Metric { Game, Idle, Capture, Queue, Begin, Chain, Setup, Walk, Blocks, Tail, Xfer, Fence, Readback, Submit, Labels, LabelWait,
              CompileWait, Bind, Evict, Gpu, GpuCopy, Busy, Reap, Run, Host, Longest, kTimes, Compiles = kTimes, CompileWaits, Binds, Evicts, Faults, Queries, Draws,
              Staged, Uploads, Reaped, kMetrics };
const char* kName[kMetrics]{"game", "idle", "capture", "queue", "begin", "chain", "setup", "walk", "blocks", "tail", "xfer", "fence", "rdback",
                            "submit", "labels", "lwait", "cwait", "bind", "evict", "gpu", "gcopy", "busy", "reapmax", "run", "host", "hostmax", "compiles", "cwaits", "binds",
                            "evicts", "faults", "vquery", "draws", "MBstaged", "texup", "reaped"};
// What each time is: the verdict names the one that grew most (leaves only: Busy is their sum, Gpu explains Fence,
// LabelWait is inside Labels, CompileWait/Bind/Evict inside the recording phases).
const char* kMeaning[kTimes]{
    "game frame (submit to submit)", "GPU thread idle: the game submitted late (guest CPU)", "game thread: CaptureFrameMemory",
    "GPU thread picked the submit late", "BeginFrame: staging guest memory", "chain: the submit's command buffers as segments",
    "Submit setup: early wait for the frame in flight, then evictions / regrow (evict column)",
    "walk (serial recording)", "blocks (parallel recording)", "tail", "xfer submit", "fence: waiting for the GPU (frame before)",
    "readback (CPU)", "queue submit", "Process: PM4 labels and WAIT_REG_MEM", "WAIT_REG_MEM waits on guest labels",
    "recording waited for shader compiles", "sparse binds (queue wait idle)", "texture evictions / regrow",
    "GPU render time", "GPU copy time", "GPU thread busy", "evicted textures' destruction (longest object, its own thread)",
    "game thread CPU time", "game thread inside host calls", "game thread's longest host call"};
const char* kOther[2]{"vblank thread woke late (OS scheduling)", "present waited for a swapchain image"};
constexpr bool kLeaf[kTimes]{false, true, true, true, true, true, true, true, true, true, true, true, true, true, true, false,
                             false, false, false, false, false, false, false, false, false, false};

double Value(const std::vector<Frame>& f, size_t i, int m) {
    const Frame& x = f[i];
    const auto ms = [](u64 a, u64 b) { return b > a ? double(b - a) * 1e-6 : 0.0; };
    switch (m) {
    case Game: return i ? ms(f[i - 1].submit, x.submit) : 0;
    case Idle: return i ? ms(f[i - 1].processed, x.start) : 0;
    case Capture: return ms(x.submit, x.captured);
    case Queue: return ms(x.captured, x.start);
    case Begin: case Chain: case Setup: case Walk: case Blocks: case Tail: case Xfer: case Fence: case Readback: case Submit:
        return x.ms[m - Begin];
    case Labels: return ms(x.rendered, x.processed);
    case LabelWait: return x.label_wait_ms;
    case CompileWait: return x.compile_wait_ms;
    case Bind: return x.bind_ms;
    case Evict: return x.evict_ms;
    case Gpu: return x.ms[12];
    case GpuCopy: return x.ms[11];
    case Busy: return ms(x.start, x.processed);
    case Reap: return x.reap_max_ms;
    case Run: return x.game.cpu_ms;
    case Host: return x.game.host_ms;
    case Longest: return x.game.longest_ms;
    case Reaped: return x.reaped;
    case Compiles: return x.compiles;
    case CompileWaits: return x.compile_waits;
    case Binds: return x.binds;
    case Evicts: return x.evicts;
    case Faults: return x.faults;
    case Queries: return x.queries;
    case Draws: return x.draws;
    case Staged: return x.ms[14];
    case Uploads: return x.ms[15];
    default: return 0;
    }
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: framestat <frames.bin> [ratio 1.5] [extra ms 2.5] [from: steady_clock ns]\n"), 1;
    const double ratio = argc > 2 ? std::atof(argv[2]) : 1.5, extra = argc > 3 ? std::atof(argv[3]) : 2.5;
    const u64 from = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 0;  // drops before it are not reported
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return std::fprintf(stderr, "cannot open %s\n", argv[1]), 1;
    char magic[8];
    u32 sizes[4];
    u64 counts[4]{};
    if (std::fread(magic, 8, 1, in) != 1 || std::memcmp(magic, "GOW3FRM2", 8) || std::fread(sizes, sizeof(sizes), 1, in) != 1 ||
        std::fread(counts, sizeof(counts), 1, in) != 1 || sizes[2] != sizeof(Present) || sizes[3] != sizeof(Compile))
        return std::fprintf(stderr, "not a frame recording of this layout\n"), 1;
    // The current layout, or the one before the write-fault time and the vblank thread's breakdown (read with those zero).
    const bool old_layout = sizes[0] == sizeof(FrameV1) && sizes[1] == sizeof(FlipV1);
    if (!old_layout && (sizes[0] != sizeof(Frame) || sizes[1] != sizeof(Flip)))
        return std::fprintf(stderr, "not a frame recording of this layout\n"), 1;
    std::vector<Frame> frames(counts[0]);
    std::vector<Flip> flips(counts[1]);
    std::vector<Present> presents(counts[2]);
    std::vector<Compile> compiles(counts[3]);
    bool read = true;
    if (old_layout) {
        std::vector<FrameV1> old_frames(counts[0]);
        std::vector<FlipV1> old_flips(counts[1]);
        read = std::fread(old_frames.data(), sizeof(FrameV1), old_frames.size(), in) == old_frames.size() &&
               std::fread(old_flips.data(), sizeof(FlipV1), old_flips.size(), in) == old_flips.size();
        for (size_t i = 0; i < frames.size(); ++i) {
            Frame& f = frames[i];
            const FrameV1& o = old_frames[i];
            std::memcpy(&f, &o, offsetof(Frame, game));
            f.game = {o.game.cpu_ms, o.game.host_ms, o.game.longest_ms, 0.f, o.game.longest};
            std::memcpy(reinterpret_cast<char*>(&f) + offsetof(Frame, ms), reinterpret_cast<const char*>(&o) + offsetof(FrameV1, ms),
                        kTail);
        }
        for (size_t i = 0; i < flips.size(); ++i) flips[i] = {old_flips[i].at, old_flips[i].late_ns, old_flips[i].pending, 0, 0, 0};
    } else {
        read = std::fread(frames.data(), sizeof(Frame), frames.size(), in) == frames.size() &&
               std::fread(flips.data(), sizeof(Flip), flips.size(), in) == flips.size();
    }
    if (!read || std::fread(presents.data(), sizeof(Present), presents.size(), in) != presents.size() ||
        std::fread(compiles.data(), sizeof(Compile), compiles.size(), in) != compiles.size())
        return std::fprintf(stderr, "truncated recording\n"), 1;
    std::fclose(in);
    // The names of the host calls (`game.longest` ids), written beside the recording.
    std::vector<std::string> host_names(1);
    if (FILE* nf = std::fopen((std::string(argv[1]) + ".hle").c_str(), "rb")) {
        char line[1024];
        unsigned id;
        while (std::fgets(line, sizeof(line), nf))
            if (std::sscanf(line, "%u", &id) == 1 && id < 100000) {
                host_names.resize(std::max<size_t>(host_names.size(), id + 1));
                const char* tab = std::strchr(line, '\t');
                host_names[id] = tab ? std::string(tab + 1, std::strcspn(tab + 1, "\r\n")) : "";
            }
        std::fclose(nf);
    }
    const auto host_name = [&](u32 id) { return id < host_names.size() && !host_names[id].empty() ? host_names[id].c_str() : "-"; };
    // The game kept recording while the rings were written, so the oldest few slots can hold records newer than the rest.
    const auto trim = [](auto& v, auto key) {
        if (v.size() < 128) return;
        const u64 ref = key(v[64]);
        size_t n = 0;
        while (n < 64 && key(v[n]) > ref) ++n;
        v.erase(v.begin(), v.begin() + n);
    };
    trim(frames, [](const Frame& f) { return f.submit; });
    trim(flips, [](const Flip& f) { return f.at; });
    trim(presents, [](const Present& p) { return p.start; });
    if (frames.size() < 2 || flips.size() < 2) return std::fprintf(stderr, "too few frames\n"), 1;
    u64 t0 = flips.front().at;  // the frames' first record may be an unfinished one (submit 0)
    for (const Frame& f : frames)
        if (f.submit) {
            t0 = std::min(t0, f.submit);
            break;
        }
    const auto sec = [&](u64 t) { return double(t - t0) * 1e-9; };
    // FRAMESTAT_CSV=path: every frame's metrics, for plots and correlations.
    if (const char* csv = std::getenv("FRAMESTAT_CSV"); FILE* out = csv ? std::fopen(csv, "w") : nullptr) {
        std::fprintf(out, "n,t");
        for (int m = 0; m < kMetrics; ++m) std::fprintf(out, ",%s", kName[m]);
        std::fprintf(out, ",xfer_gpu\n");
        for (size_t i = 1; i < frames.size(); ++i) {
            std::fprintf(out, "%llu,%.4f", (unsigned long long)frames[i].n, sec(frames[i].submit));
            for (int m = 0; m < kMetrics; ++m) std::fprintf(out, ",%.3f", Value(frames, i, m));
            std::fprintf(out, ",%.3f\n", frames[i].ms[10]);
        }
        std::fclose(out);
    }

    // Display intervals and their running median (the 240 flips before).
    std::vector<double> gap(flips.size()), med(flips.size());
    for (size_t i = 1; i < flips.size(); ++i) gap[i] = double(flips[i].at - flips[i - 1].at) * 1e-6;
    std::vector<double> window;
    for (size_t i = 1; i < flips.size(); ++i) {
        window.assign(gap.begin() + std::max<ptrdiff_t>(1, ptrdiff_t(i) - 240), gap.begin() + i);
        med[i] = window.size() >= 30 ? Median(window) : 0;
    }
    std::vector<bool> drop(flips.size());
    for (size_t i = 1; i < flips.size(); ++i) drop[i] = flips[i].at >= from && med[i] > 0 && gap[i] > ratio * med[i] && gap[i] - med[i] > extra;
    if (from) std::printf("drops from %.1f s on\n", from > t0 ? sec(from) : 0.0);
    // Episodes: drops at most 10 flips apart.
    std::vector<std::pair<size_t, size_t>> episodes;
    for (size_t i = 1; i < flips.size(); ++i)
        if (drop[i]) {
            if (!episodes.empty() && i - episodes.back().second <= 10) episodes.back().second = i;
            else episodes.push_back({i, i});
        }
    std::printf("%zu frames, %zu flips, %zu presents over %.1f s; median display interval %.2f ms (%.0f fps); %zu drop episodes "
                "(interval > %.2gx median and +%.1f ms)\n\n",
                frames.size(), flips.size(), presents.size(), sec(flips.back().at), Median(std::vector<double>(gap.begin() + 1, gap.end())),
                1000 / Median(std::vector<double>(gap.begin() + 1, gap.end())), episodes.size(), ratio, extra);

    // From `from` on: the interval distribution of the display flips, the host presents (what an overlay measures) and the
    // game's submits, the 1% / 0.1% lows (mean of the slowest intervals), and the slowest 250 ms of each.
    if (from) {
        const auto dist = [&](const char* what, size_t n, auto&& at) {
            std::vector<double> v;
            u64 bin = 0, start = 0;
            size_t in_bin = 0, worst_bin = ~size_t(0);
            for (size_t i = 1; i < n; ++i) {
                if (at(i - 1) < from || at(i) <= at(i - 1)) continue;
                v.push_back(double(at(i) - at(i - 1)) * 1e-6);
                if (!start) start = at(i);
                if ((at(i) - start) / 250'000'000 != bin) worst_bin = std::min(worst_bin, in_bin), bin = (at(i) - start) / 250'000'000, in_bin = 0;
                ++in_bin;
            }
            if (v.size() < 100) return;
            size_t spikes[3]{};  // above the median of the 30 before (an overlay graph's teeth)
            std::vector<double> win;
            for (size_t i = 30; i < v.size(); ++i) {
                win.assign(v.begin() + (i - 30), v.begin() + i);
                const double m = Median(win);
                spikes[0] += v[i] > 1.25 * m, spikes[1] += v[i] > 1.5 * m, spikes[2] += v[i] > 2 * m;
            }
            std::sort(v.begin(), v.end());
            const auto p = [&](double q) { return v[size_t(q * double(v.size() - 1))]; };
            const auto low = [&](size_t k) { double s = 0; for (size_t i = 0; i < k; ++i) s += v[v.size() - 1 - i]; return 1000 * double(k) / s; };
            double sum = 0;
            for (double x : v) sum += x;
            std::printf("%-8s %6zu, avg %.0f fps, 1%% low %.0f, 0.1%% low %.0f, slowest 250 ms %.0f fps; ms p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.2f; over",
                        what, v.size(), 1000 * double(v.size()) / sum, low(std::max<size_t>(1, v.size() / 100)), low(std::max<size_t>(1, v.size() / 1000)),
                        4.0 * double(worst_bin), p(.5), p(.9), p(.99), p(.999), v.back());
            for (double e : {3.0, 4.0, 5.0, 6.0, 8.0, 12.0})
                std::printf(" %g:%zu", e, size_t(v.end() - std::upper_bound(v.begin(), v.end(), e)));
            std::printf("; spikes over the 30 before x1.25:%zu x1.5:%zu x2:%zu\n", spikes[0], spikes[1], spikes[2]);
        };
        dist("flips", flips.size(), [&](size_t i) { return flips[i].at; });
        dist("presents", presents.size(), [&](size_t i) { return presents[i].start; });
        dist("acquired", presents.size(), [&](size_t i) { return presents[i].acquired; });
        dist("pres.end", presents.size(), [&](size_t i) { return presents[i].end; });
        dist("submits", frames.size(), [&](size_t i) { return frames[i].submit; });
        // The present's own phases: the fence and acquire waits (start -> acquired), then recording, pacing and the present.
        std::vector<double> wait, rest;
        for (const Present& p : presents)
            if (p.start >= from && p.acquired >= p.start && p.end >= p.acquired) wait.push_back((p.acquired - p.start) * 1e-6), rest.push_back((p.end - p.acquired) * 1e-6);
        for (auto* v : {&wait, &rest}) {
            if (v->size() < 100) break;
            std::sort(v->begin(), v->end());
            const auto q = [&](double x) { return (*v)[size_t(x * double(v->size() - 1))]; };
            std::printf("present %-22s ms p10 %.3f p50 %.3f p90 %.3f p99 %.3f max %.2f\n", v == &wait ? "start -> acquired" : "acquired -> end", q(.1), q(.5), q(.9), q(.99), v->back());
        }
        std::printf("\n");
    }
    // Timeline, 10 s per row (FRAMESTAT_ROW=seconds): how the run changes (counts are sums over the row, times means or maxima).
    const u64 row = std::getenv("FRAMESTAT_ROW") ? std::max(1, std::atoi(std::getenv("FRAMESTAT_ROW"))) : 10;
    std::printf("timeline        frames  game  gpu  busy  fps  late_max stale_max flip_max event_max  drops | binds evicts reaped compiles faults texup MBstaged draws\n");
    for (u64 t = 0; sec(flips.back().at) >= double(t); t += row) {
        double game = 0, gpu = 0, busy = 0, staged = 0, draws = 0, late = 0, stale = 0, flip_work = 0, event_work = 0;
        u64 n = 0, binds = 0, evicts = 0, reaped = 0, compiles = 0, faults = 0, uploads = 0, slow = 0, flips_n = 0;
        for (size_t i = 1; i < frames.size(); ++i)
            if (sec(frames[i].submit) >= double(t) && sec(frames[i].submit) < double(t + row)) {
                ++n, game += Value(frames, i, Game), gpu += Value(frames, i, Gpu), busy += Value(frames, i, Busy), staged += frames[i].ms[14],
                    draws += frames[i].draws, binds += frames[i].binds, evicts += frames[i].evicts, reaped += frames[i].reaped,
                    compiles += frames[i].compiles, faults += frames[i].faults, uploads += u64(frames[i].ms[15]);
            }
        for (size_t i = 1; i < flips.size(); ++i)
            if (sec(flips[i].at) >= double(t) && sec(flips[i].at) < double(t + row)) {
                ++flips_n, late = std::max(late, double(flips[i].late_ns) * 1e-6), slow += drop[i], stale = std::max(stale, flips[i].stale_us * 1e-3),
                    flip_work = std::max(flip_work, flips[i].flip_us * 1e-3), event_work = std::max(event_work, flips[i].event_us * 1e-3);
            }
        if (n) std::printf("%4llu-%4llus  %7llu %5.2f %4.2f %5.2f %4.0f %9.1f %9.1f %8.1f %9.1f %6llu | %5llu %6llu %6llu %8llu %6llu %5llu %8.1f %5.0f\n", (unsigned long long)t,
                           (unsigned long long)t + row, (unsigned long long)n, game / double(n), gpu / double(n), busy / double(n), double(flips_n) / double(row), late,
                           stale, flip_work, event_work, (unsigned long long)slow, (unsigned long long)binds, (unsigned long long)evicts, (unsigned long long)reaped,
                           (unsigned long long)compiles, (unsigned long long)faults, (unsigned long long)uploads, staged / double(n), draws / double(n));
    }
    std::printf("\n");
    std::vector<int> verdicts(kTimes + 2);
    // Where the game thread's slowest frame of each episode went: inside a host call, running, or neither (off the CPU).
    int where[4]{};
    std::map<std::string, std::pair<int, double>> by_call;  // the host calls that held the slowest frames, count and ms
    for (size_t e = 0; e < episodes.size(); ++e) {
        const auto [a, b] = episodes[e];
        // The frames whose GPU work ended in the episode (a flip follows its labels).
        const u64 lo = flips[a - 1].at, hi = flips[b].at;
        size_t f0 = frames.size(), f1 = 0;
        for (size_t i = 1; i < frames.size(); ++i)
            if (frames[i].processed > lo - 20'000'000 && frames[i].processed <= hi) f0 = std::min(f0, i), f1 = i + 1;
        if (f0 >= f1) continue;
        const size_t base0 = f0 > 301 ? f0 - 300 : 1;
        double extra_ms = 0;
        int drops = 0;
        for (size_t i = a; i <= b; ++i)
            if (drop[i]) ++drops, extra_ms += gap[i] - med[i];
        std::printf("== drop %zu at %.3f s: %d slow flips in %zu, +%.1f ms over the median %.2f ms; intervals", e + 1, sec(flips[a].at), drops,
                    b - a + 1, extra_ms, med[a]);
        for (size_t i = a; i <= std::min(b, a + 15); ++i) std::printf(" %.1f", gap[i]);
        std::printf("%s\n", b > a + 15 ? " ..." : "");
        // Each metric: median of the 300 frames before; mean, max and summed growth over the episode's frames.
        double base[kMetrics], mean[kMetrics], peak[kMetrics], grown[kMetrics];
        std::vector<double> v;
        for (int m = 0; m < kMetrics; ++m) {
            v.clear();
            for (size_t i = base0; i < f0; ++i) v.push_back(Value(frames, i, m));
            base[m] = Median(v), mean[m] = 0, peak[m] = 0, grown[m] = 0;
            for (size_t i = f0; i < f1; ++i) {
                const double x = Value(frames, i, m);
                mean[m] += x, peak[m] = std::max(peak[m], x), grown[m] += std::max(0.0, x - base[m]);
            }
            mean[m] /= double(f1 - f0);
        }
        // Presentation and vblank thread in the episode's window.
        double acquire = 0, late = 0;
        u32 pending = 0;
        for (const Present& p : presents)
            if (p.start >= lo && p.start <= hi) acquire = std::max(acquire, double(p.acquired - p.start) * 1e-6);
        // The vblank thread's own lateness: what it overshot its deadline by after planning its sleep. The rest of `late_ns` is
        // the flip arriving after its deadline (stale): the game submitted it late, not a scheduling delay.
        for (size_t i = a; i <= b; ++i)
            late = std::max(late, std::max(0.0, double(flips[i].late_ns) * 1e-6 - flips[i].stale_us * 1e-3)), pending = std::max(pending, flips[i].pending);
        // The cause: what grew most among the frames' leaf phases, the vblank thread's wake-up and the present's acquire.
        int worst = -1;
        double worst_ms = 0;
        for (int m = 0; m < kTimes; ++m)
            if (kLeaf[m] && grown[m] > worst_ms) worst = m, worst_ms = grown[m];
        if (late > worst_ms) worst = kTimes, worst_ms = late;
        if (acquire > worst_ms) worst = kTimes + 1, worst_ms = acquire;
        if (worst >= 0) ++verdicts[worst];
        std::printf("   cause: %s, +%.1f ms over %zu frames\n", worst < 0 ? "none of the recorded phases grew" : worst < kTimes ? kMeaning[worst] : kOther[worst - kTimes],
                    worst_ms, f1 - f0);
        std::printf("   grew (median before -> mean / max in the drop):");
        for (int m = 0; m < kMetrics; ++m) {
            const bool time = m < kTimes;
            if ((time && peak[m] - base[m] > 0.5) || (!time && mean[m] > base[m] * 1.5 + 1))
                std::printf(" %s %.2f->%.2f/%.2f", kName[m], base[m], mean[m], peak[m]);
        }
        std::printf("\n   present acquire max %.2f ms, vblank thread late max %.2f ms, flips pending max %u\n", acquire, late, pending);
        {  // the latest flip of the episode: how much of its lateness the vblank thread owed to its own work before it
            size_t w = a;
            for (size_t i = a; i <= b; ++i)
                if (flips[i].late_ns > flips[w].late_ns) w = i;
            std::printf("   vblank thread, latest flip: late %.2f ms, already overdue when planned %.2f ms, previous pass: completing the flip %.2f ms, vblank events %.2f ms\n",
                        double(flips[w].late_ns) * 1e-6, flips[w].stale_us * 1e-3, flips[w].flip_us * 1e-3, flips[w].event_us * 1e-3);
        }
        // The game thread's slowest frame in the episode: its wall time, the CPU it ran, the time in host calls and the longest call.
        size_t slow = f0;
        for (size_t i = f0; i < f1; ++i)
            if (Value(frames, i, Game) > Value(frames, slow, Game)) slow = i;
        {
            const double wall = Value(frames, slow, Game), run = frames[slow].game.cpu_ms, host = frames[slow].game.host_ms, fault = frames[slow].game.fault_ms;
            const int w = host >= 0.6 * wall ? 0 : fault >= 0.4 * wall ? 3 : run >= 0.6 * wall ? 1 : 2;
            ++where[w];
            if (w == 0) {
                auto& c = by_call[host_name(frames[slow].game.longest)];
                ++c.first, c.second += frames[slow].game.longest_ms;
            }
            std::printf("   game thread, slowest frame %llu: %.1f ms, ran %.1f ms on the CPU, %.1f ms inside host calls (longest: %s %.1f ms), %.1f ms in write-fault handling -> %s\n",
                        (unsigned long long)frames[slow].n, wall, run, host, host_name(frames[slow].game.longest), frames[slow].game.longest_ms, fault,
                        w == 0 ? "waiting in a host call" : w == 3 ? "in the page tracker's write faults" : w == 1 ? "running guest code" : "off the CPU outside host calls (preempted)");
        }
        const int cols[]{Game, Run, Host, Longest, Idle, Busy, Begin, Walk, Blocks, Fence, Submit, Gpu, Labels, CompileWait, Bind, Evict, Reap, Reaped, Compiles, Faults, Queries, Draws,
                         Staged, Uploads};
        std::printf("   %8s", "frame");
        for (int m : cols) std::printf(" %7s", kName[m]);
        std::printf("\n   %8s", "before");
        for (int m : cols) std::printf(" %7.2f", base[m]);
        std::printf("\n");
        for (size_t i = f0; i < std::min(f1, f0 + 16); ++i) {
            std::printf("   %8llu", (unsigned long long)frames[i].n);
            for (int m : cols) std::printf(" %7.2f", Value(frames, i, m));
            std::printf("\n");
        }
        // The programs these frames compiled and what made each a new variant of its program.
        auto c = std::lower_bound(compiles.begin(), compiles.end(), frames[f0].n, [](const Compile& x, u64 n) { return x.n < n; });
        if (c != compiles.end() && c->n <= frames[f1 - 1].n) std::printf("   compiled:\n");
        for (int k = 0; c != compiles.end() && c->n <= frames[f1 - 1].n; ++c, ++k)
            if (k < 24) PrintCompile(*c);
            else if (k == 24) std::printf("      ...\n");
        std::printf("\n");
    }
    std::printf("Causes over %zu episodes:", episodes.size());
    for (int m = 0; m < kTimes + 2; ++m)
        if (verdicts[m]) std::printf("\n  %3d  %s", verdicts[m], m < kTimes ? kMeaning[m] : kOther[m - kTimes]);
    std::printf("\nGame thread's slowest frame of each episode: %d waiting in a host call, %d in the page tracker's write faults, %d running guest code, %d off the CPU outside host calls\n",
                where[0], where[3], where[1], where[2]);
    std::vector<std::pair<std::string, std::pair<int, double>>> calls(by_call.begin(), by_call.end());
    std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    for (size_t i = 0; i < std::min<size_t>(calls.size(), 12); ++i)
        std::printf("  %3d x %-60s %.1f ms in total\n", calls[i].second.first, calls[i].first.c_str(), calls[i].second.second);
    // Over the whole run: the programs compiled more than once, most first, why, and their last new variant.
    struct Prog {
        u32 n = 0, why[11]{};
        double ms = 0;
        const Compile* last = nullptr;
    };
    std::map<u64, Prog> progs;
    for (const Compile& x : compiles) {
        Prog& p = progs[x.prog | x.stage];
        ++p.n, p.ms += x.us * 1e-3, p.last = &x;
        for (int b = 0; b < 10; ++b) p.why[b] += x.changed >> b & 1;
        p.why[10] += x.changed >> 31;
    }
    std::vector<const Prog*> top;
    for (const auto& [k, p] : progs)
        if (p.n > 1) top.push_back(&p);
    std::sort(top.begin(), top.end(), [](const Prog* a, const Prog* b) { return a->n > b->n; });
    std::printf("\n%zu compiles of %zu programs; compiled more than once:\n", compiles.size(), progs.size());
    for (size_t i = 0; i < std::min<size_t>(top.size(), 20); ++i) {
        const Prog& p = *top[i];
        std::printf("  %4u x %cs %llx, %.0f ms:", p.n, "vpc"[p.last->stage % 3], (unsigned long long)p.last->prog, p.ms);
        for (int b = 0; b < 11; ++b)
            if (p.why[b]) std::printf(" %s %u", b < 10 ? kChange[b] : "prefetched", p.why[b]);
        std::printf("\n");
        PrintCompile(*p.last);
    }
    return 0;
}
