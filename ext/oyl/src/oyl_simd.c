/*
 * oyl_simd.c — SIMD scan primitives with runtime CPU dispatch.
 *
 * The SSE4.2 implementations carry a target("sse4.2") attribute, so their
 * intrinsics compile regardless of the global -march flags the translation
 * unit is built with. A constructor selects them at load time when the CPU
 * reports SSE4.2 support, and falls back to the portable scalar code
 * otherwise. The upshot: one binary, compiled with a portable baseline (no
 * -march=native), still uses SIMD on capable hardware and never executes an
 * illegal instruction on older CPUs — exactly what a distro package needs.
 *
 * Dispatch is via function pointers set in a load-time constructor (rather
 * than an ifunc resolver) so the same mechanism works on ELF and on macOS,
 * where STT_GNU_IFUNC is unavailable.
 */

#include "oyl_simd.h"
#include "oyl_chars.h"

#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__)
#  define OYL_X86 1
#  include <nmmintrin.h>
/* PCMPESTRI mode: unsigned bytes, range compare, least-significant match.
 * Defined as a macro (not a const int) because _mm_cmpestri requires its mode
 * argument to be a literal constant expression under Clang. */
#  define OYL_PCMP_MODE (_SIDD_UBYTE_OPS | _SIDD_CMP_RANGES | _SIDD_LEAST_SIGNIFICANT)
#endif

/* ── Scalar implementations (always available) ───────────────────────────── */

static size_t scan_plain_scalar_scalar(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c <= ' ' || c == '#' || c == ':' || c == ','
            || c == '[' || c == ']' || c == '{' || c == '}' || c == 0x7F) {
            return i;
        }
    }
    return len;
}

static size_t skip_blanks_scalar(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') return i;
    }
    return len;
}

static size_t scan_to_break_scalar(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') return i;
    }
    return len;
}

/* ── SSE4.2 implementations ──────────────────────────────────────────────── */

#if defined(OYL_X86)

__attribute__((target("sse4.2")))
static size_t scan_plain_scalar_sse42(const char *buf, size_t len) {
    size_t i = 0;

    /*
     * SSE4.2 PCMPESTRI — explicit length variant.
     * Must use explicit length because our ranges contain \0.
     * Mode: unsigned bytes, ranges, find first match.
     */
    const __m128i ranges = _mm_loadu_si128(
        (const __m128i *)oyl_simd_struct_ranges
    );

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int chunk_len = (len - i) < 16 ? (int)(len - i) : 16;
        int idx = _mm_cmpestri(ranges, OYL_SIMD_STRUCT_RANGES_LEN,
                               chunk, chunk_len, OYL_PCMP_MODE);
        if (idx < 16) return i + idx;
    }

    for (; i < len; i++) {
        uint8_t c = (uint8_t)buf[i];
        if (c <= ' ' || c == '#' || c == ':' || c == ','
            || c == '[' || c == ']' || c == '{' || c == '}' || c == 0x7F) {
            return i;
        }
    }
    return len;
}

__attribute__((target("sse4.2")))
static size_t skip_blanks_sse42(const char *buf, size_t len) {
    size_t i = 0;

    const __m128i spaces = _mm_set1_epi8(' ');
    const __m128i tabs   = _mm_set1_epi8('\t');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i eq_sp = _mm_cmpeq_epi8(chunk, spaces);
        __m128i eq_tb = _mm_cmpeq_epi8(chunk, tabs);
        __m128i is_blank = _mm_or_si128(eq_sp, eq_tb);
        int mask = _mm_movemask_epi8(is_blank);
        if (mask != 0xFFFF) {
            /* not all blanks — find first non-blank */
            return i + (size_t)__builtin_ctz((unsigned)~mask);
        }
    }

    for (; i < len; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') return i;
    }
    return len;
}

__attribute__((target("sse4.2")))
static size_t scan_to_break_sse42(const char *buf, size_t len) {
    size_t i = 0;

    const __m128i nl = _mm_set1_epi8('\n');
    const __m128i cr = _mm_set1_epi8('\r');

    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        __m128i eq_nl = _mm_cmpeq_epi8(chunk, nl);
        __m128i eq_cr = _mm_cmpeq_epi8(chunk, cr);
        int mask = _mm_movemask_epi8(_mm_or_si128(eq_nl, eq_cr));
        if (mask) return i + (size_t)__builtin_ctz((unsigned)mask);
    }

    for (; i < len; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') return i;
    }
    return len;
}

#endif /* OYL_X86 */

/* ── Dispatch ────────────────────────────────────────────────────────────── */

static size_t (*scan_plain_scalar_impl)(const char *, size_t) =
    scan_plain_scalar_scalar;
static size_t (*skip_blanks_impl)(const char *, size_t) =
    skip_blanks_scalar;
static size_t (*scan_to_break_impl)(const char *, size_t) =
    scan_to_break_scalar;

__attribute__((constructor))
static void oyl_simd_init(void) {
#if defined(OYL_X86)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2")) {
        scan_plain_scalar_impl = scan_plain_scalar_sse42;
        skip_blanks_impl       = skip_blanks_sse42;
        scan_to_break_impl     = scan_to_break_sse42;
    }
#endif
}

size_t oyl_scan_plain_scalar(const char *buf, size_t len) {
    return scan_plain_scalar_impl(buf, len);
}

size_t oyl_skip_blanks(const char *buf, size_t len) {
    return skip_blanks_impl(buf, len);
}

size_t oyl_scan_to_break(const char *buf, size_t len) {
    return scan_to_break_impl(buf, len);
}
