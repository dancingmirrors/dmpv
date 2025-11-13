#include <assert.h>

#ifndef DMPV_COMPILER_H
#define DMPV_COMPILER_H

#define MP_EXPAND_ARGS(...) __VA_ARGS__

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_ATTRIBUTE(a1, a2) __attribute__ ((format(printf, a1, a2)))
#define MP_NORETURN __attribute__((noreturn))
#define MP_FALLTHROUGH __attribute__((fallthrough))
#define MP_UNUSED __attribute__((unused))
#else
#define PRINTF_ATTRIBUTE(a1, a2)
#define MP_NORETURN
#define MP_FALLTHROUGH do {} while (0)
#endif

#ifdef __GNUC__
#define MP_ASSERT_UNREACHABLE() (assert(!"unreachable"), __builtin_unreachable())
#else
#define MP_ASSERT_UNREACHABLE() (assert(!"unreachable"), abort())
#endif

#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define likely(x) __builtin_expect(!!(x), 1)
#else
#define unlikely(x) (x)
#define likely(x) (x)
#endif

#endif
