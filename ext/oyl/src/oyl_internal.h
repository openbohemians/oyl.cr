/*
 * oyl_internal.h — Declarations shared between oyl's source files.
 *
 * Not installed. Everything here is hidden from the shared library's
 * exported symbols (the library is built with -fvisibility=hidden, and
 * only functions marked OYL_API in oyl.h are exported), so it can change
 * freely without affecting the ABI.
 */

#ifndef OYL_INTERNAL_H
#define OYL_INTERNAL_H

#include "oyl/oyl.h"

/* ── Schema ──────────────────────────────────────────────── */

/* A single tag resolution rule: if a plain scalar matches `pattern`
 * according to `match`, it resolves to `tag`. */
typedef struct {
    oyl_match_type  match;
    const char     *pattern;   /* string for EXACT/ICASE, name for BUILTIN */
    oyl_str         tag;
} oyl_schema_rule;

struct oyl_schema {
    const oyl_schema_rule *rules;
    int                    rule_count;
    oyl_str                default_plain_tag;   /* unmatched plain scalars */
    oyl_str                default_quoted_tag;  /* all quoted scalars */
    oyl_str                default_seq_tag;     /* untagged sequences */
    oyl_str                default_map_tag;     /* untagged mappings */
};

/* ── Scanner ─────────────────────────────────────────────── */

/* Scan the next token into caller-provided storage. oyl_scan_next() wraps
 * this for the public API; the parser calls it directly. */
oyl_status oyl_scan_token(oyl_scanner *s, oyl_token *tok);
/* Copy one scanner's state into another over the same input (the parser's
 * checkpoint at a document start). */
bool oyl_scanner_copy(oyl_scanner *dst, const oyl_scanner *src);

/* For small helpers on the per-token path that GCC may otherwise decline
 * to inline once they grow a little. */
#if defined(__GNUC__) || defined(__clang__)
#  define ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#  define ALWAYS_INLINE static inline
#endif

/* For cold paths that would otherwise be inlined into a hot caller. */
#if defined(__GNUC__) || defined(__clang__)
#  define NOINLINE static __attribute__((noinline))
#else
#  define NOINLINE static
#endif

#endif /* OYL_INTERNAL_H */
