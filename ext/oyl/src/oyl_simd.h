/*
 * oyl_simd.h — Platform SIMD scan primitives
 *
 * These functions scan a buffer for the next "interesting" byte. Each has a
 * SIMD (SSE4.2) and a scalar implementation; the active one is chosen at load
 * time by a runtime CPU check (see oyl_simd.c). This lets a single compiled
 * binary use SIMD on capable hardware while staying safe on older CPUs, so it
 * is suitable for redistributable packages built without -march=native.
 */

#ifndef OYL_SIMD_H
#define OYL_SIMD_H

#include <stddef.h>
#include <stdint.h>

/* Find first structural byte (whitespace, # : , [ ] { } or NUL) that could end
 * a plain scalar. Returns its offset, or `len` if none is found. */
size_t oyl_scan_plain_scalar(const char *buf, size_t len);

/* Skip spaces and tabs. Returns offset of the first non-blank, or `len`. */
size_t oyl_skip_blanks(const char *buf, size_t len);

/* Find first line break (\n or \r). Returns its offset, or `len`. */
size_t oyl_scan_to_break(const char *buf, size_t len);

/* Scan to end of comment (to line break or EOF) — same as scan_to_break. */
#define oyl_skip_comment oyl_scan_to_break

/* Offset of the first byte in buf[0..len) equal to a, b, c or d, or `len`.
 * Inline because quoted scalars are usually short. SSE2 is part of
 * x86-64, so no runtime dispatch. */
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

static inline size_t oyl_find_any4(const char *buf, size_t len,
                                   char a, char b, char c, char d) {
    size_t i = 0;
#if defined(__SSE2__)
    const __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b);
    const __m128i vc = _mm_set1_epi8(c), vd = _mm_set1_epi8(d);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, va), _mm_cmpeq_epi8(v, vb)),
                                 _mm_or_si128(_mm_cmpeq_epi8(v, vc), _mm_cmpeq_epi8(v, vd)));
        int mask = _mm_movemask_epi8(m);
        if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    }
#endif
    for (; i < len; i++) {
        char x = buf[i];
        if (x == a || x == b || x == c || x == d) return i;
    }
    return len;
}

/* oyl_find_any4 for text that is usually short (a quoted key or JSON
 * value): a few plain compares finish most of it before SIMD setup would
 * pay off. */
static inline size_t oyl_find_any4_short(const char *buf, size_t len,
                                         char a, char b, char c, char d) {
    size_t i = 0;
    for (; i < len && i < 8; i++) {
        char x = buf[i];
        if (x == a || x == b || x == c || x == d) return i;
    }
    return i + oyl_find_any4(buf + i, len - i, a, b, c, d);
}

/* Bitmask of the bytes the flow-key lookahead looks at in the 64 bytes at
 * p (all must be readable): bit k is set if p[k] is one of ' " # [ ] { }
 * < \n \r. Inline, since it runs once per 64 input bytes in a hot loop; SSE2
 * is part of x86-64, so this needs no runtime dispatch. */

#if defined(__SSE2__)
#include <emmintrin.h>

static inline uint64_t oyl_flow_mask16(__m128i v) {
    const __m128i lower = _mm_or_si128(v, _mm_set1_epi8(0x20)); /* [ → {, ] → } */
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(lower, _mm_set1_epi8('{')),
                             _mm_cmpeq_epi8(lower, _mm_set1_epi8('}')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('"')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\'')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('#')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('<')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\n')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8('\r')));
    return (uint16_t)_mm_movemask_epi8(m);
}

static inline uint64_t oyl_flow_mask64(const char *p) {
    return oyl_flow_mask16(_mm_loadu_si128((const __m128i *)p)) |
           oyl_flow_mask16(_mm_loadu_si128((const __m128i *)(p + 16))) << 16 |
           oyl_flow_mask16(_mm_loadu_si128((const __m128i *)(p + 32))) << 32 |
           oyl_flow_mask16(_mm_loadu_si128((const __m128i *)(p + 48))) << 48;
}
#else
static inline uint64_t oyl_flow_mask64(const char *p) {
    uint64_t m = 0;
    for (int k = 0; k < 64; k++) {
        char c = p[k];
        if (c == '\'' || c == '"' || c == '#' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '<' || c == '\n' || c == '\r')
            m |= (uint64_t)1 << k;
    }
    return m;
}
#endif

#endif /* OYL_SIMD_H */
