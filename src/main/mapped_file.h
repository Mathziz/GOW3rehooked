// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Gow3 {
enum class FileAccess { Default, Sequential, Random };

inline void UnmapReadOnly(const std::uint8_t* data, std::size_t size) {
    if (data) munmap(const_cast<std::uint8_t*>(data), size);
}

inline bool MapReadOnly(const std::filesystem::path& path, const std::uint8_t*& data, std::size_t& size,
                        FileAccess access = FileAccess::Default) {
    UnmapReadOnly(data, size);
    data = nullptr, size = 0;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        const auto length = std::size_t(st.st_size);
        if (void* view = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0); view != MAP_FAILED) {
            data = static_cast<const std::uint8_t*>(view), size = length;
            if (access != FileAccess::Default)
                madvise(view, size, access == FileAccess::Sequential ? MADV_SEQUENTIAL : MADV_RANDOM);
        }
    }
    close(fd);
    return data != nullptr;
}
} // namespace Gow3
