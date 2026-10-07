/*
 * oyl_chars.h — Character classification for YAML 1.2
 *
 * Single 256-byte lookup table. Each byte is a bitfield of character classes.
 * One table load, one AND, one branch — replaces libyaml's cascade of ifs.
 *
 * Reference: YAML 1.2.2 spec, Chapter 5 "Character Productions"
 */

#ifndef OYL_CHARS_H
#define OYL_CHARS_H

#include <stdint.h>
#include <stdbool.h>

/* ── Character class bits ────────────────────────────────── */

#define YC_SPACE     0x01  /* SP (0x20) or TAB (0x09) */
#define YC_BREAK     0x02  /* LF (0x0A) or CR (0x0D) */
#define YC_WHITE     0x03  /* YC_SPACE | YC_BREAK — any whitespace */
#define YC_INDICATOR 0x04  /* flow indicators: , [ ] { } */
#define YC_DIGIT     0x08  /* 0-9 */
#define YC_ALPHA     0x10  /* a-z A-Z _ */
#define YC_HEX       0x20  /* 0-9 a-f A-F */
#define YC_PRINTABLE 0x40  /* printable ASCII (0x20-0x7E) + TAB */
#define YC_INDICATOR2 0x80 /* all indicators: - ? : , [ ] { } # & * ! | > ' " % @ ` */

/* ── Lookup table ────────────────────────────────────────── */

static const uint8_t oyl_char_table[256] = {
    /*       0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F */
    /* 0x */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00,
    /* 1x */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 2x */ 0x41, 0xC0, 0xC0, 0xC0, 0x40, 0xC0, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0x40, 0xC4, 0xC0, 0x40, 0x40,
    /* 3x */ 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0xC0, 0x40, 0x40, 0x40, 0xC0, 0xC0,
    /* 4x */ 0xC0, 0x70, 0x70, 0x70, 0x70, 0x70, 0x70, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50,
    /* 5x */ 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0xC4, 0x40, 0xC4, 0x40, 0x50,
    /* 6x */ 0xC0, 0x70, 0x70, 0x70, 0x70, 0x70, 0x70, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50,
    /* 7x */ 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0x50, 0xC4, 0xC0, 0xC4, 0x40, 0x00,
    /* 8x-Fx: all zero — high bytes handled separately for UTF-8 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* ── Inline classification functions ─────────────────────── */

static inline bool oyl_is_space(uint8_t c)     { return (oyl_char_table[c] & YC_SPACE) != 0; }
static inline bool oyl_is_break(uint8_t c)     { return (oyl_char_table[c] & YC_BREAK) != 0; }
static inline bool oyl_is_white(uint8_t c)     { return (oyl_char_table[c] & YC_WHITE) != 0; }
static inline bool oyl_is_flow(uint8_t c)      { return (oyl_char_table[c] & YC_INDICATOR) != 0; }
static inline bool oyl_is_digit(uint8_t c)     { return (oyl_char_table[c] & YC_DIGIT) != 0; }
static inline bool oyl_is_alpha(uint8_t c)     { return (oyl_char_table[c] & YC_ALPHA) != 0; }
static inline bool oyl_is_hex(uint8_t c)       { return (oyl_char_table[c] & YC_HEX) != 0; }
static inline bool oyl_is_printable(uint8_t c) { return (oyl_char_table[c] & YC_PRINTABLE) != 0; }
static inline bool oyl_is_indicator(uint8_t c) { return (oyl_char_table[c] & YC_INDICATOR2) != 0; }

static inline bool oyl_is_blank(uint8_t c) { return c == ' ' || c == '\t'; }
static inline bool oyl_is_blank_or_break(uint8_t c) { return oyl_char_table[c] & YC_WHITE; }
static inline bool oyl_is_alnum(uint8_t c) { return (oyl_char_table[c] & (YC_DIGIT | YC_ALPHA)) != 0; }

/* Characters that terminate a plain scalar in flow context */
static inline bool oyl_is_flow_scalar_end(uint8_t c) {
    return (oyl_char_table[c] & (YC_INDICATOR | YC_WHITE)) != 0 || c == ':';
}

/* ── SIMD character set for scanner fast paths ───────────── */

/*
 * These are the "structural" bytes we scan for with SIMD.
 * When we find one, we drop back to scalar code to handle it.
 *
 * For plain scalars:  : # \n \r \0 , [ ] { }
 * For whitespace skip: \n \r \t space
 * For quoted strings:  ' " \\ \n \r
 */

/* Byte ranges for SSE4.2 PCMPISTRI — pairs of [lo, hi] */
static const char oyl_simd_struct_ranges[16] = {
    '\0', ' ',   /* NUL through space: all control characters and blanks */
    '#',  '#',    /* comment */
    ',',  ',',    /* flow entry */
    ':',  ':',    /* mapping value */
    '[',  ']',    /* [ \ ] — flow seq + backslash */
    '{',  '}',    /* { | } — flow map + literal/pipe */
    '\x7f', '\x7f', /* DEL */
};

#define OYL_SIMD_STRUCT_RANGES_LEN 14

#endif /* OYL_CHARS_H */
