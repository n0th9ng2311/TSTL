#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define TSTL_LIKELY(x) __builtin_expect(!!(x), 1)
#define TSTL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TSTL_FORCE_INLINE __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
#define TSTL_LIKELY(x) (x)
#define TSTL_UNLIKELY(x) (x)
#define TSTL_FORCE_INLINE __forceinline
#else
#define TSTL_LIKELY(x) (x)
#define TSTL_UNLIKELY(x) (x)
#define TSTL_FORCE_INLINE inline
#endif


#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#endif


namespace detail {
    inline void spin_hint() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#elif defined(__arm__) || defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
    }

#if defined(__cpp_lib_hardware_interference_size)
    inline constexpr std::size_t CACHE_LINE_SIZE = std::hardware_destructive_interference_size;
#elif defined(__aarch64__) || defined(_M_ARM64)
    inline constexpr std::size_t CACHE_LINE_SIZE = 128; // Apple Silicon / Graviton
#else
    inline constexpr std::size_t CACHE_LINE_SIZE = 64; // x86_64 / Default
#endif
} // namespace detail
