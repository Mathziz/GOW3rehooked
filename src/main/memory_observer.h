// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "orbis/common/types.h"

namespace Gow3 {
class MemoryObserver {
public:
    virtual ~MemoryObserver() = default;
    virtual void RegisterMemory(VAddr address, u64 size) = 0;
    virtual void MapMemory(VAddr address, u64 size) = 0;
    virtual void UnmapMemory(VAddr address, u64 size) = 0;
    // `written`: the CPU stored new data in the range (false for protection changes).
    virtual void InvalidateMemory(VAddr address, u64 size, bool written) = 0;
};
}
