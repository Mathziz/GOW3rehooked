// SPDX-License-Identifier: GPL-2.0-or-later
// Guest memory and buffers: the renderer's sparse mirror of guest memory (page binding, staging chunks, uploads to VRAM) and
// the tracker that keeps it current.
#include "gpu/buffers.h"
#include "gpu/renderer_impl.h"
#include <bit>
#ifndef GOW3_OFFLINE
#include "gpu/gpu_thread.h"
#endif

namespace Gpu {

// ---- Buffers and sparse guest mirror ----

// `shared`: used on the copy engine too (no ownership transfers).
Renderer::Impl::Buffer Renderer::Impl::MakeBuffer(u64 size, VkBufferUsageFlags usage, bool host, const char* temp_tag, bool shared) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    const u32 both[]{family, copy_family};
    if (shared && copy_family != ~0u) ci.sharingMode = VK_SHARING_MODE_CONCURRENT, ci.queueFamilyIndexCount = 2, ci.pQueueFamilyIndices = both;
    if (const VkResult e = vkCreateBuffer(dev, &ci, nullptr, &b.buf); e != VK_SUCCESS)
        Report(std::format("vkCreateBuffer {}: {} bytes, usage {:x}, {}", (int)e, size, (u32)ci.usage, temp_tag));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b.buf, &r);
    VkMemoryPropertyFlags want = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // Host buffers that are not upload sources are read by the CPU: uncached (write-combined or BAR) reads crawl.
    if (host && !(usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) && Type(r.memoryTypeBits, want | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != ~0u)
        want |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    b.mem = Alloc(r, want, true);
    vkBindBufferMemory(dev, b.buf, b.mem, 0);
    if (host) vkMapMemory(dev, b.mem, 0, size, 0, (void**)&b.ptr);
    else device_buffers += size;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, b.buf};
    b.addr = vkGetBufferDeviceAddress(dev, &ai);
    if (temp_get_fault) {  // fault diagnostics
        std::scoped_lock temp_l(temp_alloc_lock);
        temp_allocs.push_back({(u64)b.buf, temp_tag, size, frame, ~0u, slot, false});
    }
    return b;
}

void Renderer::Impl::FreeBuffer(Buffer& b) {
    if (b.buf && temp_get_fault) {
        std::scoped_lock temp_l(temp_alloc_lock);
        for (auto it = temp_allocs.rbegin(); it != temp_allocs.rend(); ++it)
            if (it->handle == (u64)b.buf && it->freed == ~0u) {
                it->freed = frame, it->busy = flight.busy;
                break;
            }
    }
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr), vkFreeMemory(dev, b.mem, nullptr);
    if (b.buf && !b.ptr) device_buffers -= b.size;
    b = {};
}

// Mirror: sparse, resident only where guest memory was written.
bool Renderer::Impl::SetupMirror(std::string& error) {
    VkBufferCreateInfo mci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                           kMirrorSize,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    const u32 both[]{family, copy_family};  // the copy engine uploads it
    if (copy_family != ~0u) mci.sharingMode = VK_SHARING_MODE_CONCURRENT, mci.queueFamilyIndexCount = 2, mci.pQueueFamilyIndices = both;
    if (vkCreateBuffer(dev, &mci, nullptr, &mirror) != VK_SUCCESS) return error = "sparse mirror buffer failed", false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, mirror, &mr);
    page = mr.alignment, mirror_type = Type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    bound.assign(kMirrorSize / page / 64 + 1, 0);
    VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, mirror};
    mirror_addr = vkGetBufferDeviceAddress(dev, &bai);
    return true;
}

void Renderer::Impl::DestroyMirror() {
    vkDestroyBuffer(dev, mirror, nullptr);
    for (auto m : chunks) vkFreeMemory(dev, m, nullptr);
    vkDestroyDescriptorPool(dev, table_pool, nullptr), vkDestroyDescriptorSetLayout(dev, table_layout, nullptr);
}

// Per-frame draw tables: the host staging buffer, its VRAM twin, and the dynamic uniform buffer sets over them.
void Renderer::Impl::SetupTables() {
    tables_host = MakeBuffer(kTableBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "?", true);
    flight.tables_host = MakeBuffer(kTableBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "?", true);
    if (boom_on) boom_buf = MakeBuffer(2ull * kBoomDraws * kBoomRec, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true), std::memset(boom_buf.ptr, 0, boom_buf.size);
    for (Buffer* t : {&tables, &flight.tables})  // a table window starting at the last offset stays inside
        *t = MakeBuffer(kTableBytes + kTableRange, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                        "?", true);
    // Draw tables (set 1): the VS's or CS's and the PS's, dynamic uniform buffers over the frame slot's `tables`, so the
    // programs read them from the constant bank; each draw binds its offsets.
    const VkDescriptorSetLayoutBinding tbl[]{{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL},
                                             {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL}};
    const VkDescriptorSetLayoutCreateInfo tlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, tbl};
    vkCreateDescriptorSetLayout(dev, &tlci, nullptr, &table_layout);
    const VkDescriptorPoolSize tps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4};
    const VkDescriptorPoolCreateInfo tpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 2, 1, &tps};
    vkCreateDescriptorPool(dev, &tpci, nullptr, &table_pool);
    const VkDescriptorSetLayout two[2]{table_layout, table_layout};
    const VkDescriptorSetAllocateInfo tsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, table_pool, 2, two};
    vkAllocateDescriptorSets(dev, &tsai, table_sets);
    for (u32 k = 0; k < 2; ++k) {  // slot 0 starts with `tables`, slot 1 with the flight's (they swap together)
        const VkDescriptorBufferInfo bi{(k ? flight.tables : tables).buf, 0, kTableRange};
        VkWriteDescriptorSet w[2]{};
        for (u32 b = 0; b < 2; ++b)
            w[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, table_sets[k], b, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &bi};
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    table_align = std::max<u64>(16, pp.limits.minUniformBufferOffsetAlignment);
}

// Gives memory to every mirror page in [lo, hi) that has none.
void Renderer::Impl::BindPages(u64 lo, u64 hi) {
    std::scoped_lock bind(bind_lock);
    for (u64 p = lo / page * page; p < std::min(hi, kMirrorSize); p += page) {
        u64& word = bound[p / page / 64];
        const u64 bit = 1ull << (p / page % 64);
        if (word & bit) continue;
        word |= bit;
        if (chunk_used + page > kChunk) {
            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, kChunk, mirror_type};
            VkDeviceMemory m;
            vkAllocateMemory(dev, &ai, nullptr, &m);
            chunks.push_back(m), chunk_used = 0, mirror_chunks += kChunk;
        }
        if (!pending_binds.empty() && pending_binds.back().resourceOffset + pending_binds.back().size == p &&
            pending_binds.back().memory == chunks.back() &&
            pending_binds.back().memoryOffset + pending_binds.back().size == chunk_used)
            pending_binds.back().size += page;
        else
            pending_binds.push_back({p, page, chunks.back(), chunk_used, 0});
        chunk_used += page;
    }
}

// ---- Staging chunks and guest-to-mirror uploads ----

Renderer::Impl::Pending Renderer::Impl::AllocateStage(u64 n) {
    constexpr u64 kStageChunk = 64ull << 20;
    while (stage_at < stage.size() && stage_used + n > stage[stage_at].size) ++stage_at, stage_used = 0;
    if (stage_at == stage.size())
        stage.push_back(MakeBuffer(std::max(kStageChunk, n), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, "stage", true)),
            stage_dev.push_back(MakeBuffer(std::max(kStageChunk, n), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, "stage dev", true));
    const Pending p{0, n, stage_used, stage_at};
    stage_used += n;
    return p;
}

void Renderer::Impl::QueueStage(Pending p) {
    if (Pending* b = pending.size() > sent ? &pending.back() : nullptr;
        b && b->chunk == p.chunk && b->addr + b->size == p.addr && b->src + b->size == p.src)
        b->size += p.size;
    else
        pending.push_back(p);
}

const u8* Renderer::Impl::Stage(u64 addr, const u8* src, u64 n, u64* hash) {
    std::scoped_lock lock(stage_lock);
    InvalidateBlocks(addr, n);
    auto p = AllocateStage(n);
    u8* const copy = stage[p.chunk].ptr + p.src;
    if (hash) *hash = Gpu::PieceHash::Copy(copy, src, n);
    else std::memcpy(copy, src, n);
    p.addr = addr, QueueStage(p);
    return copy;
}

// The bytes staged before the frame records (BeginFrame's) go to VRAM on the copy engine now, under the frame in flight's
// render, so only a VRAM copy stays in front of this frame's draws. The later `copied` signal on that queue covers it.
void Renderer::Impl::RecordSend(VkCommandBuffer c) {
    std::vector<VkBufferCopy> copies;
    for (size_t i = sent, j; i < pending.size(); i = j) {
        for (j = i; j < pending.size() && pending[j].chunk == pending[i].chunk;) ++j;
        copies.clear();
        for (size_t k = i; k < j; ++k) {
            const auto& p = pending[k];
            if (!copies.empty() && copies.back().srcOffset + copies.back().size == p.src) copies.back().size += p.size;
            else copies.push_back({p.src, p.src, p.size});
        }
        vkCmdCopyBuffer(c, stage[pending[i].chunk].buf, stage_dev[pending[i].chunk].buf, (u32)copies.size(), copies.data());
    }
    sent = pending.size();
}

void Renderer::Impl::Send() {
    if (sent == pending.size()) return;
    const VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandBuffer(send_cmd, 0), vkBeginCommandBuffer(send_cmd, &bi);
    TempLabel(send_cmd, "mirror send");  // TEMP perf
    RecordSend(send_cmd);
    TempLabelEnd(send_cmd);  // TEMP perf
    vkEndCommandBuffer(send_cmd);
    const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &send_cmd};
    std::scoped_lock lock(*xfer_lock);
    TempLost("send submit", vkQueueSubmit(xfer, 1, &si, VK_NULL_HANDLE));  // TEMP fault
}

// Staged guest writes -> mirror, recorded at the start of the frame's command buffer: the sent ones from VRAM.
void Renderer::Impl::Flush(VkCommandBuffer c) {
    std::vector<VkBufferCopy> copies;
    if (boom_on) boom_up[slot].clear();
    for (size_t i = 0; i < pending.size();) {
        const u32 chunk = pending[i].chunk;
        const bool dev = i < sent;
        copies.clear();
        for (; i < pending.size() && pending[i].chunk == chunk && (i < sent) == dev; ++i) {
            const Pending& p = pending[i];
            BindPages(p.addr, p.addr + p.size);
            copies.push_back({p.src, p.addr, p.size});
            stats.uploaded += p.size;
            if (boom_on) boom_up[slot].push_back(p);
        }
        vkCmdCopyBuffer(c, (dev ? stage_dev : stage)[chunk].buf, mirror, (u32)copies.size(), copies.data());
    }
    pending.clear(), sent = 0;
}

void Renderer::Impl::NoteTear(u64 addr, const u8* staged, const u8* live, u64 n) {
    u32 first = ~0u, last = 0, bytes = 0;
    for (u32 i = 0; i < n; ++i)
        if (staged[i] != live[i]) first = std::min(first, i), last = i, ++bytes;
    std::scoped_lock lock(tear_lock);
    if (tears.size() < 4096) tears.push_back({addr, 0, 0, 0, u32(n), first, last, bytes, 0});
}

bool Renderer::Write(u64 addr, const void* data, u64 size) {
    WriteRange w{addr, data, size};
    Write({&w, 1});
    return w.changed;
}

void Renderer::Write(std::span<WriteRange> ranges) {
    // Unchanged 4 KB pieces are skipped (the same guest memory is offered every frame). So are the bytes the frame in
    // flight writes: the mirror gets them first, and guest memory only once that frame retires.
    struct Piece {
        u64 a, n, h;
        const u8* p;
        WriteRange* w;
        u64 capture;
        bool copied = false;
        Impl::BlockHashes* blocks = nullptr;
        u64 previous = 0, hashes[0x1000 / Gpu::PieceHash::kMemoryBlock]{};
        u32 mask = 0, want = 0xFF;  // blocks: changed, offered
        u64 witness = 0;            // blocks: the witness the worker compared against
    };
    static thread_local std::vector<Piece> own;
    std::vector<Piece>& pieces = own;  // the caller's, on every thread
    pieces.clear();
    u64 capture_size = 0;
    for (WriteRange& w : ranges) {
        if (w.addr + w.size > kMirrorSize) d->Note(std::format("TEMP write beyond the mirror {:#x}+{:#x}", w.addr, w.size));  // TEMP fault
        const u64 end = w.addr < kMirrorSize ? std::min(w.addr + w.size, kMirrorSize) : w.addr;  // the mirror holds guest addresses below 32 GB
        for (u64 a = w.addr, n; a < end; a += n) {
            n = std::min<u64>(0x1000 - (a & 0xFFF), end - a);
            const u32 want = w.blocks && n == 0x1000 ? u32(w.blocks[(a - w.addr) >> 15] >> ((a - w.addr) >> 9 & 63) & 0xFF) : 0xFF;
            if (!want) continue;  // no block of the page asked for
            pieces.push_back({a, n, w.capture ? d->WrittenSlot(a, n) : 0, (const u8*)w.data + (a - w.addr), &w, w.capture ? capture_size : ~0ull});
            if (w.capture && n == 0x1000) {
                auto& b = d->written_blocks[a >> 20];
                if (!b) b = std::make_unique<Impl::BlockHashes[]>(256);
                pieces.back().blocks = &b[(a >> 12) & 255], pieces.back().want = want;
            }
            if (w.capture) capture_size = (capture_size + n + 63) & ~63ull;
        }
    }
    Impl::Pending capture{};
    u8* captured = nullptr;
    if (capture_size) {
        std::scoped_lock lock(d->stage_lock);
        capture = d->AllocateStage(capture_size);
        captured = d->stage[capture.chunk].ptr + capture.src;
    }
    // The hashes (0: the frame in flight writes there) on every recording thread, unless this one is recording.
    const auto& inflight = d->flight.back;
    const auto changed_blocks = [](u32 mask, auto fn) {
        while (mask) {
            const u32 b = std::countr_zero(mask), n = std::countr_one(mask >> b);
            fn(b * Gpu::PieceHash::kMemoryBlock, n * Gpu::PieceHash::kMemoryBlock);
            mask &= ~(((1u << n) - 1) << b);
        }
    };
    std::atomic<size_t> next{0};
    const std::function<void(u32)> hash = [&](u32) {
        RenderProbe::Scope hash_time(RenderProbe::Hash, true);
        alignas(64) u8 snapshot[0x1000];  // reused in this worker's L1; the guest bytes are read only once
        for (size_t k; (k = next.fetch_add(32, std::memory_order_relaxed)) < pieces.size();)
            for (Piece& q : std::span(pieces).subspan(k, std::min<size_t>(32, pieces.size() - k))) {
                if (std::any_of(inflight.begin(), inflight.end(), [&](const VkBufferCopy& c) { return q.a < c.srcOffset + c.size && c.srcOffset < q.a + q.n; })) q.h = 0;
                else if (q.capture == ~0ull) q.h = Impl::PieceHash(q.p, q.n);
                else {
                    q.previous = q.h;
                    if (q.blocks) {
                        // The stored block hashes are the mirror's while their witness is its page hash; the blocks not
                        // asked for keep theirs (unknown, 0, when they are not the mirror's).
                        q.witness = q.blocks->witness;
                        const bool valid = q.previous && q.witness == q.previous;
                        if (valid && q.want != 0xFF) std::copy_n(q.blocks->hashes, std::size(q.hashes), q.hashes);
                        q.h = Gpu::PieceHash::CopyBlocks(snapshot, q.p, q.n, q.hashes, q.want);
                        if (!valid) q.mask = q.want;
#if defined(__AVX2__)
                        else for (u32 b = 0; b < std::size(q.hashes); b += 4) {
                            const auto old = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q.blocks->hashes + b));
                            const auto now = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q.hashes + b));
                            q.mask |= (u32(~_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(old, now)))) & 15) << b;
                        }
#else
                        else for (u32 b = 0; b < std::size(q.hashes); b += 2) {
                            const auto old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q.blocks->hashes + b));
                            const auto now = _mm_loadu_si128(reinterpret_cast<const __m128i*>(q.hashes + b));
                            q.mask |= (u32(~_mm_movemask_pd(_mm_castsi128_pd(_mm_cmpeq_epi64(old, now)))) & 3) << b;
                        }
#endif
                        changed_blocks(q.mask, [&](u64 o, u64 n) { std::memcpy(captured + q.capture + o, snapshot + o, n); });
                        if (!q.mask) q.h = q.previous;
                        else if (q.h == q.previous) q.h ^= 2;
                        q.copied = q.mask != 0;
                    } else {
                        q.h = Gpu::PieceHash::Copy(snapshot, q.p, q.n);
                        if (q.h != q.previous) std::memcpy(captured + q.capture, snapshot, q.n), q.copied = true;
                    }
                }
            }
        if (capture_size) _mm_sfence();  // publish WC stores before the worker signals completion
    };
    if (pieces.size() > 64 && !Impl::in_worker) d->Parallel(hash);
    else hash(0);
    RenderProbe::Scope apply_time(RenderProbe::Apply, true);
    for (const Piece& q : pieces) {
        u64* w = q.h ? &d->WrittenSlot(q.a, q.n) : nullptr;
        const bool changed = w && *w != q.h;
        // Earlier overlapping writes may have invalidated the block comparisons made by the workers.
        const bool fused = changed && q.copied && (!q.blocks || q.mask == (1u << std::size(q.hashes)) - 1 ||
                                                  (*w == q.previous && q.blocks->witness == q.witness));
        const u64 staged = q.copied ? (q.blocks ? std::popcount(q.mask) * Gpu::PieceHash::kMemoryBlock : q.n) : 0;
        RenderProbe::Piece(q.a, q.blocks ? std::popcount(q.want) * Gpu::PieceHash::kMemoryBlock : q.n,
                           !w ? RenderProbe::InFlight : !changed ? RenderProbe::Same : *w ? RenderProbe::Changed : RenderProbe::New, false, fused, staged);
        if (changed) {
            q.w->changed = true;
            const u8* copy;
            if (fused) {
                std::scoped_lock lock(d->stage_lock);
                if (q.blocks) {
                    changed_blocks(q.mask, [&](u64 o, u64 n) { d->QueueStage({q.a + o, n, capture.src + q.capture + o, capture.chunk}); });
                    q.blocks->witness = q.h;
                    std::copy(std::begin(q.hashes), std::end(q.hashes), q.blocks->hashes);
                } else d->QueueStage({q.a, q.n, capture.src + q.capture, capture.chunk});
                *w = q.h, copy = captured + q.capture;
            } else copy = d->Stage(q.a, q.p, q.n, w);
            if (!q.blocks && *w != q.h) RenderProbe::Piece(q.a, q.n, RenderProbe::Changed, true);
            if (Impl::boom_on) {
                const u64 step = q.blocks && copy == captured + q.capture ? Gpu::PieceHash::kMemoryBlock : q.n;
                for (u64 o = 0; o < q.n; o += step)
                    if ((step == q.n || (q.mask & (1u << (o / step)))) && std::memcmp(copy + o, q.p + o, step))
                        d->NoteTear(q.a + o, copy + o, q.p + o, step);
            }
            // Part of a page changed: so did the page for Version (its whole piece is staged again at its next offer).
            if (q.n != 0x1000) {
                if (u64& page = d->written[q.a >> 12]) page ^= 2;
                d->InvalidateBlocks(q.a);
            }
        } else if (!q.h && Impl::boom_on) {  // skipped: the frame in flight writes there
            std::scoped_lock lock(d->tear_lock);
            if (auto& s = d->skips; !s.empty() && s.back().second == q.a) s.back().second += q.n;
            else s.push_back({q.a, q.a + q.n});
        }
    }
}

// ---- Mirror-to-host readback ----

// Live linear readback: the frame's GPU writes, mirror -> host, handed to the caller when the frame retires. With the
// copy engine's second queue: mirror -> rb_dev here (VRAM), rb_dev -> host there once the frame is done. Returns the bytes.
u64 Renderer::Impl::RecordReadback(const VkCommandBufferBeginInfo& bi) {
    back.clear();
    u64 rb_total = 0;
    if (readback)
        for (auto& w : gpu_writes)  // a write left in its image has not reached the mirror (tiled: the CPU does not read it)
            if (!w.lazy) {
#ifndef GOW3_OFFLINE
                // The game's 4K Video image is rendered and sampled on the GPU.
                const bool video_output = Gow3::GpuThread::IsVideoOutput(w.lo, w.hi - w.lo);
                Gow3::GpuThread::TraceVideoReadback(w.lo, w.hi - w.lo, frame, video_output);
                if (video_output) continue;
#endif
                back.push_back({w.lo, rb_total, w.hi - w.lo}), rb_total += w.hi - w.lo;
            }
    if (!back.empty()) {
        const u64 size = std::max<u64>(rb_total, 16ull << 20);
        if (rb.size < rb_total) FreeBuffer(rb), rb = MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, "rb", true);
        if (rbq && rb_dev.size < rb_total)
            FreeBuffer(rb_dev), rb_dev = MakeBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, "rb dev", true);
        TempLabel(primary, "readback");  // TEMP perf
        vkCmdCopyBuffer(primary, mirror, (rbq ? rb_dev : rb).buf, (u32)back.size(), back.data());
        TempLabelEnd(primary);  // TEMP perf
    }
    if (rbq && rb_total) {
        vkResetCommandBuffer(rb_cmd, 0);
        vkBeginCommandBuffer(rb_cmd, &bi);
        TempLabel(rb_cmd, "readback to host");  // TEMP perf
        const VkBufferCopy c{0, 0, rb_total};
        vkCmdCopyBuffer(rb_cmd, rb_dev.buf, rb.buf, 1, &c);
        TempLabelEnd(rb_cmd);  // TEMP perf
        vkEndCommandBuffer(rb_cmd);
    }
    return rb_total;
}
} // namespace Gpu

#ifndef GOW3_OFFLINE  // the tracker needs the emulator's runtime (framecap builds without it)
// ---- Guest memory write tracking ----
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include "orbis/core/signals.h"
#endif
#include "orbis/core/memory.h"
#include "gpu/vk_gpu.h"
#include "main/memory_observer.h"
#include "gpu/shaders/shader_cache.h"

namespace Gow3::GuestMemory {
namespace {
constexpr u64 kPage = 0x1000, kPages = 1ull << 23;  // guest addresses below 32 GB, as the renderer's mirror
// Page state: low 3 bits the kind, bit 3 set when the page was PAGE_EXECUTE_READWRITE.
enum Kind : u8 { Unknown, Clean, Dirty, Dynamic, Fixed, Region };
constexpr u8 kExec = 8;
const std::unique_ptr<std::atomic<u8>[]> state(new std::atomic<u8>[kPages]{});
// Host readability per page (one bit each) as VirtualQuery reported it, dropped on every guest mapping change:
// VirtualQuery walks the page tables to the end of the region, tens of microseconds to milliseconds on the large views.
const std::unique_ptr<std::atomic<u64>[]> known(new std::atomic<u64>[kPages / 64]{}), readable(new std::atomic<u64>[kPages / 64]{});
#ifdef _WIN32
std::mutex dirty_lock;
std::vector<u64> dirty; // pages reopened by CPU writes since the last frame
#else
// Two levels let the signal handler publish without locks and keep collection proportional to dirty pages.
const std::unique_ptr<std::atomic<u64>[]> dirty_words(new std::atomic<u64>[kPages / 64]{});
const std::unique_ptr<std::atomic<u64>[]> dirty_groups(new std::atomic<u64>[kPages / 4096]{});
#endif
std::vector<u64> closed;                      // pages the last frame protected again from `dirty`, sorted (GPU thread)
struct Live { u64 page; u32 quiet; };         // quiet: frames since its bytes last changed
std::vector<Live> dynamic;                    // pages offered every frame (GPU thread)
std::atomic<u32> generation{1};               // bumped on every mapping change: a query racing it may report the old one

Kind KindOf(u64 page) { return Kind(state[page].load(std::memory_order_acquire) & 7); }
#ifdef _WIN32
using PageProtection = DWORD;
constexpr PageProtection ReadOnly = PAGE_READONLY, ExecuteRead = PAGE_EXECUTE_READ;
constexpr PageProtection ReadWrite = PAGE_READWRITE, ExecuteReadWrite = PAGE_EXECUTE_READWRITE;
bool Writable(DWORD p) { return p & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY); }
bool ProtectPages(u64 address, u64 size, PageProtection prot) {
    DWORD old;
    return VirtualProtect(reinterpret_cast<void*>(address), size, prot, &old) != 0;
}
#else
using PageProtection = int;
constexpr PageProtection ReadOnly = PROT_READ, ExecuteRead = PROT_READ | PROT_EXEC;
constexpr PageProtection ReadWrite = PROT_READ | PROT_WRITE, ExecuteReadWrite = PROT_READ | PROT_WRITE | PROT_EXEC;
struct PageRange { u64 begin, end; bool read, write, exec; };

bool QueryPage(u64 address, PageRange& r) {
    VAddr begin, end;
    int prot;
    if (!Core::Memory::Instance()->QueryHostRegion(address, begin, end, prot)) return false;
    r = {begin, end, bool(prot & (PROT_READ | PROT_WRITE)), bool(prot & PROT_WRITE), bool(prot & PROT_EXEC)};
    return true;
}

bool ProtectPages(u64 address, u64 size, PageProtection prot) {
    return mprotect(reinterpret_cast<void*>(address), size, prot) == 0;
}
#endif

// f(word, mask) over the bitmap words of pages [p0, p1), while it returns true.
bool Words(u64 p0, u64 p1, auto&& f) {
    for (u64 p = p0, end = std::min(p1, kPages); p < end;) {
        const u64 n = std::min(64 - p % 64, end - p);
        if (!f(p / 64, (n == 64 ? ~0ull : (1ull << n) - 1) << p % 64)) return false;
        p += n;
    }
    return true;
}
void Mark(std::atomic<u64>* bits, u64 p0, u64 p1, bool on) {
    Words(p0, p1, [&](u64 w, u64 m) { return (on ? bits[w].fetch_or(m) : bits[w].fetch_and(~m)), true; });
}
bool All(const std::atomic<u64>* bits, u64 p0, u64 p1) {
    return Words(p0, p1, [&](u64 w, u64 m) { return (bits[w].load(std::memory_order_acquire) & m) == m; });
}

// Protects [p0, p1) with one call per allocation piece, splitting at 16 KB guest boundaries when a span crosses allocations.
void ProtectRange(u64 p0, u64 p1, PageProtection prot) {
    if (p1 <= p0 || ProtectPages(p0 * kPage, (p1 - p0) * kPage, prot)) return;
#ifdef _WIN32
    if (p1 - p0 == 1) return;
#else
    if (p1 - p0 == 1) {
        // If the page could not be closed, stage it again on the next draw instead of expecting a write fault.
        if (!(prot & PROT_WRITE)) {
            u8 expected = Clean | (prot & PROT_EXEC ? kExec : 0);
            state[p0].compare_exchange_strong(expected, Unknown);
        }
        return;
    }
#endif
    u64 mid = ((p0 + p1) / 2) & ~3ull;
    if (mid <= p0) mid = (p0 + p1) / 2;
    ProtectRange(p0, mid, prot), ProtectRange(mid, p1, prot);
}

void QueueDirty(u64 page, u64 end) {
#ifdef _WIN32
    std::scoped_lock lock(dirty_lock);
    for (u64 p = page; p < end; ++p) dirty.push_back(p);
#else
    Words(page, end, [](u64 word, u64 mask) {
        dirty_words[word].fetch_or(mask, std::memory_order_release);
        dirty_groups[word / 64].fetch_or(u64{1} << (word % 64), std::memory_order_release);
        return true;
    });
#endif
}

// Reopens a protected page and queues it: the number of pages reopened, 0 when another thread already did. The protected pages
// right after it, up to the end of its `span`-page group (a power of two), go with it: guest writes run forward (memcpy, streams),
// and one VirtualProtect for the run replaces a fault and a call per page. The extra ones are copied at the next frame like the
// written one and protected again after it.
u64 Reopen(u64 page, u64 span = 16) {
    u8 v = state[page].load(std::memory_order_acquire);
    while ((v & 7) == Clean)
        if (state[page].compare_exchange_weak(v, u8((v & ~7) | Dirty))) {
            u64 end = page + 1;
            for (const u64 group_end = std::min((page | (span - 1)) + 1, kPages); end < group_end; ++end) {
                u8 n = state[end].load(std::memory_order_relaxed);
                if ((n & (7 | kExec)) != ((v & kExec) | Clean) || !state[end].compare_exchange_strong(n, u8((n & ~7) | Dirty))) break;
            }
            ProtectRange(page, end, v & kExec ? ExecuteReadWrite : ReadWrite);
            QueueDirty(page, end);
            return end - page;
        }
    return 0;
}

// Aliasless readbacks share the mapping with guest writers. Keep it dirty until BeginFrame samples it.
bool OpenForReadback(u64 page, bool writable) {
    u8 v = state[page].load(std::memory_order_acquire);
    if ((v & 7) == Clean) {
        u8 dirty_state = (v & kExec) | Dirty;
        if (state[page].compare_exchange_strong(v, dirty_state)) {
            if (!ProtectPages(page * kPage, kPage, v & kExec ? ExecuteReadWrite : ReadWrite)) {
                state[page].compare_exchange_strong(dirty_state, v);
                return false;
            }
            QueueDirty(page, page + 1);
            return true;
        }
        // A concurrent reopen publishes Dirty before changing the host protection.
        writable = false;
    }
    return writable || ((v & 7) == Dirty && ProtectPages(page * kPage, kPage, v & kExec ? ExecuteReadWrite : ReadWrite));
}

#ifdef _WIN32
LONG CALLBACK OnFault(EXCEPTION_POINTERS* e) {
    const auto* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    const u64 page = r->ExceptionInformation[1] / kPage;
    constexpr LONG resume = EXCEPTION_CONTINUE_EXECUTION, next_handler = EXCEPTION_CONTINUE_SEARCH;
#else
bool OnFault(void* context, void* address) {
    if (!Common::IsWriteError(context)) return false;
    const u64 page = reinterpret_cast<u64>(address) / kPage;
    constexpr bool resume = true, next_handler = false;
#endif
    if (page >= kPages) return next_handler;
    const Gow3::FaultTimer timer;
    static thread_local u64 lost_page = ~0ull;  // the page this thread last lost the reopen of, and how many times in a row
    static thread_local u32 lost = 0;
    // A fault where the last reopened run ended is a copy going forward (memcpy of megabytes): the runs double, up to 1 MB.
    static thread_local u64 next_page = ~0ull, span = 16;
    span = page == next_page ? std::min<u64>(span * 2, 256) : 16;
    if (const u64 n = Reopen(page, span)) return lost = 0, next_page = page + n, resume;
    // Another thread won the reopen and may not have changed the protection yet: run the access again, which faults again only
    // while it is still protected. VirtualQuery here walks the page tables of a view of gigabytes: milliseconds per fault.
    const Kind k = KindOf(page);
    if ((k == Dirty || k == Dynamic || k == Clean) && (lost = page == lost_page ? lost + 1 : 1, lost_page = page, lost <= 16)) {
        if (lost > 2) {
#ifdef _WIN32
            SwitchToThread();
#else
            sched_yield();
#endif
        }
        return resume;
    }
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m;
    if ((k == Dirty || k == Dynamic || k == Clean) && VirtualQuery(reinterpret_cast<void*>(page * kPage), &m, sizeof(m)) &&
        (Writable(m.Protect) || m.Protect == PAGE_READONLY || m.Protect == PAGE_EXECUTE_READ))
        return resume;
#else
    if ((k == Dirty || k == Dynamic || k == Clean) && lost <= 1024) {
        const timespec ts{0, 10'000};
        nanosleep(&ts, nullptr);
        return resume;
    }
#endif
    return next_handler;
}

// Reopen may have made a page writable just before our protection change landed.
void CloseCleanPages(u64 first, u64 end, bool exec) {
    ProtectRange(first, end, exec ? ExecuteRead : ReadOnly);
    for (u64 p = first; p < end; ++p)
        if (KindOf(p) == Dirty) ProtectRange(p, p + 1, exec ? ExecuteReadWrite : ReadWrite);
}

// Encoded as page << 1 | executable, sorted.
void Protect(const std::vector<u64>& pages) {
    for (size_t i = 0, j; i < pages.size(); i = j) {
        for (j = i + 1; j < pages.size() && pages[j] == pages[j - 1] + 2; ++j) {}
        const u64 first = pages[i] >> 1;
        CloseCleanPages(first, first + (j - i), pages[i] & 1);
    }
}

// A guest mapping or protection change: the pages are tracked and queried again.
void Forget(u64 addr, u64 size) {
    const u64 p0 = addr / kPage, p1 = std::min((addr + size + kPage - 1) / kPage, kPages);
    for (u64 p = p0; p < p1; ++p)
        if (KindOf(p) != Region) state[p].store(Unknown, std::memory_order_release);
    Mark(known.get(), p0, p1, false);
    generation.fetch_add(1, std::memory_order_release);
}

// Guest mappings and host writes into guest memory (file reads), from the memory manager.
struct Observer final : MemoryObserver {
    void RegisterMemory(VAddr address, u64 size) override { Forget(address, size); }
    void MapMemory(VAddr address, u64 size) override { Forget(address, size), Gpu::ShaderCache::Instance().Streamed(size); }
    void UnmapMemory(VAddr address, u64 size) override { Forget(address, size); }
    void InvalidateMemory(VAddr address, u64 size, bool written) override {
        if (!written) return Forget(address, size);  // guest protection change: tracked again, with its protection
        Gpu::ShaderCache::Instance().Streamed(size);  // file reads into guest memory: what a loading screen is made of
        for (u64 p = address / kPage, end = std::min((address + size + kPage - 1) / kPage, kPages); p < end; ++p) Reopen(p);
    }
} observer;
} // namespace

std::atomic<u64> temp_queries{0};  // debug: VirtualQuery calls of Readable and Track

// ---- Public tracking API ----

void Install() {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, OnFault);
#else
    Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 1);
#endif
    Core::Memory::Instance()->SetMemoryObserver(&observer);
}

void SetRegions(const std::vector<Range>& regions) {
    for (auto [lo, hi] : regions)
        for (u64 p = lo / kPage, end = std::min((hi + kPage - 1) / kPage, kPages); p < end; ++p) {
            u8 v = state[p].load();
            if ((v & 7) == Clean) {  // tracked before the regions were known: reopen without queueing
#ifdef _WIN32
                DWORD old;
                VirtualProtect(reinterpret_cast<void*>(p * kPage), kPage, v & kExec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old);
#else
                ProtectPages(p * kPage, kPage, v & kExec ? ExecuteReadWrite : ReadWrite);
#endif
            }
            state[p].store(Region);
        }
}

u32 BeginFrame(Gpu::Renderer& renderer, const std::vector<Range>& offer) {
    std::vector<u64> pages;
#ifdef _WIN32
    {
        std::scoped_lock lock(dirty_lock);
        pages.swap(dirty);
    }
    std::sort(pages.begin(), pages.end());
#else
    for (u64 i = 0; i < kPages / 4096; ++i) {
        for (u64 group = dirty_groups[i].exchange(0, std::memory_order_acquire); group; group &= group - 1) {
            const u64 word = i * 64 + std::countr_zero(group);
            for (u64 bits = dirty_words[word].exchange(0, std::memory_order_acquire); bits; bits &= bits - 1)
                pages.push_back(word * 64 + std::countr_zero(bits));
        }
    }
#endif
    // Everything this frame offers goes to the renderer in one batch (hashed in parallel), each page after its protection.
    static std::vector<Gpu::Renderer::WriteRange> writes;
    static std::vector<u64> now, protect;  // pages protected again: from `dirty` (the next `closed`); all of them, for Protect
    writes.clear(), now.clear(), protect.clear();
    auto write = [&](u64 a, const void* p, u64 n) { writes.push_back({a, p, n}); };
    for (u64 page : pages) {
        u8 v = state[page].load();
        if ((v & 7) != Dirty) continue;
        if (std::binary_search(closed.begin(), closed.end(), page)) {  // written in two consecutive frames: offered every frame from now on
            state[page].store(u8((v & ~7) | Dynamic));
            dynamic.push_back({page, 0});
        } else {
            // Protected before the copy: a write after it faults again and is seen next frame.
            state[page].store(u8((v & ~7) | Clean));
            now.push_back(page), protect.push_back(page << 1 | (v & kExec ? 1 : 0));
        }
        write(page * kPage, reinterpret_cast<const void*>(page * kPage), kPage);
    }
    closed.swap(now);
    // A dynamic page unchanged for 64 frames is protected again (before its copy, as above) and tracked by its faults.
    size_t kept = 0;
    for (Live d : dynamic) {
        u8 v = state[d.page].load();
        if ((v & 7) != Dynamic) continue;
        const u64 a = d.page * kPage;
        if (d.quiet >= 64 && state[d.page].compare_exchange_strong(v, u8((v & ~7) | Clean))) {
            protect.push_back(d.page << 1 | (v & kExec ? 1 : 0));
            write(a, reinterpret_cast<const void*>(a), kPage);
            continue;
        }
        dynamic[kept++] = d;
    }
    dynamic.resize(kept);
    std::sort(protect.begin(), protect.end());
    Protect(protect);
    const size_t live = writes.size();  // the dynamic pages kept, in order
    for (Live d : dynamic) write(d.page * kPage, reinterpret_cast<const void*>(d.page * kPage), kPage);
    for (auto [lo, hi] : offer)
        if (hi > lo && Readable(lo, hi - lo)) writes.push_back({lo, reinterpret_cast<const void*>(lo), hi - lo, false, true});
    renderer.Write(writes);
    for (size_t i = 0; i < dynamic.size(); ++i) dynamic[i].quiet = writes[live + i].changed ? 0 : dynamic[i].quiet + 1;
    return u32(pages.size());
}

bool Track(Gpu::Renderer& renderer, u64 lo, u64 hi) {
    lo &= ~(kPage - 1), hi = std::min((hi + kPage - 1) & ~(kPage - 1), kPages * kPage);
    // Recording threads call this per draw: ranges without untracked pages return without the lock.
    while (lo < hi && KindOf(lo / kPage) != Unknown) lo += kPage;
    if (lo >= hi) return false;
    static std::mutex track_lock;
    std::scoped_lock lock(track_lock);
    bool staged = false;
    for (u64 a = lo; a < hi;) {
        if (KindOf(a / kPage) != Unknown) {
            a += kPage;
            continue;
        }
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION m;
#else
        PageRange range;
#endif
        GOW3_DIAG temp_queries.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m))) break;
        const u64 region_end = std::min(hi, u64(m.BaseAddress) + m.RegionSize);
        if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !m.Protect) {
#else
        if (!QueryPage(a, range)) break;
        const u64 region_end = std::min(hi, range.end);
        if (!range.read) {
#endif
            a = region_end;
            continue;
        }
        u64 end = a;
        while (end < region_end && KindOf(end / kPage) == Unknown) end += kPage;
#ifdef _WIN32
        const DWORD prot = m.Protect & 0xFF;
        const bool writable = Writable(prot), exec = prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
#else
        const bool writable = range.write, exec = range.exec;
#endif
        for (u64 p = a / kPage; p < end / kPage; ++p) state[p].store(u8((writable ? Clean : Fixed) | (exec ? kExec : 0)));
        if (writable) CloseCleanPages(a / kPage, end / kPage, exec);
        renderer.Write(a, reinterpret_cast<const void*>(a), end - a);
        staged = true;
        a = end;
    }
    return staged;
}

u64 Store(u64 addr, const void* data, u64 size) {
    // Through the host's alias of the physical memory (16 KB pages), as the console's GPU writes: no query, no protection
    // change. Memory without one (file mappings) is written through the guest mapping when writable or ours.
    constexpr u64 kBacking = 0x4000;
    const auto& backing = Core::Memory::Instance()->GetAddressSpace().BackingPages();
    const u8* src = static_cast<const u8*>(data);
    const bool single = size <= kPage - (addr & (kPage - 1));
    u64 hash = 0;
    for (u64 o = 0; o < size;) {
        const u64 a = addr + o, page = a / kPage, n = std::min(kPage - (a & (kPage - 1)), size - o);
        auto write_piece = [&](void* dst) {
            if (single) hash = Gpu::PieceHash::Copy(dst, src + o, n);
            else std::memcpy(dst, src + o, n);
        };
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION m;
#else
        PageRange range;
#endif
        if (const auto* e = a >> 40 ? nullptr : backing.find(a / kBacking); e && *e) {
            write_piece(*e + (a & (kBacking - 1)));
#ifdef _WIN32
        } else if (page < kPages && VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m)) && m.State == MEM_COMMIT &&
                   OpenForReadback(page, Writable(m.Protect))) {
#else
        } else if (page < kPages && QueryPage(a, range) && range.write && OpenForReadback(page, true)) {
#endif
            write_piece(reinterpret_cast<void*>(a));
        }
        o += n;
    }
    return hash ? hash : Gpu::PieceHash::Hash(data, size);
}

char PageKind(u64 addr) { return addr / kPage < kPages ? "UCDYFR"[KindOf(addr / kPage)] : '?'; }

bool Readable(u64 addr, u64 size) {
    if (!size) return true;
    const u64 p0 = addr / kPage, p1 = (addr + size + kPage - 1) / kPage;
    if (p1 <= kPages && All(known.get(), p0, p1)) return All(readable.get(), p0, p1);
    // Each region queried is learned whole.
    const u32 gen = generation.load(std::memory_order_acquire);
    u64 lo = 0, a = addr;
    bool ok = true;
    while (ok && a < addr + size) {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION m;
#else
        PageRange range;
#endif
        GOW3_DIAG temp_queries.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof(m))) return false;
        const u64 base = u64(m.BaseAddress), end = base + m.RegionSize;
        ok = m.State == MEM_COMMIT && m.Protect && !(m.Protect & (PAGE_NOACCESS | PAGE_GUARD));
#else
        if (!QueryPage(a, range)) return false;
        const u64 base = range.begin, end = range.end;
        ok = range.read;
#endif
        Mark(readable.get(), base / kPage, end / kPage, ok), Mark(known.get(), base / kPage, end / kPage, true);
        lo = a == addr ? base : lo, a = end;
    }
    if (generation.load(std::memory_order_acquire) != gen) Mark(known.get(), lo / kPage, a / kPage, false);  // may be gone
    return ok;
}
} // namespace Gow3::GuestMemory
#endif
