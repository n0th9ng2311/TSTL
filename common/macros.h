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
