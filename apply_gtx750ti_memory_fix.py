#!/usr/bin/env python3
"""GOW3rehooked: experimental low-VRAM guest mirror allocation patch.

Usage:
    python apply_gtx750ti_memory_fix.py C:\\path\\to\\GOW3rehooked

Patches only three renderer source files. Verifies every replacement before
writing and keeps complete backups. Local Vulkan feature-negotiation changes
outside these blocks are preserved. Does NOT emulate sparseResidencyBuffer.
"""
from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path
import shutil
import sys

PATCHES = {
    "src/gpu/renderer_impl.h": [
        (
            "    // Integrated GPU with VK_EXT_memory_budget: its device-local heap is a carve-out of the same RAM (1 GiB by default on the\n"
            "    // Steam Deck), so DeviceType takes the largest heap once that budget is spent.\n"
            "    bool uma_budget = false;",
            "    // VK_EXT_memory_budget is useful on both integrated and discrete GPUs. When the local heap is at its\n"
            "    // budget, guest mirror chunks may live in a compatible host-visible heap instead of exhausting VRAM.\n"
            "    bool memory_budget = false;",
        ),
        (
            "    u64 chunk_used = kChunk;",
            "    u64 chunk_capacity = kChunk, chunk_used = kChunk;  // smaller allocations on low-VRAM GPUs",
        ),
    ],
    "src/gpu/device.cpp": [
        (
            "    uma_budget = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && HasExtension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);",
            "    memory_budget = HasExtension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);",
        ),
        (
            "mirror_bits, uma_budget,",
            "mirror_bits, memory_budget,",
        ),
        (
            "integrated with budget {}",
            "memory budget available {}",
        ),
    ],
    "src/gpu/buffers.cpp": [
        (
            "// Device-local, unless the GPU is integrated and that heap's budget is spent: then the largest heap with room, the same RAM\n"
            "// (no PCIe in between). Write-combined over host-cached, which the GPU snoops.",
            "// Device-local while its budget permits; on a low-VRAM discrete GPU, move compatible allocations\n"
            "// into a host-visible heap when necessary (slower over PCIe, but avoids immediate VRAM exhaustion).",
        ),
        (
            "    if (!uma_budget || local == ~0u) return local;",
            "    if (!memory_budget || local == ~0u) return local;",
        ),
        (
            "    // An eighth of the heap stays free for the driver and the presenter.\n"
            "    const auto room = [&](u32 h) { return budget.heapUsage[h] + size + mem.memoryHeaps[h].size / 8 <= budget.heapBudget[h]; };",
            "    // Leave headroom for the driver, render targets and presentation, without overflowing arithmetic.\n"
            "    const auto room = [&](u32 h) {\n"
            "        const VkDeviceSize reserve = std::min<VkDeviceSize>(mem.memoryHeaps[h].size / 8, 256ull << 20);\n"
            "        return budget.heapUsage[h] <= budget.heapBudget[h] &&\n"
            "               size <= budget.heapBudget[h] - budget.heapUsage[h] &&\n"
            "               reserve <= budget.heapBudget[h] - budget.heapUsage[h] - size;\n"
            "    };",
        ),
        (
            "        if (bits >> i & 1 && t.heapIndex != heap && !(t.propertyFlags & kAvoid) && room(t.heapIndex) &&",
            "        if (bits >> i & 1 && t.heapIndex != heap && (t.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&\n"
            "            !(t.propertyFlags & kAvoid) && room(t.heapIndex) &&",
        ),
        (
            "    page = mr.alignment, mirror_bits = mr.memoryTypeBits;\n"
            "    bound.assign(mirror_size / page / 64 + 1, 0);",
            "    page = mr.alignment, mirror_bits = mr.memoryTypeBits;\n"
            "    if (const u32 local = Type(mirror_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); local != ~0u &&\n"
            "        mem.memoryHeaps[mem.memoryTypes[local].heapIndex].size <= (3ull << 30)) {\n"
            "        chunk_capacity = std::max<u64>(page, 64ull << 20);\n"
            "        chunk_used = chunk_capacity;\n"
            "        Gow3::Log(std::format(\"Renderer: low-VRAM GPU; using {} MiB guest mirror chunks\", chunk_capacity >> 20));\n"
            "    }\n"
            "    bound.assign(mirror_size / page / 64 + 1, 0);",
        ),
        (
            "        if (chunk_used + page > kChunk) {\n"
            "            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};\n"
            "            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, kChunk, DeviceType(mirror_bits, kChunk)};\n"
            "            VkDeviceMemory m = VK_NULL_HANDLE;\n"
            "            if (const VkResult e = vkAllocateMemory(dev, &ai, nullptr, &m); e != VK_SUCCESS) {\n"
            "                word &= ~bit;  // the rest stays without memory (reads zero); the next write there tries again\n"
            "                if (!chunk_failed)\n"
            "                    chunk_failed = true, Gow3::Log(std::format(\"Renderer: vkAllocateMemory {} for the guest mirror (type {}, {} MB held)\", (int)e,\n"
            "                                                               (int)ai.memoryTypeIndex, mirror_chunks.load() >> 20));\n"
            "                break;\n"
            "            }\n"
            "            chunks.push_back(m), chunk_used = 0, mirror_chunks += kChunk;\n"
            "        }",
            "        if (chunk_used + page > chunk_capacity) {\n"
            "            VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};\n"
            "            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags, chunk_capacity, DeviceType(mirror_bits, chunk_capacity)};\n"
            "            VkDeviceMemory m = VK_NULL_HANDLE;\n"
            "            VkResult e = vkAllocateMemory(dev, &ai, nullptr, &m);\n"
            "            // Memory budget data may lag behind the driver's actual allocations. On VRAM exhaustion,\n"
            "            // retry a compatible host-visible memory type on a different heap.\n"
            "            if (e == VK_ERROR_OUT_OF_DEVICE_MEMORY || e == VK_ERROR_OUT_OF_HOST_MEMORY) {\n"
            "                const u32 previous_type = ai.memoryTypeIndex;\n"
            "                const u32 previous_heap = previous_type < mem.memoryTypeCount ? mem.memoryTypes[previous_type].heapIndex : ~0u;\n"
            "                for (u32 type = 0; type < mem.memoryTypeCount; ++type) {\n"
            "                    if (!(mirror_bits >> type & 1) || type == previous_type ||\n"
            "                        mem.memoryTypes[type].heapIndex == previous_heap ||\n"
            "                        !(mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;\n"
            "                    ai.memoryTypeIndex = type;\n"
            "                    e = vkAllocateMemory(dev, &ai, nullptr, &m);\n"
            "                    if (e == VK_SUCCESS) {\n"
            "                        if (!spilled.exchange(true))\n"
            "                            Gow3::Log(std::format(\"Renderer: guest mirror allocation moved to system memory type {} after VRAM allocation failure\", type));\n"
            "                        break;\n"
            "                    }\n"
            "                }\n"
            "            }\n"
            "            if (e != VK_SUCCESS) {\n"
            "                word &= ~bit;  // report below; page remains unbound\n"
            "                if (!chunk_failed)\n"
            "                    chunk_failed = true, Gow3::Log(std::format(\"Renderer: vkAllocateMemory {} for the guest mirror (last type {}, {} MB held)\", (int)e,\n"
            "                                                               (int)ai.memoryTypeIndex, mirror_chunks.load() >> 20));\n"
            "                break;\n"
            "            }\n"
            "            chunks.push_back(m), chunk_used = 0, mirror_chunks += chunk_capacity;\n"
            "        }",
        ),
    ],
}


def apply_changes(root: Path) -> None:
    if not (root / ".git").exists():
        raise RuntimeError(f"Nao encontrei .git em {root}; use a pasta raiz do repositorio (nao a pasta do jogo)")

    planned: dict[Path, tuple[bytes, bytes, str]] = {}
    for relpath, edits in PATCHES.items():
        dest = root / relpath
        original_bytes = dest.read_bytes()
        newline = "\r\n" if b"\r\n" in original_bytes else "\n"
        old = original_bytes.decode("utf-8").replace("\r\n", "\n")
        updated = old
        for needle, replacement in edits:
            n = updated.count(needle)
            if n != 1:
                if n == 0 and replacement in updated:
                    raise RuntimeError(f"{relpath}: parte do patch ja foi aplicada. Nada foi alterado.")
                raise RuntimeError(f"{relpath}: trecho original mudou (ocorrencias={n}). Nada foi alterado. Compare com o codigo do GitHub.")
            updated = updated.replace(needle, replacement, 1)
        if newline == "\r\n":
            updated = updated.replace("\n", "\r\n")
        planned[dest] = (original_bytes, updated.encode("utf-8"), relpath)

    backup = root / ("gtx750ti-backup-" + datetime.now().strftime("%Y%m%d-%H%M%S"))
    if backup.exists():
        raise RuntimeError("Ja existe uma pasta de backup com esse horario; tente de novo.")
    for dest, (original_bytes, _, relpath) in planned.items():
        target = backup / relpath
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(original_bytes)
    for dest, (_, modified_bytes, relpath) in planned.items():
        dest.write_bytes(modified_bytes)
        print(f"Alterado: {relpath}")
    print(f"Backup: {backup}")
    print("Patch de alocacao aplicado. Recompile sua versao local e teste em janela a 720p/30fps.")
    print("ATENCAO: isto nao implementa sparseResidencyBuffer e nao garante corrigir VK_ERROR_DEVICE_LOST.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Patch experimental do guest mirror para GPUs com pouca VRAM")
    parser.add_argument("repository", nargs="?", type=Path, default=Path.cwd(), help="Pasta raiz do repositorio GOW3rehooked")
    args = parser.parse_args()
    try:
        apply_changes(args.repository.resolve())
    except (RuntimeError, OSError, UnicodeError) as error:
        print(f"Erro: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
