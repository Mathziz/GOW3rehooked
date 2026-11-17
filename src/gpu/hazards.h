// SPDX-License-Identifier: GPL-2.0-or-later
// Image hazards of a recording thread's command buffer (lazy pass ends); the barriers themselves are in hazards.cpp.
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace Gpu {
// Lazy pass ends: a pass ends without a barrier unless what follows depends on it. Render targets this thread's command
// buffer wrote (attachments, storage images of ended passes) and sampled since its last full barrier, and the same
// before its first one (Submit orders the blocks by them; `all`: something there needed every earlier write done).
// Guest textures need none of it: only uploads and write-backs write them, behind full barriers. Barriers for fragment
// work leave the vertex stages out: targets written before one (`wv`, since the last barrier that did not) and those a
// vertex shader sampled before the first barrier that did not (`vs_in`, `wide` once there was one) order the rest.
struct Hazards {
    std::vector<VkImage> w, r, in_w, in_r, wv, vs_in;
    bool fenced = false, all = false, wide = false;
    void Reset() { w.clear(), r.clear(), in_w.clear(), in_r.clear(), wv.clear(), vs_in.clear(), fenced = all = wide = false; }
};
} // namespace Gpu
