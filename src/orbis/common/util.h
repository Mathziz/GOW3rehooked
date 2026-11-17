// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <intrin.h>

#include "orbis/common/types.h"

namespace Common {

template <typename T>
[[nodiscard]] constexpr T AlignUp(T value, std::size_t size) {
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned value.");
    auto mod{static_cast<T>(value % size)};
    value -= mod;
    return static_cast<T>(mod == T{0} ? value : value + size);
}

template <typename T>
[[nodiscard]] constexpr T AlignDown(T value, std::size_t size) {
    static_assert(std::is_unsigned_v<T>, "T must be an unsigned value.");
    return static_cast<T>(value - value % size);
}

template <typename T>
[[nodiscard]] constexpr T AlignUpPow2(T value, std::size_t size) {
    return static_cast<T>(value + (size - 1) & ~(size - 1));
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Is16KBAligned(T value) {
    return (value & 0x3FFF) == 0;
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Is64KBAligned(T value) {
    return (value & 0xFFFF) == 0;
}

template <typename T>
    requires std::is_integral_v<T>
[[nodiscard]] constexpr bool Is2MBAligned(T value) {
    return (value & 0x1FFFFF) == 0;
}

template <typename N, typename D>
    requires std::is_integral_v<N> && std::is_unsigned_v<D>
[[nodiscard]] constexpr N DivCeil(N number, D divisor) {
    return static_cast<N>((static_cast<D>(number) + divisor - 1) / divisor);
}

template <typename T>
concept IsContiguousContainer = std::contiguous_iterator<typename T::iterator>;

template <class Func>
struct FuncTraits {};

template <class ReturnType_, class... Args>
struct FuncTraits<ReturnType_ (*)(Args...)> {
    using ReturnType = ReturnType_;
    static constexpr size_t NUM_ARGS = sizeof...(Args);
    template <size_t I>
    using ArgType = std::tuple_element_t<I, std::tuple<Args...>>;
};

template <typename Func>
struct LambdaTraits : LambdaTraits<decltype(&std::remove_reference_t<Func>::operator())> {};

template <typename ReturnType, typename LambdaType, typename... Args>
struct LambdaTraits<ReturnType (LambdaType::*)(Args...) const> {
    template <size_t I>
    using ArgType = std::tuple_element_t<I, std::tuple<Args...>>;
    static constexpr size_t NUM_ARGS{sizeof...(Args)};
};

template <class T>
class Singleton {
public:
    static T* Instance() {
        if (!m_instance) [[unlikely]] Create();
        return m_instance.get();
    }

protected:
    Singleton();
    ~Singleton();

private:
    // Out of line: the constructor call, its unwind state and the frame it needs stay off every caller's common path.
    [[clang::noinline]] static void Create() { m_instance = std::make_unique<T>(); }
    static inline std::unique_ptr<T> m_instance{};
};

/// Spin-waiting mutex for short critical sections.
class SpinLock {
public:
    void lock() {
        while (lck.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }
    }
    void unlock() {
        lck.clear(std::memory_order_release);
    }
    [[nodiscard]] bool try_lock() {
        return !lck.test_and_set(std::memory_order_acquire);
    }

private:
    std::atomic_flag lck = ATOMIC_FLAG_INIT;
};

__forceinline u64 FencedRDTSC() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
}

/// a * b / d without overflowing the intermediate product.
[[nodiscard]] inline u64 MultiplyAndDivide64(u64 a, u64 b, u64 d) {
    const u64 moda = a % d;
    return a / d * b + moda * (b / d) + moda * (b % d) / d;
}

class NativeClock final {
public:
    NativeClock();

    u64 GetTscFrequency() const {
        return rdtsc_frequency;
    }
    u64 GetTimeUS(u64 base_ptc = 0) const {
        return static_cast<u64>((static_cast<unsigned __int128>(GetUptime() - base_ptc) *
                                 us_rdtsc_factor) >> 64);
    }
    u64 GetUptime() const {
        return FencedRDTSC();
    }

private:
    u64 rdtsc_frequency;
    u64 us_rdtsc_factor;
};

[[nodiscard]] std::string GetLastErrorMsg();

[[nodiscard]] std::string ToLower(std::string_view str);
std::vector<std::string> SplitString(const std::string& str, char delimiter);
[[nodiscard]] std::string UTF16ToUTF8(std::wstring_view input);
[[nodiscard]] std::wstring UTF8ToUTF16W(std::string_view str);

extern const char g_scm_rev[];

void* GetRip(void* ctx);
bool IsWriteError(void* ctx);
bool IsExecuteError(void* ctx);

} // namespace Common

[[nodiscard]] inline u64 HashCombine(const u64 seed, const u64 hash) {
    return seed ^ (hash + 0x9e3779b9 + (seed << 12) + (seed >> 4));
}

[[nodiscard]] inline u32 HashCombine(const u32 seed, const u32 hash) {
    return seed ^ (hash + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

/// Hash of the integer keys of tables that mask the hash with a power of two, as tsl::robin_map
/// does. The std::hash of MSVC runs FNV-1a over every byte of the key, a chain of eight
/// multiplications.
struct IntegerKeyHash {
    [[nodiscard]] std::size_t operator()(u64 key) const noexcept {
        key ^= key >> 33;
        key *= 0xFF51AFD7ED558CCDULL;
        key ^= key >> 33;
        return static_cast<std::size_t>(key);
    }
};

/// A value that stays at its compile-time constant; assignments are ignored.
template <typename T, T Value>
class FixedValue {
    T m_value{Value};

public:
    constexpr FixedValue() = default;
    constexpr explicit(false) operator T() const {
        return m_value;
    }
    FixedValue& operator=(const T&) {
        m_value = Value;
        return *this;
    }
};

template <class T>
class RingBufferQueue {
public:
    RingBufferQueue(u64 size) : m_storage(size) {}

    void Push(T item) {
        const u64 index = (m_begin + m_size) % m_storage.size();
        m_storage[index] = std::move(item);
        if (m_size < m_storage.size()) {
            m_size += 1;
        } else {
            m_begin = (m_begin + 1) % m_storage.size();
        }
    }

    std::optional<T> Pop() {
        if (m_size == 0) {
            return {};
        }
        const u64 index = m_begin;
        m_begin = (m_begin + 1) % m_storage.size();
        m_size -= 1;
        return std::move(m_storage[index]);
    }

    std::optional<T> Peek() {
        if (m_size == 0) {
            return {};
        }
        return m_storage[m_begin];
    }

    void Clear() {
        m_begin = 0;
        m_size = 0;
    }

private:
    u64 m_begin = 0;
    u64 m_size = 0;
    std::vector<T> m_storage;
};

namespace detail {
template <class F>
struct ScopeGuard {
    F f;
    ~ScopeGuard() {
        f();
    }
};
enum class ScopeGuardOnExit {};
template <typename F>
ScopeGuard<F> operator+(ScopeGuardOnExit, F&& f) {
    return {std::forward<F>(f)};
}
} // namespace detail

#define SCOPE_EXIT_CAT_(a, b) a##b
#define SCOPE_EXIT_CAT(a, b) SCOPE_EXIT_CAT_(a, b)
/// Runs the following block when the enclosing scope exits: SCOPE_EXIT { ... };
#define SCOPE_EXIT                                                                                 \
    auto SCOPE_EXIT_CAT(scope_exit_, __COUNTER__) = detail::ScopeGuardOnExit() + [&]()
