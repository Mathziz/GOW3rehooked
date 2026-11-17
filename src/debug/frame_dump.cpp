// SPDX-License-Identifier: GPL-2.0-or-later
#include "debug/frame_dump.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>
#include "debug/dump_archive.h"
#include "main/host.h"
// Frame dump PNGs (F9).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "externals/stb/stb_image_write.h"

namespace Gow3::FrameDump {
namespace {
bool f9_down = false, every_nth = false;  // every_nth: this frame's trace is a GOW3_TEMP_TRACE_EVERY one (no images)
u64 frame = 0;
u32 every_pending = 0, f9_left = 0;  // f9_left: frames of the F9 dump still to trace
std::mutex encoding;  // the PNG threads, one at a time (stb's compression level is global)

void WritePngToVector(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<u8>*>(ctx);
    const auto* bytes = static_cast<const u8*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

void AddPng(std::string_view name, const Gpu::Image& img) {
    std::vector<u8> png;
    stbi_write_png_to_func(WritePngToVector, &png, int(img.w), int(img.h), 4, img.rgba.data(), int(img.w * 4));
    DumpArchive::Add(name, png);
}
}

void Begin(Gpu::Stats& stats) {
    // Live frame dump: F9 traces the next 3 frames (what changes between them); draw lines and render targets go to dumps\trace.
    const bool f9 = GetAsyncKeyState(VK_F9) & 0x8000;
    if (f9 && !f9_down) f9_left = 3;
    f9_down = f9;
    // GOW3_TEMP_TRACE_EVERY=N traces every Nth frame into dumps\trace\draws-<frame>.txt (no images).
    static const u64 every = [] { const char* e = DebugOption("GOW3_TEMP_TRACE_EVERY"); return e ? std::strtoull(e, nullptr, 10) : 0ull; }();
    every_nth = every && ++frame % every == 0 && !f9_left;
    stats.trace = f9_left || every_nth;
    if (f9_left) --f9_left;
    // GOW3_DUMP_STOP=<draw>: the dumped frame stops after that draw, so its targets show the intermediate state.
    static const char* dump_stop = DebugOption("GOW3_DUMP_STOP");
    stats.stop = stats.trace && dump_stop ? std::strtoull(dump_stop, nullptr, 10) : ~0ull;
}

void End(Gpu::Renderer& r, Gpu::Stats& stats) {
    if (!stats.trace) return;
    // Each F9 dump in its own folder inside the session .dmp archive, named by its local time (trace/HHMMSS).
    SYSTEMTIME now;
    GetLocalTime(&now);
    std::string dir = "trace";
    if (!every_nth) {
        std::string stamp = std::format("trace/{:02}{:02}{:02}", now.wHour, now.wMinute, now.wSecond);
        dir = stamp;
        for (u32 suffix = 2; DumpArchive::Contains(dir + "/draws.txt"); ++suffix) dir = std::format("{}-{}", stamp, suffix);
    }
    std::string report;
    for (size_t i = 0; i < stats.trace_lines.size(); ++i) report += std::format("draw {} {}\n", i, stats.trace_lines[i]);
    const std::string entry = every_nth ? std::format("{}/draws-{}.txt", dir, frame) : dir + "/draws.txt";
    DumpArchive::Add(entry, report);
    if (!every_nth) {  // each program's SPIR-V, named stage-program-key
        for (auto& [prog, spirv] : stats.trace_spirv)
            DumpArchive::Add(std::format("{}/spirv/{}.spv", dir, prog), spirv.data(), spirv.size());
    }
    if (every_nth) {  // batched every 64, flushed on exit as well
        if (++every_pending >= 64) DumpArchive::Flush(), every_pending = 0;
    } else {
        // The images encode on a thread of their own, so the next traced frames don't wait for them; flushed once encoded.
        // Textures (GOW3_DEBUG=2, or GOW3_DUMP_TEX=1): the guest textures the frame bound (mip 0, layer 0, alpha kept), and
        // each one's channel range in textures.txt.
        std::thread([dir, targets = r.ReadTargets(), textures = DebugDeep("GOW3_DUMP_TEX") ? r.ReadTextures(true) : std::vector<Gpu::Image>{}] {
            std::scoped_lock lock(encoding);
            for (const auto& t : targets)
                if (!t.rgba.empty()) AddPng(std::format("{}/{}-{:x}-{}x{}-f{}.png", dir, t.depth ? "z" : "rt", t.base, t.w, t.h, t.format), t);
            std::string list;
            stbi_write_png_compression_level = 2;
            for (const auto& t : textures) {
                u8 lo[4]{255, 255, 255, 255}, hi[4]{};
                for (size_t p = 0; p < t.rgba.size(); ++p) lo[p & 3] = std::min(lo[p & 3], t.rgba[p]), hi[p & 3] = std::max(hi[p & 3], t.rgba[p]);
                list += t.rgba.empty() ? std::format("tex {:x} {}x{} f{} not read (volume, integer format or no blit)\n", t.base, t.w, t.h, t.format)
                                       : std::format("tex {:x} {}x{} f{} rgba min {} {} {} {} max {} {} {} {}\n", t.base, t.w, t.h, t.format, lo[0], lo[1],
                                                     lo[2], lo[3], hi[0], hi[1], hi[2], hi[3]);
                if (!t.rgba.empty()) AddPng(std::format("{}/tex-{:x}-{}x{}-f{}.png", dir, t.base, t.w, t.h, t.format), t);
            }
            stbi_write_png_compression_level = 8;
            if (!list.empty()) DumpArchive::Add(dir + "/textures.txt", list);
            DumpArchive::Flush();
        }).detach();
    }
    constexpr u64 kLuminance = 0x2b9a02c00;  // 1x1 R16G16F the CPU reads for exposure
    Log(std::format("Frame dump: {} draws to {} ({}); guest luminance {:08x}", stats.trace_lines.size(), DumpArchive::Path().string(),
                    every_nth ? entry : dir, GuestMemory::Readable(kLuminance, 4) ? *reinterpret_cast<const u32*>(kLuminance) : 0xDEADBEEFu));
    stats.trace = false, stats.trace_lines.clear(), stats.trace_spirv.clear();
}

// Exploded vertices (GOW3_BOOM=1): a draw (vertex shader, pixel shader, vertices drawn) in the frames it starts exploding (polygons
// near the camera reach far off the screen frame after frame), its first 16 then every 256th, with the loads of its first 2: their
// address's guest memory now, page kind (U unknown, C clean, D dirty, Y dynamic, F fixed, R region), frame region and whether the
// retired frame offered it (STALE: a region outside the offer or an unknown page, the mirror did not get it that frame).
void Boom(Gpu::Stats& stats, const std::vector<GuestMemory::Range>& regions, const std::vector<GuestMemory::Range>& offer) {
    static std::vector<GuestMemory::Range> offer_before;  // the retired frame's, offered by the Render before this one
    static std::unordered_set<u64> before, now;           // draws that exploded in the frame retired before, in this one
    static std::unordered_map<u64, u32> boom_seen;
    now.clear();
    for (const auto& [key, head, loads] : stats.boom) {
        if (!now.insert(key).second || before.contains(key)) continue;
        const u32 n = boom_seen[key]++;
        if (n >= 16 && n % 256) continue;
        Log(head);
        if (n >= 2) continue;
        for (size_t j = 0; j + 5 < loads.size(); j += 6) {
            const u64 a = (u64)loads[j + 1] << 2;
            const u32 v = loads[j + 2], w = loads[j + 3];
            const bool readable = GuestMemory::Readable(a, 4);
            const u32 g = readable ? *reinterpret_cast<const u32*>(a) : 0;
            int region = -1;
            for (size_t q = 0; q < regions.size(); ++q)
                if (a >= regions[q].first && a < regions[q].second) region = int(q);
            const bool offered = std::any_of(offer_before.begin(), offer_before.end(), [&](const GuestMemory::Range& o) { return a >= o.first && a < o.second; });
            const char kind = GuestMemory::PageKind(a);
            Log(std::format("  load {:3} pc {:05x} @{:x} = {:08x} ({:g}){} guest now {}{} page {} region {} offered {}{} writers {:x} upload {} next {}", j / 6,
                            loads[j], a, v, std::bit_cast<float>(v), (v >> 23 & 0xFF) >= 161 ? " BAD" : "", readable ? std::format("{:08x}", g) : "unreadable",
                            readable && g != v ? " DIFF" : "", kind, region, offered, kind == 'U' || (region >= 0 && !offered) ? " STALE" : "", w,
                            w & 4 ? std::format("{:08x}{}", loads[j + 4], loads[j + 4] != v ? "*" : "") : "-",
                            w & 8 ? std::format("{:08x}{}", loads[j + 5], loads[j + 5] != v ? "*" : "") : "-"));
        }
    }
    stats.boom.clear();
    before.swap(now);
    // Stale reads: bytes a draw of this frame read that the mirror did not get this frame: in a region outside the frame's offer
    // (the mirror holds what an older frame offered there), on a page still unknown after its Track (never staged: unmapped or
    // without access then), or skipped because the frame in flight writes there. Per vertex shader and cause, when its run of
    // frames begins and every 64th frame of it.
    auto& reads = stats.reads;
    std::sort(reads.begin(), reads.end(), [](const auto& x, const auto& y) { return std::tie(x.lo, x.hi, x.prog) < std::tie(y.lo, y.hi, y.prog); });
    reads.erase(std::unique(reads.begin(), reads.end(), [](const auto& x, const auto& y) { return x.lo == y.lo && x.hi == y.hi && x.prog == y.prog; }), reads.end());
    auto& skips = stats.skips;
    std::sort(skips.begin(), skips.end());
    size_t merged = 0;
    for (const auto& s : skips)
        if (merged && s.first <= skips[merged - 1].second) skips[merged - 1].second = std::max(skips[merged - 1].second, s.second);
        else skips[merged++] = s;
    skips.resize(merged);
    const auto overlap = [](u64 lo, u64 hi, const std::vector<GuestMemory::Range>& v) {
        u64 n = 0;
        for (auto [a, b] : v) n += a < hi && lo < b ? std::min(hi, b) - std::max(lo, a) : 0;
        return n;
    };
    struct Stale {
        u64 lo, hi, bytes;
        u32 reads;
    };
    std::map<u64, Stale> stale;  // vertex shader | cause << 62
    for (const auto& r : reads) {
        u64 bytes[3]{0, 0, overlap(r.lo, r.hi, skips)};  // outside the offer, unknown, skipped
        for (u64 a = r.lo, end; a < r.hi; a = end) {
            end = std::min((a | 0xFFF) + 1, r.hi);
            if (const char k = GuestMemory::PageKind(a); k == 'U') bytes[1] += end - a;
            else if (k == 'R') bytes[0] += end - a - overlap(a, end, offer);
        }
        for (u64 c = 0; c < 3; ++c)
            if (bytes[c]) {
                Stale& s = stale[r.prog | c << 62];
                if (!s.reads++) s.lo = r.lo, s.hi = r.hi;
                s.bytes += bytes[c];
            }
    }
    static constexpr const char* kCause[]{"in a region outside the frame's offer", "on pages never staged (unknown)", "skipped: the frame in flight writes there"};
    static std::map<u64, std::pair<u64, u32>> runs;  // vertex shader | cause -> last frame seen, frames in the run (gaps of one go on)
    for (const auto& [key, s] : stale) {
        auto& [last, run] = runs[key];
        run = last && stats.frame - last <= 2 ? run + 1 : 0, last = stats.frame;
        if (run % 64) continue;
        std::string where;  // the regions under the first read, with what the frame offered of each
        for (size_t q = 0; key >> 62 == 0 && q < regions.size(); ++q)
            if (regions[q].first < s.hi && s.lo < regions[q].second) {
                where += std::format("; region {} [{:x}, {:x}) offered", q, regions[q].first, regions[q].second);
                const size_t had = where.size();
                for (auto [a, b] : offer)
                    if (a < regions[q].second && regions[q].first < b) where += std::format(" [{:x}, {:x})", a, b);
                if (where.size() == had) where += " nothing";
            }
        Log(std::format("Stale read: frame {} vs {:x}: {} reads, {} bytes {}, first [{:x}, {:x}) page {}{}", stats.frame, key & ~(3ull << 62), s.reads, s.bytes,
                        kCause[key >> 62], s.lo, s.hi, GuestMemory::PageKind(s.lo), where));
    }
    // Torn snapshots: a piece the game wrote while it was copied to the mirror's staging, with the frame's reads over the changed
    // bytes. Per page, the first 4 and every 256th.
    static std::map<u64, u32> tear_seen;
    for (auto& t : stats.tears) {
        if (const u32 n = tear_seen[t.addr >> 12]++; n >= 4 && n % 256) continue;
        for (const auto& r : reads)
            if (r.lo <= t.addr + t.last && t.addr + t.first < r.hi && !t.hits++) t.prog = r.prog, t.lo = r.lo, t.hi = r.hi;
        Log(std::format("Torn snapshot: frame {} page {} {:x}+{:x}: {} bytes changed during the copy, +{:x} to +{:x}; {} reads of the frame cover them{}", stats.frame,
                        GuestMemory::PageKind(t.addr), t.addr, t.n, t.bytes, t.first, t.last, t.hits,
                        t.hits ? std::format(" (vs {:x}, first [{:x}, {:x}))", t.prog, t.lo, t.hi) : std::string()));
    }
    // TEMP: the share of the offered ring and arenas the frame's work declares to Track (exact bytes, 512-byte blocks, 4 KB
    // pages), by region size (the arenas change slots every pass), summed over 256 frames. Empty unless GOW3_BOOM is on.
    if (!stats.gpu_reads.empty()) {
        auto& g = stats.gpu_reads;
        std::sort(g.begin(), g.end());
        size_t m = 0;
        for (const auto& r : g)
            if (m && r.first <= g[m - 1].second) g[m - 1].second = std::max(g[m - 1].second, r.second);
            else g[m++] = r;
        g.resize(m);
        const auto covered = [&](u64 a, u64 b, u64 align) {  // bytes of [a, b) in `align` blocks some read touches
            u64 n = 0, done = a;
            for (auto it = std::partition_point(g.begin(), g.end(), [&](const auto& r) { return ((r.second + align - 1) & ~(align - 1)) <= a; });
                 it != g.end() && (it->first & ~(align - 1)) < b; ++it) {
                const u64 lo = std::max({a, done, it->first & ~(align - 1)}), hi = std::min(b, (it->second + align - 1) & ~(align - 1));
                if (hi > lo) n += hi - lo, done = hi;
            }
            return n;
        };
        // region size -> offered (or deferred), read, in 512 B blocks, in 4 KB pages; read in such regions outside the offer
        static std::map<u64, std::array<u64, 5>> cover;
        static u32 frames = 0;
        for (auto [a, b] : offer) {
            u64 size = 0;
            for (auto [lo, hi] : regions)
                if (a >= lo && a < hi) size = hi - lo;
            auto& c = cover[size];
            c[0] += b - a, c[1] += covered(a, b, 1), c[2] += covered(a, b, 512), c[3] += covered(a, b, 4096);
        }
        for (auto [lo, hi] : regions) {  // stale unless an earlier frame staged them: the other half, the spares
            u64 inside = 0;
            for (auto [a, b] : offer)
                if (a >= lo && a < hi) inside += covered(a, std::min(b, hi), 1);
            cover[hi - lo][4] += covered(lo, hi, 1) - inside;
        }
        if (++frames == 256) {
            for (const auto& [size, c] : cover)
                Log(std::format("TEMP GPU reads of the offer, region of {:.1f} MB: offered {} KB/frame, declared read {} KB ({:.0f}%), in 512 B blocks {} KB ({:.0f}%), "
                                "in 4 KB pages {} KB ({:.0f}%); read outside the offer {} B/frame", size / 1048576.0, c[0] / 256 / 1024, c[1] / 256 / 1024, 100.0 * c[1] / std::max<u64>(c[0], 1),
                                c[2] / 256 / 1024, 100.0 * c[2] / std::max<u64>(c[0], 1), c[3] / 256 / 1024, 100.0 * c[3] / std::max<u64>(c[0], 1), c[4] / 256));
            cover.clear(), frames = 0;
        }
        g.clear();
    }
    reads.clear(), skips.clear(), stats.tears.clear();
    offer_before = offer;
}
} // namespace Gow3::FrameDump
