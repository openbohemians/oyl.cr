/*
 * oyl_port.h — Compiler differences
 *
 * GCC's and Clang's builtins and attributes, with MSVC's equivalents and
 * portable C for other compilers. Not installed.
 */

#ifndef OYL_PORT_H
#define OYL_PORT_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

/* SSE2 is part of x86-64, and of 32-bit x86 as MSVC builds it by default.
 * GCC and Clang say so with __SSE2__; MSVC never does. (ARM64EC defines
 * _M_X64 too, but only emulates the intrinsics.) */
#if defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC)) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  define OYL_SSE2 1
#endif

/* For small helpers on the per-token path that GCC may otherwise decline
 * to inline once they grow a little. */
#if defined(__GNUC__) || defined(__clang__)
#  define ALWAYS_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#  define ALWAYS_INLINE static __forceinline
#else
#  define ALWAYS_INLINE static inline
#endif

/* For the hot entry points, whose speed shouldn't depend on where they
 * happen to land: on oyl_scan_flow_fast, code added elsewhere in its file
 * moved it off a 64-byte boundary and cost JSON 10%. MSVC can't align a
 * function. */
#if defined(__GNUC__) || defined(__clang__)
#  define HOT_ALIGNED __attribute__((aligned(64)))
#else
#  define HOT_ALIGNED
#endif

/* For cold paths that would otherwise be inlined into a hot caller. */
#if defined(__GNUC__) || defined(__clang__)
#  define NOINLINE static __attribute__((noinline))
#elif defined(_MSC_VER)
#  define NOINLINE static __declspec(noinline)
#else
#  define NOINLINE static
#endif

/* A function the library runs when it loads, before main: `OYL_CONSTRUCTOR(f)
 * { ... }` defines a static void f(void). MSVC's C runtime calls the
 * pointers in section .CRT$XCU at load (C++'s static constructors are
 * there too), and /include keeps the linker from dropping the pointer,
 * which nothing refers to; 32-bit x86 names C symbols with a leading _. */
#if defined(__GNUC__) || defined(__clang__)
#  define OYL_CONSTRUCTOR(f) __attribute__((constructor)) static void f(void)
#elif defined(_MSC_VER)
#  pragma section(".CRT$XCU", read)
#  if defined(_M_IX86)
#    define OYL_SYMBOL_PREFIX "_"
#  else
#    define OYL_SYMBOL_PREFIX ""
#  endif
#  define OYL_CONSTRUCTOR(f) \
       static void f(void); \
       __declspec(allocate(".CRT$XCU")) void (*f##_at_load)(void) = f; \
       __pragma(comment(linker, "/include:" OYL_SYMBOL_PREFIX #f "_at_load")) \
       static void f(void)
#endif

/* The number of trailing zero bits in x, which isn't 0 */
#if defined(__GNUC__) || defined(__clang__)
static inline unsigned oyl_ctz32(uint32_t x) { return (unsigned)__builtin_ctz(x); }
static inline unsigned oyl_ctz64(uint64_t x) { return (unsigned)__builtin_ctzll(x); }
#elif defined(_MSC_VER)
static __forceinline unsigned oyl_ctz32(uint32_t x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return (unsigned)i;
}
static __forceinline unsigned oyl_ctz64(uint64_t x) {
    unsigned long i;
#  if defined(_M_X64) || defined(_M_ARM64)
    _BitScanForward64(&i, x);
#  else
    if (!_BitScanForward(&i, (unsigned long)x)) {
        _BitScanForward(&i, (unsigned long)(x >> 32));
        i += 32;
    }
#  endif
    return (unsigned)i;
}
#else
static inline unsigned oyl_ctz32(uint32_t x) {
    unsigned n = 0;
    for (; !(x & 1); x >>= 1) n++;
    return n;
}
static inline unsigned oyl_ctz64(uint64_t x) {
    unsigned n = 0;
    for (; !(x & 1); x >>= 1) n++;
    return n;
}
#endif

/* a * b and a + b in *r, wrapping; true if they overflowed */
#if defined(__GNUC__) || defined(__clang__)
static inline bool oyl_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *r) {
    return __builtin_mul_overflow(a, b, r);
}
static inline bool oyl_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *r) {
    return __builtin_add_overflow(a, b, r);
}
#else
static inline bool oyl_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *r) {
#  if defined(_MSC_VER) && defined(_M_X64)
    uint64_t hi;
    *r = _umul128(a, b, &hi);
    return hi != 0;
#  elif defined(_MSC_VER) && defined(_M_ARM64)
    *r = a * b;
    return __umulh(a, b) != 0;
#  else
    *r = a * b;
    return b != 0 && a > UINT64_MAX / b;
#  endif
}
static inline bool oyl_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *r) {
    *r = a + b;
    return *r < a;
}
#endif

#endif /* OYL_PORT_H */
