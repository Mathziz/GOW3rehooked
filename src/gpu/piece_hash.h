// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#ifdef _WIN32
#include <intrin.h>
#endif
#include <xxhash.h>

namespace Gpu::PieceHash {
// rapidhash v3, Copyright (c) 2025 Nicolas De Carli; MIT License.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions: the above copyright notice and
// this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
// "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
namespace detail {
using u64 = std::uint64_t;
inline constexpr u64 secret[]{0x2d358dccaa6c78a5ull, 0x8bb84b93962eacc9ull, 0x4b33a62ed433d4a3ull, 0x4d5a2da51de1aa47ull,
                              0xa0761d6478bd642full, 0xe7037ed1a0b428dbull, 0x90ed1765281c388cull, 0xaaaaaaaaaaaaaaaaull};
static inline u64 Read64(const void* p) { u64 v; std::memcpy(&v, p, 8); return v; }
static inline u64 Read32(const void* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }
#ifdef _WIN32
static inline u64 Mix(u64 a, u64 b) { u64 hi; u64 lo = _umul128(a, b, &hi); return lo ^ hi; }
#else
static inline u64 Mix(u64 a, u64 b) { const unsigned __int128 p = static_cast<unsigned __int128>(a) * b; return u64(p) ^ u64(p >> 64); }
#endif
static inline u64 Rapid(const void* data, std::size_t size, u64 seed = 0) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    seed ^= Mix(seed ^ secret[2], secret[1]);
    u64 a = 0, b = 0; std::size_t i = size;
    if (size <= 16) {
        if (size >= 4) {
            seed ^= size;
            if (size >= 8) a = Read64(p), b = Read64(p + size - 8);
            else a = Read32(p), b = Read32(p + size - 4);
        } else if (size) a = u64(p[0]) << 45 | p[size - 1], b = p[size >> 1];
    } else {
        if (i > 112) {
            u64 s1 = seed, s2 = seed, s3 = seed, s4 = seed, s5 = seed, s6 = seed;
            auto block = [&] {
                seed = Mix(Read64(p) ^ secret[0], Read64(p + 8) ^ seed);
                s1 = Mix(Read64(p + 16) ^ secret[1], Read64(p + 24) ^ s1);
                s2 = Mix(Read64(p + 32) ^ secret[2], Read64(p + 40) ^ s2);
                s3 = Mix(Read64(p + 48) ^ secret[3], Read64(p + 56) ^ s3);
                s4 = Mix(Read64(p + 64) ^ secret[4], Read64(p + 72) ^ s4);
                s5 = Mix(Read64(p + 80) ^ secret[5], Read64(p + 88) ^ s5);
                s6 = Mix(Read64(p + 96) ^ secret[6], Read64(p + 104) ^ s6);
                p += 112; i -= 112;
            };
            while (i > 224) { block(); block(); }
            if (i > 112) block();
            seed ^= s1 ^ s2 ^ s3 ^ s4 ^ s5 ^ s6;
        }
        if (i > 16) {
            seed = Mix(Read64(p) ^ secret[2], Read64(p + 8) ^ seed);
            if (i > 32) {
                seed = Mix(Read64(p + 16) ^ secret[2], Read64(p + 24) ^ seed);
                if (i > 48) {
                    seed = Mix(Read64(p + 32) ^ secret[1], Read64(p + 40) ^ seed);
                    if (i > 64) {
                        seed = Mix(Read64(p + 48) ^ secret[1], Read64(p + 56) ^ seed);
                        if (i > 80) {
                            seed = Mix(Read64(p + 64) ^ secret[2], Read64(p + 72) ^ seed);
                            if (i > 96) seed = Mix(Read64(p + 80) ^ secret[1], Read64(p + 88) ^ seed);
                        }
                    }
                }
            }
        }
        a = Read64(p + i - 16) ^ i; b = Read64(p + i - 8);
    }
    a ^= secret[1]; b ^= seed;
#ifdef _WIN32
    u64 hi; a = _umul128(a, b, &hi); b = hi;
#else
    const unsigned __int128 prod = static_cast<unsigned __int128>(a) * b;
    a = u64(prod); b = u64(prod >> 64);
#endif
    return Mix(a ^ secret[7], b ^ secret[1] ^ i);
}
__attribute__((target("aes"))) static inline u64 AesShort(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i y;
    if (n == 24) y = _mm_set_epi64x(n, Read64(p + 16));
    else y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16));
    const auto k = _mm_set_epi64x(0x9e3779b97f4a7c15ull, n ^ 0x243f6a8885a308d3ull);
    auto h = _mm_aesenc_si128(_mm_xor_si128(x, k), y);
    h = _mm_aesenc_si128(h, k); h = _mm_aesenc_si128(h, k);
    return u64(_mm_cvtsi128_si64(h)) ^ u64(_mm_extract_epi64(h, 1));
}
} // namespace detail
// ---- Stable hashes (format-invariant across CPUs) ----
static inline std::uint64_t Stable64(const void* data, std::size_t size) { return XXH3_64bits(data, size); }
static inline std::uint64_t Stable64Seeded(const void* data, std::size_t size, std::uint64_t seed) { return XXH3_64bits_withSeed(data, size, seed); }
static inline XXH128_hash_t Stable128(const void* data, std::size_t size) { return XXH3_128bits(data, size); }

// ---- Fast memory hashing and hash-while-copying (AES-NI when supported, XXH3 fallback) ----
inline constexpr std::size_t kMemoryBlock = 512;

bool HasAes();
bool UsesAes();
std::uint64_t Hash(const void* data, std::size_t size);
std::uint64_t Copy(void* dst, const void* data, std::size_t size);
// Only the `want` blocks (a bit each) are copied and hashed; the root covers every entry of `hashes`, the others as given.
std::uint64_t CopyBlocks(void* dst, const void* data, std::size_t size, std::uint64_t* hashes, unsigned want = ~0u);

// ---- Domain key helpers ----
// Shader and disk cache hashes keep their existing format.
std::uint64_t ShaderCode(const void* p, std::size_t n);
std::uint64_t ShaderContent(const void* p, std::size_t n, std::uint64_t seed);
static inline std::uint64_t ShaderVariant(const void* p, std::size_t n) { return detail::Rapid(p, n); }
static inline std::uint64_t ShaderKey(const void* p, std::size_t n, std::uint64_t seed) { return detail::Rapid(p, n, seed); }
static inline std::uint64_t ShaderSpecKey(const void* p, std::size_t n, std::uint64_t seed) { return Stable64Seeded(p, n, seed); }
static inline XXH128_hash_t ShaderContract(const void* p, std::size_t n) { return Stable128(p, n); }
static inline std::uint64_t WadName(const void* p, std::size_t n) { return Stable64(p, n); }

static inline std::uint64_t TextureKey(const void* p, std::size_t n) { return UsesAes() && n == 32 ? detail::AesShort(p, n) : Stable64(p, n); }
static inline std::uint64_t SamplerKey(const void* p, std::size_t n) { return detail::Rapid(p, n); }
static inline std::uint64_t TargetKey(const void* p, std::size_t n) { return UsesAes() && n == 24 ? detail::AesShort(p, n) : Stable64(p, n); }
static inline std::uint64_t TrackingKey(const void* p, std::size_t n, std::uint64_t seed) { return detail::Rapid(p, n, seed); }
static inline std::uint64_t DriverKey(const void* p, std::size_t n) { return Stable64(p, n); }
static inline std::uint64_t DriverBinaryKey(const void* p, std::size_t n, std::uint64_t seed) { return detail::Rapid(p, n, seed); }
static inline std::uint64_t DebugHash(const void* p, std::size_t n) { return Hash(p, n); }

} // namespace Gpu::PieceHash
