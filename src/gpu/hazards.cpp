// SPDX-License-Identifier: GPL-2.0-or-later
// Resource hazards: full and attachment barriers, image layout transitions and the end of a rendering pass.
#include "gpu/hazards.h"
#include "gpu/renderer_impl.h"

namespace Gpu {
namespace {
void SubmitMemoryBarrier(VkCommandBuffer c, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                         VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkDependencyFlags flags = 0) {
    const VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, src_stage, src_access, dst_stage, dst_access};
    const VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO, nullptr, flags, 1, &mb};
    vkCmdPipelineBarrier2(c, &di);
}
} // namespace

// ---- Pipeline barriers ----

// Everything recorded so far before what follows. `frag`: before its fragment work, compute and transfers only (the
// vertex stages may start earlier): for image hazards (Hazards tracks the vertex shaders' samples). Shaders write only
// images (buffer stores are not translated): the mirror changes through write-back copies alone.
void Renderer::Impl::Barrier(VkCommandBuffer c, bool frag, std::source_location temp_at) {
    GOW3_DIAG temp_bars[0].fetch_add(1, std::memory_order_relaxed);
    if (temp_barsites) {
        std::scoped_lock l(temp_site_lock);
        ++temp_sites[temp_at.line() + (frag ? 100000 : 0)];
    }
    if (block_now) block_now->started = true;  // not a pass the block before may continue
    if (c == cmd) {  // everything this command buffer did so far is ordered
        if (!hz.fenced) hz.fenced = true, hz.in_w = hz.w;
        if (!frag) hz.wide = true, hz.wv.clear();
        else
            for (VkImage x : hz.w) Add(hz.wv, x);
        hz.w.clear(), hz.r.clear();
    }
    constexpr VkPipelineStageFlags2 kFragStages =
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    constexpr VkPipelineStageFlags2 kAllSrcStages = kFragStages | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    constexpr VkPipelineStageFlags2 kAllDstStages =
        kAllSrcStages | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT;
    constexpr VkAccessFlags2 kSrcAccess =
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    constexpr VkAccessFlags2 kFragDstAccess =
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    constexpr VkAccessFlags2 kAllDstAccess =
        kFragDstAccess | VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT;
    SubmitMemoryBarrier(c, frag ? kFragStages : kAllSrcStages, kSrcAccess, frag ? kFragStages : kAllDstStages,
                        frag ? kFragDstAccess : kAllDstAccess);
}

void Renderer::Impl::StageBarrier(VkCommandBuffer c, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    GOW3_DIAG temp_bars[0].fetch_add(1, std::memory_order_relaxed);
    SubmitMemoryBarrier(c, src_stage, src_access, dst_stage, dst_access);
}

// Between two parts of one pass recorded apart: attachment writes before attachment accesses, as within the pass (no
// `flags`: between passes that share an attachment; `war`: also orders prior shader reads before attachment writes).
void Renderer::Impl::PassBarrier(VkCommandBuffer c, VkDependencyFlags flags, bool war) {
    GOW3_DIAG temp_bars[1].fetch_add(1, std::memory_order_relaxed);
    constexpr VkPipelineStageFlags2 s = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    const VkPipelineStageFlags2 src = war ? s | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : s;
    SubmitMemoryBarrier(c, src, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, s,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        war ? 0 : flags);
}

// ---- Image layouts and pass transitions ----

void Renderer::Impl::ToGeneral(VkCommandBuffer c, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img, b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(c, &di);
}

// Ends the open pass; `barrier`: and orders everything this command buffer did so far before what follows (lazy: also
// with no pass open, when ended passes are pending; with nothing of its own pending, the block asks Submit for it).
void Renderer::Impl::EndRendering(bool barrier) {
    const bool was = rendering;
    if (rendering) {
        EndQuery();  // begun inside the pass
        vkCmdEndRendering(cmd);
        TempLabelEnd(cmd);
        if (prof && prof_pass != ~0u)
            ProfEnd(prof_pass), prof_label[prof_pass / 2] += std::format(" draws {}", stats.draws - prof_draws), prof_pass = ~0u;
        rendering = false;
        for (VkImage i : pass_w) Add(hz.w, i);
        pass_w.clear();
    }
    if (!barrier) return;
    if (was || !hz.w.empty() || !hz.r.empty()) {
        Barrier(cmd, true);  // dispatches, copies and passes follow, the first two behind a full barrier
    } else if (!hz.fenced) {
        hz.all = true;
    }
}
} // namespace Gpu
