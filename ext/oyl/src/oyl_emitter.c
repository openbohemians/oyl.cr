/*
 * oyl_emitter.c — YAML 1.2 event emitter
 *
 * Consumes parser events and produces YAML text in three styles:
 *   - BLOCK: standard indented YAML (default)
 *   - FLOW:  JSON-like inline with spaces
 *   - MINIMAL: compact flow, no optional whitespace
 */

#include "oyl_internal.h"
#include "oyl_chars.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(__SSE2__)
#include <emmintrin.h>   /* part of x86-64: no runtime check */
#endif

/* ── Context types ───────────────────────────────────────── */

typedef enum {
    EMIT_CTX_BLOCK_MAP,
    EMIT_CTX_BLOCK_SEQ,
    EMIT_CTX_FLOW_MAP,
    EMIT_CTX_FLOW_SEQ,
} emit_ctx_type;

typedef struct {
    emit_ctx_type type;
    int           indent;     /* absolute column for this level */
    int           count;      /* entries emitted so far */
    bool          expect_key; /* in mappings: true=key, false=value */
    bool          complex_key; /* block mapping: current key used "? " */
} emit_ctx;

/* ── Emitter state ───────────────────────────────────────── */

typedef struct {
    oyl_emit_style style;
    int            indent;   /* spaces per level, 1-10 */
} emit_opts;

struct oyl_emitter {
    emit_opts     opts;
    oyl_arena    *arena;

    /* output buffer (malloc'd) */
    char   *buf;
    size_t  len;
    size_t  cap;

    /* context stack */
    emit_ctx *stack;
    int       stack_len;
    int       stack_cap;
    int       flow_depth;   /* flow collections on the stack */

    /* where the events are: oyl_emit checks they come in a valid order */
    bool stream_started;
    bool stream_ended;
    bool doc_open;
    bool root_done;       /* the document has its root node (maybe still open) */
    bool open_ended;      /* the last document ended without "...", so the
                           * next one needs "---" */
    bool bare_doc;        /* the document started without "---" */
    oyl_status error;     /* the first error; every later call returns it */

    /* the directives of the next document, then of the one open; its tags
     * are written with their handles */
    bool has_version;     /* %YAML 1.version_minor */
    int  version_minor;
    int  n_tag_dirs;
    struct {
        char     handle[16];
        char     prefix[256];   /* its value: written with %-escapes where needed */
        uint8_t  hlen;
        uint16_t plen;
    } tag_dirs[8];

    /* state */
    bool first_doc;
    bool wrote_block_key; /* just wrote a block map key, value next */
    bool after_seq_dash;  /* just wrote "- " for a block seq entry */
    bool key_was_alias;   /* the current mapping key is an alias */
    bool wrote_props;     /* the current node has an anchor or tag */
    size_t key_start;     /* where the current block mapping key starts */
};

/* ── Failing ─────────────────────────────────────────────── */

/* The emitter fails: it keeps the first error and sets its state so that
 * every later call fails the checks of event order, and comes here for
 * that error. Nothing on the way of a call that succeeds checks for it. */
NOINLINE oyl_status emit_fail(oyl_emitter *e, oyl_status st) {
    if (!e->error) e->error = st;
    e->stream_started = e->stream_ended = true;
    e->doc_open = false;
    e->stack_len = 0;
    e->flow_depth = 0;
    return e->error;
}

oyl_status oyl_emitter_fail(oyl_emitter *e, oyl_status st) {
    return emit_fail(e, st);
}

/* ── Buffer management ───────────────────────────────────── */

#define EMIT_INIT_CAP 4096

NOINLINE oyl_status buf_grow(oyl_emitter *e, size_t need) {
    if (need > SIZE_MAX - e->len) return emit_fail(e, OYL_ERR_MEMORY);
    size_t new_cap = e->cap * 2;
    if (new_cap < e->len + need) new_cap = e->len + need;
    char *nb = realloc(e->buf, new_cap);
    if (!nb) return emit_fail(e, OYL_ERR_MEMORY);
    e->buf = nb;
    e->cap = new_cap;
    return OYL_OK;
}

static inline oyl_status buf_ensure(oyl_emitter *e, size_t need) {
    return need <= e->cap - e->len ? OYL_OK : buf_grow(e, need);
}

static inline oyl_status buf_put(oyl_emitter *e, char c) {
    oyl_status st = buf_ensure(e, 1);
    if (st != OYL_OK) return st;
    e->buf[e->len++] = c;
    return OYL_OK;
}

static oyl_status buf_puts(oyl_emitter *e, const char *s, size_t slen) {
    oyl_status st = buf_ensure(e, slen);
    if (st != OYL_OK) return st;
    if (slen) memcpy(e->buf + e->len, s, slen);
    e->len += slen;
    return OYL_OK;
}

#define PUTS(e, lit) buf_puts(e, lit, sizeof(lit) - 1)

static oyl_status buf_indent(oyl_emitter *e, int spaces) {
    if (spaces <= 0) return OYL_OK;
    oyl_status st = buf_ensure(e, (size_t)spaces);
    if (st != OYL_OK) return st;
    memset(e->buf + e->len, ' ', (size_t)spaces);
    e->len += (size_t)spaces;
    return OYL_OK;
}

/* ── Context stack ───────────────────────────────────────── */

static oyl_status push_ctx(oyl_emitter *e, emit_ctx_type type, int indent) {
    if (e->stack_len >= e->stack_cap) {
        int new_cap = e->stack_cap * 2;
        if (new_cap < 8) new_cap = 8;
        emit_ctx *ns = realloc(e->stack, (size_t)new_cap * sizeof(emit_ctx));
        if (!ns) return emit_fail(e, OYL_ERR_MEMORY);
        e->stack = ns;
        e->stack_cap = new_cap;
    }
    e->stack[e->stack_len++] = (emit_ctx){
        .type = type,
        .indent = indent,
        .count = 0,
        .expect_key = (type == EMIT_CTX_BLOCK_MAP || type == EMIT_CTX_FLOW_MAP),
    };
    if (type == EMIT_CTX_FLOW_MAP || type == EMIT_CTX_FLOW_SEQ) e->flow_depth++;
    return OYL_OK;
}

static emit_ctx *top_ctx(oyl_emitter *e) {
    if (e->stack_len == 0) return NULL;
    return &e->stack[e->stack_len - 1];
}

static void pop_ctx(oyl_emitter *e) {
    if (e->stack_len == 0) return;
    emit_ctx_type type = e->stack[--e->stack_len].type;
    if (type == EMIT_CTX_FLOW_MAP || type == EMIT_CTX_FLOW_SEQ) e->flow_depth--;
}

static bool in_any_flow(oyl_emitter *e) {
    return e->flow_depth > 0;
}

/* ── Separator strings ───────────────────────────────────── */

static const char *entry_sep(oyl_emitter *e) {
    return (e->opts.style == OYL_EMIT_MINIMAL) ? "," : ", ";
}

static int entry_sep_len(oyl_emitter *e) {
    return (e->opts.style == OYL_EMIT_MINIMAL) ? 1 : 2;
}

static const char *kv_sep(oyl_emitter *e) {
    (void)e;
    return ": "; /* always space after : for safe round-tripping */
}

static int kv_sep_len(oyl_emitter *e) {
    (void)e;
    return 2;
}

/* ── Scalar quoting decision ─────────────────────────────── */

static bool is_yaml_keyword(const char *s, size_t len) {
    if (len == 0) return false;
    /* null variants */
    if (len == 4 && memcmp(s, "null", 4) == 0) return true;
    if (len == 4 && memcmp(s, "Null", 4) == 0) return true;
    if (len == 4 && memcmp(s, "NULL", 4) == 0) return true;
    if (len == 1 && s[0] == '~') return true;
    /* bool variants */
    if (len == 4 && memcmp(s, "true", 4) == 0) return true;
    if (len == 4 && memcmp(s, "True", 4) == 0) return true;
    if (len == 4 && memcmp(s, "TRUE", 4) == 0) return true;
    if (len == 5 && memcmp(s, "false", 5) == 0) return true;
    if (len == 5 && memcmp(s, "False", 5) == 0) return true;
    if (len == 5 && memcmp(s, "FALSE", 5) == 0) return true;
    /* special floats */
    if (len == 4 && memcmp(s, ".inf", 4) == 0) return true;
    if (len == 5 && memcmp(s, "+.inf", 5) == 0) return true;
    if (len == 5 && memcmp(s, "-.inf", 5) == 0) return true;
    if (len == 4 && memcmp(s, ".nan", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".Inf", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".NaN", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".Nan", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".INF", 4) == 0) return true;
    if (len == 4 && memcmp(s, ".NAN", 4) == 0) return true;
    return false;
}

/* Forward declarations for int/float matchers from oyl_schema.c */
/* We duplicate the logic here to avoid exposing internal functions */
static bool looks_like_number(const char *s, size_t len) {
    if (len == 0) return false;
    size_t i = 0;
    if (s[0] == '+' || s[0] == '-') { i = 1; if (i >= len) return false; }
    /* hex/octal */
    if (i + 1 < len && s[i] == '0' && (s[i+1] == 'x' || s[i+1] == 'o'))
        return true; /* close enough — if it starts 0x/0o, quote it */
    /* digits */
    bool all_digit = true;
    bool has_dot = false;
    bool has_e = false;
    for (size_t j = i; j < len; j++) {
        if (s[j] >= '0' && s[j] <= '9') continue;
        if (s[j] == '.' && !has_dot) { has_dot = true; continue; }
        if ((s[j] == 'e' || s[j] == 'E') && !has_e) { has_e = true; continue; }
        if ((s[j] == '+' || s[j] == '-') && j > 0 && (s[j-1] == 'e' || s[j-1] == 'E')) continue;
        all_digit = false;
        break;
    }
    return all_digit;
}

/* What a scalar's bytes contain. One pass over it (scalar_classes) answers
 * most of what choosing its style asks; the rest is about its ends. */
enum {
    SC_CTRL  = 1 << 0,   /* a control character other than tab and \n, or DEL */
    SC_BREAK = 1 << 1,   /* \n or \r */
    SC_LF    = 1 << 2,   /* \n */
    SC_COLON = 1 << 3,
    SC_HASH  = 1 << 4,
    SC_FLOW  = 1 << 5,   /* , [ ] { } */
    SC_UNI   = 1 << 6,   /* a lead byte of a character escaped_char() finds */
    SC_ESC   = 1 << 7,   /* a character escaped_char() finds: set by scalar_classes */
};

/* Text this long is classified 16 bytes at a time, where SSE2 is */
#define SC_SIMD_MIN 32

#define C_ SC_CTRL
#define LF (SC_BREAK | SC_LF)
#define CR (SC_CTRL | SC_BREAK)
#define CO SC_COLON
#define HA SC_HASH
#define FL SC_FLOW
#define UN SC_UNI
static const uint8_t scalar_class[256] = {
    /*       0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
    /* 0x */ C_, C_, C_, C_, C_, C_, C_, C_, C_, 0,  LF, C_, C_, CR, C_, C_,
    /* 1x */ C_, C_, C_, C_, C_, C_, C_, C_, C_, C_, C_, C_, C_, C_, C_, C_,
    /* 2x */ 0,  0,  0,  HA, 0,  0,  0,  0,  0,  0,  0,  0,  FL, 0,  0,  0,
    /* 3x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  CO, 0,  0,  0,  0,  0,
    /* 4x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* 5x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  FL, 0,  FL, 0,  0,
    /* 6x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* 7x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  FL, 0,  FL, 0,  C_,
    /* 8x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* 9x */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* Ax */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* Bx */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* Cx */ 0,  0,  UN, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* Dx */ 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    /* Ex */ 0,  0,  UN, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  UN,
    /* Fx: 0 */
};
#undef C_
#undef LF
#undef CR
#undef CO
#undef HA
#undef FL
#undef UN

/* Is there a character at s[i] that only a double-quoted scalar can hold,
 * escaped? Returns its length in bytes (0 if none) and its code point.
 * They are the C1 control characters, U+FEFF (a byte order mark), U+FFFE
 * and U+FFFF, which YAML doesn't allow unescaped; and NEL, U+2028 and
 * U+2029, which it does, but which YAML 1.1 readers take as line breaks.
 * Oyl doesn't decode UTF-8, so invalid UTF-8 is written as it is. */
static size_t escaped_char(const char *s, size_t len, size_t i, uint32_t *code) {
    uint8_t c = (uint8_t)s[i];
    if (c == 0xC2 && i + 1 < len && (uint8_t)s[i + 1] >= 0x80 && (uint8_t)s[i + 1] <= 0x9F) {
        *code = (uint8_t)s[i + 1];
        return 2;
    }
    if (i + 2 >= len) return 0;
    uint8_t c1 = (uint8_t)s[i + 1], c2 = (uint8_t)s[i + 2];
    if ((c == 0xE2 && c1 == 0x80 && (c2 == 0xA8 || c2 == 0xA9)) ||
        (c == 0xEF && ((c1 == 0xBB && c2 == 0xBF) || (c1 == 0xBF && (c2 == 0xBE || c2 == 0xBF))))) {
        *code = (uint32_t)(c & 0x0F) << 12 | (uint32_t)(c1 & 0x3F) << 6 | (c2 & 0x3F);
        return 3;
    }
    return 0;
}

#if defined(__SSE2__)
#define SPLAT(x) _mm_set1_epi8((char)(x))
#define EQ(a, b) _mm_cmpeq_epi8(a, b)
#define OR(a, b) _mm_or_si128(a, b)
#define AND(a, b) _mm_and_si128(a, b)
/* Where in v0 a character escaped_char() finds starts, given the 16
 * bytes from one (v1) and two (v2) bytes on, as a mask of 0xFF bytes */
static inline __m128i escaped_mask(__m128i v0, __m128i v1, __m128i v2) {
    __m128i ef = EQ(v0, SPLAT(0xEF));
    __m128i c1 = AND(EQ(v0, SPLAT(0xC2)), EQ(AND(v1, SPLAT(0xE0)), SPLAT(0x80)));   /* C2 80-9F */
    __m128i ls = AND(AND(EQ(v0, SPLAT(0xE2)), EQ(v1, SPLAT(0x80))),
                     EQ(AND(v2, SPLAT(0xFE)), SPLAT(0xA8)));                       /* E2 80 A8-A9 */
    __m128i bom = AND(AND(ef, EQ(v1, SPLAT(0xBB))), EQ(v2, SPLAT(0xBF)));          /* EF BB BF */
    __m128i nc = AND(AND(ef, EQ(v1, SPLAT(0xBF))),
                     EQ(AND(v2, SPLAT(0xFE)), SPLAT(0xBE)));                       /* EF BF BE-BF */
    return OR(OR(c1, ls), OR(bom, nc));
}

/* The same for b[0..16), reading two bytes past them, as a bit mask */
static inline int escaped_starts(const char *b) {
    return _mm_movemask_epi8(escaped_mask(_mm_loadu_si128((const __m128i *)b),
                                          _mm_loadu_si128((const __m128i *)(b + 1)),
                                          _mm_loadu_si128((const __m128i *)(b + 2))));
}

/* Bytes under 0x20 */
static inline __m128i below_space(__m128i v) {
    return EQ(_mm_min_epu8(v, SPLAT(0x1F)), v);
}
#endif

/* Is there a character escaped_char() finds in s[from..len)? */
NOINLINE bool has_escaped_char(const char *s, size_t from, size_t len) {
    uint32_t code;
    for (size_t i = from; i < len; i++)
        if ((scalar_class[(uint8_t)s[i]] & SC_UNI) && escaped_char(s, len, i, &code))
            return true;
    return false;
}

/* The classes of s[from..len), a byte at a time, with SC_ESC */
static inline unsigned scalar_classes_bytes(const char *s, size_t from, size_t len) {
    unsigned f = 0;
    for (size_t i = from; i < len; i++) f |= scalar_class[(uint8_t)s[i]];
    /* rare: a C2, E2 or EF lead byte */
    if ((f & SC_UNI) && has_escaped_char(s, from, len)) f |= SC_ESC;
    return f;
}

#if defined(__SSE2__)
/* The first character escaped_char() finds in s[from..len): its offset,
 * or len. 16 starting bytes at a time, each with the two after it */
static size_t next_escaped_char(const char *s, size_t from, size_t len) {
    size_t i = from;
    for (; i + 18 <= len; i += 16) {
        int m = escaped_starts(s + i);
        if (m) return i + (size_t)__builtin_ctz((unsigned)m);
    }
    uint32_t code;
    for (; i < len; i++)
        if ((scalar_class[(uint8_t)s[i]] & SC_UNI) && escaped_char(s, len, i, &code)) return i;
    return len;
}

/* The classes of s[0..len), len at least 16, 16 bytes at a time: each
 * class kept as a mask, without a branch; an escaped character looked for
 * afterwards, if there was a byte one could start with. Not inlined: its
 * registers would cost every short scalar. */
NOINLINE unsigned scalar_classes_long(const char *s, size_t len) {
    __m128i ctrl = _mm_setzero_si128(), lf = ctrl, cr = ctrl, colon = ctrl, hash = ctrl,
            flow = ctrl, lead = ctrl;
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i n = EQ(v, SPLAT('\n'));
        /* below 0x20 but tab and \n, and DEL */
        ctrl = OR(ctrl, OR(_mm_andnot_si128(OR(n, EQ(v, SPLAT('\t'))), below_space(v)),
                           EQ(v, SPLAT(0x7F))));
        lf = OR(lf, n);
        cr = OR(cr, EQ(v, SPLAT('\r')));
        colon = OR(colon, EQ(v, SPLAT(':')));
        hash = OR(hash, EQ(v, SPLAT('#')));
        /* [ and { are 0x7B with 0x20 set, ] and } 0x7D */
        __m128i w = OR(v, SPLAT(0x20));
        flow = OR(flow, OR(EQ(v, SPLAT(',')), OR(EQ(w, SPLAT(0x7B)), EQ(w, SPLAT(0x7D)))));
        lead = OR(lead, OR(EQ(v, SPLAT(0xC2)), OR(EQ(v, SPLAT(0xE2)), EQ(v, SPLAT(0xEF)))));
    }
    unsigned f = 0;
    for (size_t j = i; j < len; j++) f |= scalar_class[(uint8_t)s[j]];
    if (_mm_movemask_epi8(ctrl)) f |= SC_CTRL;
    if (_mm_movemask_epi8(lf)) f |= SC_BREAK | SC_LF;
    if (_mm_movemask_epi8(cr)) f |= SC_BREAK;
    if (_mm_movemask_epi8(colon)) f |= SC_COLON;
    if (_mm_movemask_epi8(hash)) f |= SC_HASH;
    if (_mm_movemask_epi8(flow)) f |= SC_FLOW;
    if (_mm_movemask_epi8(lead)) f |= SC_UNI;
    if ((f & SC_UNI) && next_escaped_char(s, 0, len) < len) f |= SC_ESC;
    return f;
}
#endif

/* The classes of all of s, with SC_ESC and without SC_UNI (which marks
 * bytes, for escaped_char) */
static unsigned scalar_classes(const char *s, size_t len) {
#if defined(__SSE2__)
    if (len >= SC_SIMD_MIN) return scalar_classes_long(s, len) & ~(unsigned)SC_UNI;
#endif
    return scalar_classes_bytes(s, 0, len) & ~(unsigned)SC_UNI;
}

/* Could this text be misread if emitted plain? Only syntax is considered
 * here; values that would resolve to another type (numbers, true, null)
 * are handled by choose_style. `f` is scalar_classes(s, len). */
static bool needs_quoting(const char *s, size_t len, bool flow_ctx, unsigned f) {
    if (len == 0) return true;

    /* starts with an indicator. '-', '?' and ':' may start a plain scalar
     * when followed by a "safe" character: not a blank, and in flow
     * context not a flow indicator ("-123", "?x", ":x", "-foo"). In flow
     * context "?x" and ":x" are quoted all the same: libyaml and libfyaml
     * read "[:x]" and "[?x]" as mappings. */
    uint8_t first = (uint8_t)s[0];
    if (oyl_is_indicator(first)) {
        if (!(first == '-' || first == '?' || first == ':')) return true;
        if (len < 2) return true;
        uint8_t next = (uint8_t)s[1];
        if (oyl_is_blank_or_break(next)) return true;
        if (flow_ctx && (first != '-' || oyl_is_flow(next))) return true;
    }
    /* space or tab at either end */
    if (first == ' ' || first == '\t') return true;
    if (s[len - 1] == ' ' || s[len - 1] == '\t') return true;

    /* document markers */
    if (len >= 3 && (memcmp(s, "---", 3) == 0 || memcmp(s, "...", 3) == 0))
        return true;

    /* line breaks, and flow indicators in flow context */
    if (f & SC_BREAK) return true;
    if (flow_ctx && (f & SC_FLOW)) return true;

    /* ": ", ":\t", and a trailing ':' (which would read as a mapping value
     * indicator); " #" and "\t#" (a comment) */
    if (f & SC_COLON) {
        for (const char *c = memchr(s, ':', len); c; c = memchr(c + 1, ':', len - (size_t)(c + 1 - s))) {
            size_t i = (size_t)(c - s);
            if (i + 1 == len || s[i + 1] == ' ' || s[i + 1] == '\t') return true;
        }
    }
    if (f & SC_HASH) {
        for (const char *c = memchr(s, '#', len); c; c = memchr(c + 1, '#', len - (size_t)(c + 1 - s))) {
            size_t i = (size_t)(c - s);
            if (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) return true;
        }
    }
    return false;
}

/* Can `s` be written as a literal block scalar and read back exactly?
 * Not with control characters (other than tab and line feed) or the
 * characters escaped_char() finds, and not when the first non-empty line
 * starts with a space: that would need an indentation indicator. `f` is
 * scalar_classes(s, len). */
static bool literal_ok(const char *s, size_t len, unsigned f) {
    if (f & (SC_CTRL | SC_ESC)) return false;
    size_t i = 0;
    while (i < len && s[i] == '\n') i++;
    return !(i < len && s[i] == ' ');
}

static bool is_str_tag(oyl_str tag) {
    static const char str_tag[] = "tag:yaml.org,2002:str";
    return tag.len == sizeof(str_tag) - 1 && memcmp(tag.data, str_tag, tag.len) == 0;
}

/* A plain scalar is emitted plain whenever its text survives that: a plain
 * event means "resolve by schema", so 8080 or true stay untyped text and
 * reparse as they came in. Only a scalar tagged !!str that would resolve to
 * another type (number, bool, null) is quoted to keep it a string. */
static oyl_scalar_style choose_style(oyl_scalar_style requested,
                                     const char *val, size_t len,
                                     bool flow_ctx, bool is_key, oyl_str tag) {
    if (requested == OYL_SCALAR_DOUBLE_QUOTED) return requested;
    unsigned f = scalar_classes(val, len);
    /* Block scalars are written literal (folded values are already
     * folded, and literal keeps them exact), only in block context and
     * not as keys; otherwise double-quoted */
    if (requested == OYL_SCALAR_LITERAL || requested == OYL_SCALAR_FOLDED)
        return !flow_ctx && !is_key && literal_ok(val, len, f) ? OYL_SCALAR_LITERAL
                                                                : OYL_SCALAR_DOUBLE_QUOTED;
    /* single quotes fold line breaks and can't escape: keep the value
     * exact with double quotes */
    if (requested == OYL_SCALAR_SINGLE_QUOTED)
        return f & (SC_BREAK | SC_CTRL | SC_ESC) ? OYL_SCALAR_DOUBLE_QUOTED : requested;

    /* PLAIN requested — auto-detect */
    if (len == 0 && !is_str_tag(tag))
        return OYL_SCALAR_PLAIN;              /* empty (null) node */
    if (f & (SC_CTRL | SC_LF | SC_ESC))
        return OYL_SCALAR_DOUBLE_QUOTED;      /* control characters, line feeds */
    if (!needs_quoting(val, len, flow_ctx, f)) {
        if (is_str_tag(tag) && (is_yaml_keyword(val, len) || looks_like_number(val, len)))
            return OYL_SCALAR_DOUBLE_QUOTED;
        return OYL_SCALAR_PLAIN;
    }
    /* needs quoting; without control characters or line feeds there's no
     * line break either, so a literal block scalar isn't an option */
    return OYL_SCALAR_DOUBLE_QUOTED;
}

/* ── Scalar emission ─────────────────────────────────────── */

static oyl_status emit_plain(oyl_emitter *e, const char *s, size_t len) {
    return buf_puts(e, s, len);
}

/* Quoted scalars are written in chunks of CHUNK bytes, each with room
 * reserved once for its longest escaped form */
#define CHUNK 4096

static oyl_status emit_single_quoted(oyl_emitter *e, const char *s, size_t len) {
    oyl_status st = buf_put(e, '\'');
    for (size_t i = 0; i < len && st == OYL_OK;) {
        /* the text through the next quote, which is then doubled */
        const char *q = memchr(s + i, '\'', len - i);
        size_t run = q ? (size_t)(q - s) + 1 - i : len - i;
        st = buf_puts(e, s + i, run);
        if (q && st == OYL_OK) st = buf_put(e, '\'');
        i += run;
    }
    return st == OYL_OK ? buf_put(e, '\'') : st;
}

/* The escape for a character escaped_char() finds, written at `o`: \N, \L
 * and \P, \xHH for the C1 controls, \uHHHH for the rest */
static char *put_escape(char *o, uint32_t code) {
    static const char hex[] = "0123456789ABCDEF";
    *o++ = '\\';
    if (code == 0x85 || code == 0x2028 || code == 0x2029) {
        *o++ = code == 0x85 ? 'N' : code == 0x2028 ? 'L' : 'P';
    } else if (code < 0x100) {
        *o++ = 'x';
        *o++ = hex[code >> 4];
        *o++ = hex[code & 15];
    } else {
        *o++ = 'u';
        for (int sh = 12; sh >= 0; sh -= 4) *o++ = hex[(code >> sh) & 15];
    }
    return o;
}

/* Write the escape for s[*i], a byte double quotes can't hold as it is
 * or the start of a character escaped_char() finds, at `o`; or, for a
 * lead byte of no such character, the byte. Moves *i past it. */
static char *put_dq_special(char *o, const char *s, size_t len, size_t *i) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t c = (uint8_t)s[*i];
    if (c >= 0x80) {   /* C2, E2 or EF */
        uint32_t code;
        size_t w = escaped_char(s, len, *i, &code);
        if (!w) {
            *o++ = (char)c;
            *i += 1;
            return o;
        }
        *i += w;
        return put_escape(o, code);
    }
    *i += 1;
    *o++ = '\\';
    switch (c) {
    case '\\': *o++ = '\\'; break;
    case '"':  *o++ = '"'; break;
    case '\n': *o++ = 'n'; break;
    case '\r': *o++ = 'r'; break;
    case '\t': *o++ = 't'; break;
    case '\0': *o++ = '0'; break;
    case '\a': *o++ = 'a'; break;
    case '\b': *o++ = 'b'; break;
    default:   /* other control characters and DEL */
        *o++ = 'x';
        *o++ = hex[c >> 4];
        *o++ = hex[c & 15];
        break;
    }
    return o;
}

NOINLINE oyl_status emit_double_quoted(oyl_emitter *e, const char *s, size_t len) {
    oyl_status st = buf_put(e, '"');
    for (size_t i = 0; i < len && st == OYL_OK;) {
        size_t n = len - i < CHUNK ? len - i : CHUNK;
        /* \xHH for each byte, and a \uHHHH begun at the last one */
        if ((st = buf_ensure(e, 4 * n + 2)) != OYL_OK) break;
        char *o = e->buf + e->len;
        size_t end = i + n;
#if defined(__SSE2__)
        /* 16 bytes at a time, each block up to the first byte that needs
         * an escape (an escaped character's two bytes after read too) */
        while (i + 16 <= end && i + 18 <= len) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
            int m = _mm_movemask_epi8(OR(OR(below_space(v), EQ(v, SPLAT(0x7F))),
                                         OR(EQ(v, SPLAT('\\')), EQ(v, SPLAT('"')))));
            if (_mm_movemask_epi8(v)) m |= escaped_starts(s + i);
            if (!m) {
                _mm_storeu_si128((__m128i *)o, v);
                o += 16;
                i += 16;
                continue;
            }
            size_t k = (size_t)__builtin_ctz((unsigned)m);
            memcpy(o, s + i, k);
            o += k;
            i += k;
            o = put_dq_special(o, s, len, &i);
        }
#endif
        while (i < end) {
            uint8_t c = (uint8_t)s[i];
            if ((uint8_t)(c - 0x20) < 0x5F && c != '\\' && c != '"') {   /* ASCII but controls and DEL */
                *o++ = (char)c;
                i++;
            } else if (c >= 0x80 && !(scalar_class[c] & SC_UNI)) {
                *o++ = (char)c;
                i++;
            } else {
                o = put_dq_special(o, s, len, &i);
            }
        }
        e->len = (size_t)(o - e->buf);
    }
    return st == OYL_OK ? buf_put(e, '"') : st;
}

/* Write `s` as a literal block scalar, which reads back exactly. Callers
 * check literal_ok() first. Chomping follows the trailing line breaks:
 * none "|-", one "|", more (or nothing but breaks) "|+". Empty lines are
 * written without indentation. */
static oyl_status emit_block_scalar(oyl_emitter *e, const char *s, size_t len,
                                    int indent) {
    oyl_status st;
    size_t trail = 0;
    while (trail < len && s[len - 1 - trail] == '\n') trail++;
    size_t body = len - trail;

    const char *header = body == 0 ? (trail ? "|+\n" : "|-\n")
                       : trail == 0 ? "|-\n" : trail == 1 ? "|\n" : "|+\n";
    st = buf_puts(e, header, strlen(header));
    if (st != OYL_OK) return st;

    /* body lines, each ending in a line break */
    size_t pos = 0;
    while (pos < body) {
        const char *nl = memchr(s + pos, '\n', body - pos);
        size_t eol = nl ? (size_t)(nl - s) : body;
        if (eol > pos) {
            st = buf_indent(e, indent);
            if (st != OYL_OK) return st;
            st = buf_puts(e, s + pos, eol - pos);
            if (st != OYL_OK) return st;
        }
        st = buf_put(e, '\n');
        if (st != OYL_OK) return st;
        pos = eol + 1;
    }

    /* the first trailing break ended the last body line; kept extras are
     * empty lines (all of them when there is no body) */
    for (size_t i = body ? 1 : 0; i < trail; i++) {
        st = buf_put(e, '\n');
        if (st != OYL_OK) return st;
    }
    return OYL_OK;
}

static oyl_status emit_scalar(oyl_emitter *e, oyl_str value, oyl_scalar_style requested,
                              oyl_str tag) {
    const char *val = value.data;
    size_t vlen = value.len;
    bool flow_ctx = in_any_flow(e) || e->opts.style != OYL_EMIT_BLOCK;

    emit_ctx *ctx = top_ctx(e);
    bool is_key = ctx && (ctx->type == EMIT_CTX_BLOCK_MAP || ctx->type == EMIT_CTX_FLOW_MAP) &&
                  ctx->expect_key;
    oyl_scalar_style style = choose_style(requested, val, vlen, flow_ctx, is_key, tag);

    int indent = ctx ? ctx->indent + e->opts.indent : e->opts.indent;

    switch (style) {
    case OYL_SCALAR_PLAIN:
        if (vlen == 0) {
            /* empty node: nothing to write, except in a flow sequence
             * where "[a, ]" would drop the entry, so write ~ (null). Keep
             * the space after an anchor or tag ("&a : v", not "&a: v") */
            if (ctx && ctx->type == EMIT_CTX_FLOW_SEQ && !e->wrote_props)
                return buf_put(e, '~');
            /* a document of nothing reads back only with "---" */
            if (!ctx && !e->wrote_props && e->bare_doc)
                return PUTS(e, "---");
            if (!e->wrote_props && !is_key && e->len > 0 && e->buf[e->len - 1] == ' ')
                e->len--;
            return OYL_OK;
        }
        return emit_plain(e, val, vlen);
    case OYL_SCALAR_SINGLE_QUOTED:
        return emit_single_quoted(e, val, vlen);
    case OYL_SCALAR_DOUBLE_QUOTED:
        return emit_double_quoted(e, val, vlen);
    case OYL_SCALAR_LITERAL:
    case OYL_SCALAR_FOLDED:
        return emit_block_scalar(e, val, vlen, indent);
    }
    return emit_fail(e, OYL_ERR_EMIT);
}

/* ── Property emission (anchor, tag) ─────────────────────── */

/* Can `name` be written as an anchor or alias name, and read back as it
 * is? Not empty, and every character YAML's ns-anchor-char: no spaces,
 * line breaks or flow indicators, nothing YAML must escape (control
 * characters, DEL, the C1 controls but NEL, U+FEFF, U+FFFE, U+FFFF). */
static bool anchor_name_ok(oyl_str name) {
    if (!name.data || name.len == 0) return false;
    for (size_t i = 0; i < name.len; i++) {
        uint8_t c = (uint8_t)name.data[i];
        uint32_t code;
        if (c <= ' ' || c == 0x7F || c == ',' || c == '[' || c == ']' || c == '{' || c == '}')
            return false;
        if ((scalar_class[c] & SC_UNI) && escaped_char(name.data, name.len, i, &code) &&
            code != 0x85 && code != 0x2028 && code != 0x2029)
            return false;
    }
    return true;
}

static bool has_anchor(oyl_str anchor) {
    return anchor.data && anchor.len;
}

static oyl_status emit_anchor(oyl_emitter *e, oyl_str anchor) {
    if (!has_anchor(anchor)) return OYL_OK;
    oyl_status st = buf_put(e, '&');
    if (st != OYL_OK) return st;
    st = buf_puts(e, anchor.data, anchor.len);
    if (st != OYL_OK) return st;
    return buf_put(e, ' ');
}

/* Characters allowed in a shorthand tag's suffix: URI characters other
 * than '!' and the flow indicators */
static bool is_tag_char(uint8_t c) {
    return oyl_is_uri_char(c) && c != '!' && !oyl_is_flow(c);
}

static bool all_tag_chars(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (!is_tag_char((uint8_t)s[i])) return false;
    return true;
}

/* Does a %TAG prefix's byte need a %-escape: one that isn't a URI
 * character, and first a flow indicator or '#', which would start a
 * comment */
static bool prefix_escaped(const char *s, size_t i) {
    uint8_t c = (uint8_t)s[i];
    return !oyl_is_uri_char(c) || (i == 0 && (oyl_is_flow(c) || c == '#'));
}

/* Write text as URI characters, any other byte %-escaped (a prefix: a
 * flow indicator or '#' first too); the parser decodes them */
static oyl_status emit_uri(oyl_emitter *e, const char *s, size_t len, bool prefix) {
    oyl_status st = OYL_OK;
    for (size_t i = 0; i < len && st == OYL_OK; i++) {
        if (prefix ? !prefix_escaped(s, i) : oyl_is_uri_char((uint8_t)s[i])) {
            st = buf_put(e, s[i]);
        } else {
            char esc[4];
            snprintf(esc, sizeof esc, "%%%02X", (uint8_t)s[i]);
            st = buf_puts(e, esc, 3);
        }
    }
    return st;
}

/* The handle to write a tag with, and how much of the tag it stands for
 * (*skip): the document's %TAG handle with the longest prefix of the tag,
 * else !! for a standard tag and ! for a local one, unless the document's
 * directives give that handle another prefix; each followed by tag
 * characters. NULL: the tag can't be written short. */
static const char *tag_handle(const oyl_emitter *e, oyl_str tag, size_t *skip) {
    static const char std[] = "tag:yaml.org,2002:";
    static const size_t slen = sizeof(std) - 1;
    const char *handle = NULL;
    bool std_free = true, local_free = true;
    for (int i = 0; i < e->n_tag_dirs; i++) {
        size_t plen = e->tag_dirs[i].plen;
        if (e->tag_dirs[i].hlen == 2) std_free = false;
        if (e->tag_dirs[i].hlen == 1) local_free = false;
        if (tag.len > plen && (!handle || plen > *skip) &&
            memcmp(tag.data, e->tag_dirs[i].prefix, plen) == 0 &&
            all_tag_chars(tag.data + plen, tag.len - plen)) {
            handle = e->tag_dirs[i].handle;
            *skip = plen;
        }
    }
    if (handle) return handle;
    if (std_free && tag.len > slen && memcmp(tag.data, std, slen) == 0 &&
        all_tag_chars(tag.data + slen, tag.len - slen)) {
        *skip = slen;
        return "!!";
    }
    if (local_free && tag.data[0] == '!' && all_tag_chars(tag.data + 1, tag.len - 1)) {
        *skip = 1;
        return "!";
    }
    return NULL;
}

/* Write a tag as a handle and a suffix ("!!str", "!foo", "!e!foo"), or
 * verbatim "!<...>" with any other byte percent-encoded; the parser
 * decodes verbatim tags, so each form reads back as the same tag. A lone
 * "!" is the non-specific tag, whatever the handles. */
static oyl_status emit_tag(oyl_emitter *e, oyl_str tag) {
    if (!tag.data || tag.len == 0) return OYL_OK;
    oyl_status st;
    size_t skip = 1;
    const char *handle = tag.len == 1 && tag.data[0] == '!' ? "!" : tag_handle(e, tag, &skip);
    if (handle) {
        st = buf_puts(e, handle, strlen(handle));
        if (st != OYL_OK) return st;
        if (tag.len > skip) st = buf_puts(e, tag.data + skip, tag.len - skip);
    } else {
        st = PUTS(e, "!<");
        if (st != OYL_OK) return st;
        st = emit_uri(e, tag.data, tag.len, false);
        if (st != OYL_OK) return st;
        st = buf_put(e, '>');
    }
    if (st != OYL_OK) return st;
    return buf_put(e, ' ');
}

static bool has_props(oyl_str anchor, oyl_str tag) {
    return (anchor.data && anchor.len) || (tag.data && tag.len);
}

static oyl_status emit_props(oyl_emitter *e, oyl_str anchor, oyl_str tag) {
    e->wrote_props = has_props(anchor, tag);
    if (!e->wrote_props) return OYL_OK;
    oyl_status st = emit_anchor(e, anchor);
    if (st != OYL_OK) return st;
    return emit_tag(e, tag);
}

/* Properties of a block collection go on the line that introduces it
 * ("key: &a !!map", "- &a"), and the collection itself starts on the next
 * line; a block collection can never start on its properties' line. */
static oyl_status emit_block_collection_props(oyl_emitter *e, oyl_str anchor, oyl_str tag,
                                              bool after_key) {
    oyl_status st;
    if (!has_props(anchor, tag)) return OYL_OK;
    if (after_key) {
        st = buf_put(e, ' ');
        if (st != OYL_OK) return st;
    }
    st = emit_props(e, anchor, tag);
    if (st != OYL_OK) return st;
    if (e->len > 0 && e->buf[e->len - 1] == ' ') e->len--; /* trailing space */
    return OYL_OK;
}

/* A block collection as a mapping key must use the explicit form:
 *   ? <props>
 *     <collection>
 *   : <value>
 * Writes "?" and the key's properties; the collection follows on the next
 * lines, and the value's ':' goes on its own line (see emit_pre_node). */
static oyl_status emit_complex_key_open(oyl_emitter *e, emit_ctx *map,
                                        oyl_str anchor, oyl_str tag) {
    oyl_status st = buf_put(e, '?');
    if (st != OYL_OK) return st;
    st = emit_block_collection_props(e, anchor, tag, true);
    if (st != OYL_OK) return st;
    map->complex_key = true;
    e->after_seq_dash = false;
    return OYL_OK;
}

/* ── Pre-node prefix ─────────────────────────────────────── */

/* Is the block mapping key just written too long to be implicit? YAML
 * allows at most 1024 characters, its properties and the space before
 * ':' included. */
static bool key_too_long(const oyl_emitter *e) {
    size_t n = e->len - e->key_start + e->key_was_alias;
    if (n <= 1024) return false;
    size_t chars = e->key_was_alias;
    for (size_t i = e->key_start; i < e->len; i++)
        chars += ((uint8_t)e->buf[i] & 0xC0) != 0x80;   /* not a UTF-8 continuation byte */
    return chars > 1024;
}

/* Make the key just written explicit ("? key"), so that its value goes on
 * the next line */
static oyl_status make_key_explicit(oyl_emitter *e, emit_ctx *map) {
    oyl_status st = buf_ensure(e, 2);
    if (st != OYL_OK) return st;
    memmove(e->buf + e->key_start + 2, e->buf + e->key_start, e->len - e->key_start);
    memcpy(e->buf + e->key_start, "? ", 2);
    e->len += 2;
    map->complex_key = true;
    e->key_was_alias = false;   /* ':' starts its own line */
    return OYL_OK;
}

static oyl_status emit_pre_node(oyl_emitter *e, bool is_collection) {
    emit_ctx *ctx = top_ctx(e);
    oyl_status st;

    if (!ctx) {
        /* root level — newline between documents */
        if (e->len > 0 && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, '\n');
            if (st != OYL_OK) return st;
        }
        return OYL_OK;
    }

    switch (ctx->type) {
    case EMIT_CTX_BLOCK_MAP:
        if (ctx->expect_key) {
            /* newline + indent before key (skip for first key after "- " or at doc start) */
            if (e->after_seq_dash) {
                /* compact mapping in sequence: first key shares "- " line */
            } else if (e->len > 0 && e->buf[e->len - 1] != '\n') {
                st = buf_put(e, '\n');
                if (st != OYL_OK) return st;
                st = buf_indent(e, ctx->indent);
                if (st != OYL_OK) return st;
            } else if (e->len > 0) {
                /* after a newline, just indent */
                st = buf_indent(e, ctx->indent);
                if (st != OYL_OK) return st;
            }
            e->key_start = e->len;
        } else {
            if (!ctx->complex_key && key_too_long(e)) {
                st = make_key_explicit(e, ctx);
                if (st != OYL_OK) return st;
            }
            /* after an explicit "? key", the value indicator starts its own
             * line at the mapping's indentation */
            if (ctx->complex_key) {
                if (e->len > 0 && e->buf[e->len - 1] != '\n') {
                    st = buf_put(e, '\n');
                    if (st != OYL_OK) return st;
                }
                st = buf_indent(e, ctx->indent);
                if (st != OYL_OK) return st;
                ctx->complex_key = false;
            }
            /* ":" before a block collection value (its properties and the
             * line break follow, see block_collection_open); ": " otherwise */
            /* after an alias key, ':' needs a space: it could otherwise be
             * read as part of the alias name ("*a : v") */
            if (e->key_was_alias) {
                st = buf_put(e, ' ');
                if (st != OYL_OK) return st;
                e->key_was_alias = false;
            }
            if (is_collection) {
                st = PUTS(e, ":");
                if (st != OYL_OK) return st;
            } else {
                st = PUTS(e, ": ");
                if (st != OYL_OK) return st;
            }
            e->wrote_block_key = false;
        }
        break;

    case EMIT_CTX_BLOCK_SEQ:
        /* newline + indent + "- " (no newline when already at a line
         * start, e.g. after a block scalar, which ends with one) */
        if (e->len > 0 && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, '\n');
            if (st != OYL_OK) return st;
        }
        st = buf_indent(e, ctx->indent);
        if (st != OYL_OK) return st;
        st = PUTS(e, "- ");
        if (st != OYL_OK) return st;
        e->after_seq_dash = true;
        break;

    case EMIT_CTX_FLOW_MAP:
        if (ctx->expect_key) {
            if (ctx->count > 0) {
                st = buf_puts(e, entry_sep(e), (size_t)entry_sep_len(e));
                if (st != OYL_OK) return st;
            }
        } else {
            if (e->key_was_alias) {
                st = buf_put(e, ' ');
                if (st != OYL_OK) return st;
                e->key_was_alias = false;
            }
            st = buf_puts(e, kv_sep(e), (size_t)kv_sep_len(e));
            if (st != OYL_OK) return st;
        }
        break;

    case EMIT_CTX_FLOW_SEQ:
        if (ctx->count > 0) {
            st = buf_puts(e, entry_sep(e), (size_t)entry_sep_len(e));
            if (st != OYL_OK) return st;
        }
        break;
    }

    return OYL_OK;
}

/* Advance the parent context after emitting a node */
static void advance_ctx(oyl_emitter *e) {
    emit_ctx *ctx = top_ctx(e);
    if (!ctx) {
        e->root_done = true;
        return;
    }

    if (ctx->type == EMIT_CTX_BLOCK_MAP || ctx->type == EMIT_CTX_FLOW_MAP) {
        if (ctx->expect_key) {
            ctx->expect_key = false;
            e->wrote_block_key = (ctx->type == EMIT_CTX_BLOCK_MAP);
        } else {
            ctx->expect_key = true;
            ctx->count++;
            e->wrote_block_key = false;
        }
    } else {
        /* sequence */
        ctx->count++;
    }
    e->after_seq_dash = false;
}

/* ── Nodes ───────────────────────────────────────────────── */

/* One function per kind of event, taking its fields: oyl_emit, the
 * building functions below and oyl_emit_tree all come here */

/* Is a node allowed next: the document's root, or an entry of the
 * collection open? */
static bool node_ok(const oyl_emitter *e) {
    return e->stack_len > 0 || (e->doc_open && !e->root_done);
}

/* Is a directive allowed next: between documents */
static bool directive_ok(const oyl_emitter *e) {
    return e->stream_started && !e->stream_ended && !e->doc_open;
}

static oyl_status emit_yaml_directive(oyl_emitter *e, int major, int minor) {
    if (!directive_ok(e) || e->has_version || major != 1 || minor < 0)
        return emit_fail(e, OYL_ERR_EMIT);
    e->has_version = true;
    e->version_minor = minor;
    return OYL_OK;
}

static int tag_directive_index(const oyl_emitter *e, oyl_str handle) {
    for (int i = 0; i < e->n_tag_dirs; i++)
        if (e->tag_dirs[i].hlen == handle.len && memcmp(e->tag_dirs[i].handle, handle.data, handle.len) == 0)
            return i;
    return -1;
}

static oyl_status emit_tag_directive(oyl_emitter *e, oyl_str handle, oyl_str prefix) {
    /* as long as the parser reads: 15 bytes of handle, 255 of prefix */
    size_t written = prefix.len;
    for (size_t i = 0; prefix.data && i < prefix.len; i++)
        if (prefix_escaped(prefix.data, i)) written += 2;
    if (!directive_ok(e) || e->n_tag_dirs == 8 || !handle.data || !prefix.data ||
        !oyl_tag_handle_ok(handle.data, handle.len) || handle.len >= 16 ||
        prefix.len == 0 || written >= 256 || tag_directive_index(e, handle) >= 0)
        return emit_fail(e, OYL_ERR_EMIT);
    int i = e->n_tag_dirs++;
    memcpy(e->tag_dirs[i].handle, handle.data, handle.len);
    e->tag_dirs[i].handle[handle.len] = '\0';   /* emit_tag() writes it as a string */
    e->tag_dirs[i].hlen = (uint8_t)handle.len;
    memcpy(e->tag_dirs[i].prefix, prefix.data, prefix.len);
    e->tag_dirs[i].plen = (uint16_t)prefix.len;
    return OYL_OK;
}

bool oyl_emitter_has_directive(const oyl_emitter *e, oyl_str handle) {
    return handle.data ? tag_directive_index(e, handle) >= 0 : e->has_version;
}

/* The document's directives, before its "---": after a document that
 * ended without "...", that first, as YAML has it */
static oyl_status emit_directives(oyl_emitter *e) {
    oyl_status st = OYL_OK;
    if (e->open_ended) {
        if (e->len > 0 && e->buf[e->len - 1] != '\n') st = buf_put(e, '\n');
        if (st == OYL_OK) st = PUTS(e, "...\n");
    }
    if (st == OYL_OK && e->has_version) {
        char line[32];
        int n = snprintf(line, sizeof line, "%%YAML 1.%d\n", e->version_minor);
        st = buf_puts(e, line, (size_t)n);
    }
    for (int i = 0; i < e->n_tag_dirs && st == OYL_OK; i++) {
        st = PUTS(e, "%TAG ");
        if (st == OYL_OK) st = buf_puts(e, e->tag_dirs[i].handle, e->tag_dirs[i].hlen);
        if (st == OYL_OK) st = buf_put(e, ' ');
        if (st == OYL_OK) st = emit_uri(e, e->tag_dirs[i].prefix, e->tag_dirs[i].plen, true);
        if (st == OYL_OK) st = buf_put(e, '\n');
    }
    return st;
}

static oyl_status emit_document_start(oyl_emitter *e, bool implicit) {
    oyl_status st;
    if (!e->stream_started || e->stream_ended || e->doc_open)
        return emit_fail(e, OYL_ERR_EMIT);
    if (e->has_version || e->n_tag_dirs) {
        st = emit_directives(e);
        if (st != OYL_OK) return st;
        implicit = false;
    }
    /* a document without "---" can only start the stream or follow "..." */
    if (e->open_ended) implicit = false;
    if (!implicit) {
        if (!e->first_doc && e->len > 0 && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, '\n');
            if (st != OYL_OK) return st;
        }
        st = PUTS(e, "---");
        if (st != OYL_OK) return st;
    }
    e->doc_open = true;
    e->root_done = false;
    e->bare_doc = implicit;
    e->first_doc = false;
    return OYL_OK;
}

static oyl_status emit_document_end(oyl_emitter *e, bool implicit) {
    oyl_status st;
    if (!e->doc_open || !e->root_done || e->stack_len)
        return emit_fail(e, OYL_ERR_EMIT);
    if (!implicit) {
        if (e->len > 0 && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, '\n');
            if (st != OYL_OK) return st;
        }
        st = PUTS(e, "...\n");
        if (st != OYL_OK) return st;
    }
    e->doc_open = false;
    e->open_ended = implicit;
    e->has_version = false;
    e->n_tag_dirs = 0;
    return OYL_OK;
}

static oyl_status emit_mapping_start(oyl_emitter *e, oyl_str anchor, oyl_str tag, bool flow) {
    oyl_status st;
    if (!node_ok(e) || (has_anchor(anchor) && !anchor_name_ok(anchor)))
        return emit_fail(e, OYL_ERR_EMIT);
    /* inside a flow collection everything must be flow */
    bool use_flow = (e->opts.style != OYL_EMIT_BLOCK) || flow || in_any_flow(e);
    emit_ctx *outer = top_ctx(e);
    bool map_value = !use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP &&
                     !outer->expect_key;

    st = emit_pre_node(e, !use_flow);
    if (st != OYL_OK) return st;

    if (!use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP && outer->expect_key) {
        st = emit_complex_key_open(e, outer, anchor, tag);
    } else if (use_flow) {
        st = emit_props(e, anchor, tag);
    } else {
        st = emit_block_collection_props(e, anchor, tag, map_value);
        /* a mapping with properties in a sequence can't share the
         * "- &a" line with its first key (the props would go to the key) */
        if (st == OYL_OK && e->after_seq_dash && has_props(anchor, tag))
            e->after_seq_dash = false;
    }
    if (st != OYL_OK) return st;

    if (use_flow) {
        st = buf_put(e, '{');
        if (st != OYL_OK) return st;
        st = push_ctx(e, EMIT_CTX_FLOW_MAP, 0);
    } else {
        emit_ctx *parent = top_ctx(e);
        int indent;
        if (parent && parent->type == EMIT_CTX_BLOCK_SEQ) {
            /* compact mapping in sequence: indent aligns with content after "- " */
            indent = parent->indent + 2;
        } else if (parent && (parent->type == EMIT_CTX_BLOCK_MAP)) {
            indent = parent->indent + e->opts.indent;
        } else {
            indent = 0;
        }
        st = push_ctx(e, EMIT_CTX_BLOCK_MAP, indent);
    }
    if (st != OYL_OK) return st;
    /* advance parent context, preserving after_seq_dash for compact mapping */
    bool saved_dash = e->after_seq_dash;
    e->stack_len--;
    advance_ctx(e);
    e->stack_len++;
    e->after_seq_dash = saved_dash;
    return OYL_OK;
}

static oyl_status emit_mapping_end(oyl_emitter *e) {
    oyl_status st;
    emit_ctx *ctx = top_ctx(e);
    /* a mapping ends after a value, not a key */
    if (!ctx || (ctx->type != EMIT_CTX_BLOCK_MAP && ctx->type != EMIT_CTX_FLOW_MAP) ||
        !ctx->expect_key)
        return emit_fail(e, OYL_ERR_EMIT);

    if (ctx->type == EMIT_CTX_FLOW_MAP) {
        st = buf_put(e, '}');
        if (st != OYL_OK) return st;
    } else if (ctx->count == 0) {
        /* empty block mapping → {} ("k: {}", "? {}") */
        if (e->len > 0 && e->buf[e->len - 1] != ' ' && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, ' ');
            if (st != OYL_OK) return st;
        }
        st = PUTS(e, "{}");
        if (st != OYL_OK) return st;
    }
    pop_ctx(e);
    e->after_seq_dash = false;
    return OYL_OK;
}

static oyl_status emit_sequence_start(oyl_emitter *e, oyl_str anchor, oyl_str tag, bool flow) {
    oyl_status st;
    if (!node_ok(e) || (has_anchor(anchor) && !anchor_name_ok(anchor)))
        return emit_fail(e, OYL_ERR_EMIT);
    /* inside a flow collection everything must be flow */
    bool use_flow = (e->opts.style != OYL_EMIT_BLOCK) || flow || in_any_flow(e);
    emit_ctx *outer = top_ctx(e);
    bool map_value = !use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP &&
                     !outer->expect_key;

    st = emit_pre_node(e, !use_flow);
    if (st != OYL_OK) return st;

    if (!use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP && outer->expect_key)
        st = emit_complex_key_open(e, outer, anchor, tag);
    else
        st = use_flow ? emit_props(e, anchor, tag)
                      : emit_block_collection_props(e, anchor, tag, map_value);
    if (st != OYL_OK) return st;

    if (use_flow) {
        st = buf_put(e, '[');
        if (st != OYL_OK) return st;
        st = push_ctx(e, EMIT_CTX_FLOW_SEQ, 0);
    } else {
        emit_ctx *parent = top_ctx(e);
        int indent;
        if (parent && parent->type == EMIT_CTX_BLOCK_MAP) {
            indent = parent->indent + e->opts.indent;
        } else if (parent && parent->type == EMIT_CTX_BLOCK_SEQ) {
            indent = parent->indent + e->opts.indent;
        } else {
            indent = 0;
        }
        st = push_ctx(e, EMIT_CTX_BLOCK_SEQ, indent);
    }
    if (st != OYL_OK) return st;
    /* advance parent context, preserving after_seq_dash */
    bool saved_dash = e->after_seq_dash;
    e->stack_len--;
    advance_ctx(e);
    e->stack_len++;
    e->after_seq_dash = saved_dash;
    return OYL_OK;
}

static oyl_status emit_sequence_end(oyl_emitter *e) {
    oyl_status st;
    emit_ctx *ctx = top_ctx(e);
    if (!ctx || (ctx->type != EMIT_CTX_BLOCK_SEQ && ctx->type != EMIT_CTX_FLOW_SEQ))
        return emit_fail(e, OYL_ERR_EMIT);

    if (ctx->type == EMIT_CTX_FLOW_SEQ) {
        st = buf_put(e, ']');
        if (st != OYL_OK) return st;
    } else if (ctx->count == 0) {
        /* empty block sequence → [] ("k: []", "? []") */
        if (e->len > 0 && e->buf[e->len - 1] != ' ' && e->buf[e->len - 1] != '\n') {
            st = buf_put(e, ' ');
            if (st != OYL_OK) return st;
        }
        st = PUTS(e, "[]");
        if (st != OYL_OK) return st;
    }
    pop_ctx(e);
    e->after_seq_dash = false;
    return OYL_OK;
}

static oyl_status emit_scalar_node(oyl_emitter *e, oyl_str value, oyl_scalar_style style,
                                   oyl_str anchor, oyl_str tag) {
    if (!node_ok(e) || (has_anchor(anchor) && !anchor_name_ok(anchor)))
        return emit_fail(e, OYL_ERR_EMIT);
    if (!value.data && value.len) return emit_fail(e, OYL_ERR_EMIT);
    oyl_status st = emit_pre_node(e, false);
    if (st != OYL_OK) return st;

    st = emit_props(e, anchor, tag);
    if (st != OYL_OK) return st;

    st = emit_scalar(e, value, style, tag);
    if (st != OYL_OK) return st;

    advance_ctx(e);
    return OYL_OK;
}

static oyl_status emit_alias_node(oyl_emitter *e, oyl_str name) {
    if (!node_ok(e) || !anchor_name_ok(name)) return emit_fail(e, OYL_ERR_EMIT);
    oyl_status st = emit_pre_node(e, false);
    if (st != OYL_OK) return st;
    emit_ctx *ctx = top_ctx(e);
    e->key_was_alias = ctx && (ctx->type == EMIT_CTX_BLOCK_MAP ||
                               ctx->type == EMIT_CTX_FLOW_MAP) && ctx->expect_key;

    st = buf_put(e, '*');
    if (st != OYL_OK) return st;
    st = buf_puts(e, name.data, name.len);
    if (st != OYL_OK) return st;

    advance_ctx(e);
    return OYL_OK;
}

static oyl_status emit_stream_start(oyl_emitter *e) {
    if (e->stream_started) return emit_fail(e, OYL_ERR_EMIT);
    e->stream_started = true;
    e->first_doc = true;
    return OYL_OK;
}

static oyl_status emit_stream_end(oyl_emitter *e) {
    /* directives must have a document */
    if (!e->stream_started || e->stream_ended || e->doc_open || e->has_version || e->n_tag_dirs)
        return emit_fail(e, OYL_ERR_EMIT);
    e->stream_ended = true;
    /* ensure trailing newline */
    if (e->len > 0 && e->buf[e->len - 1] != '\n')
        return buf_put(e, '\n');
    return OYL_OK;
}

/* ── Main emit function ──────────────────────────────────── */

static oyl_status emit_event(oyl_emitter *e, const oyl_event *evt) {
    switch (evt->type) {
    case OYL_EVT_STREAM_START:   return emit_stream_start(e);
    case OYL_EVT_STREAM_END:     return emit_stream_end(e);

    case OYL_EVT_DOC_START:      return emit_document_start(e, evt->implicit);
    case OYL_EVT_DOC_END:        return emit_document_end(e, evt->implicit);
    case OYL_EVT_MAPPING_START:  return emit_mapping_start(e, evt->anchor, evt->tag, evt->flow);
    case OYL_EVT_MAPPING_END:    return emit_mapping_end(e);
    case OYL_EVT_SEQUENCE_START: return emit_sequence_start(e, evt->anchor, evt->tag, evt->flow);
    case OYL_EVT_SEQUENCE_END:   return emit_sequence_end(e);
    case OYL_EVT_SCALAR:
        return emit_scalar_node(e, evt->value, evt->scalar_style, evt->anchor, evt->tag);
    case OYL_EVT_ALIAS:          return emit_alias_node(e, evt->value);
    default:
        return emit_fail(e, OYL_ERR_EMIT);
    }
}

oyl_status oyl_emit(oyl_emitter *e, const oyl_event *evt) {
    return emit_event(e, evt);
}

/* ── Constructor / destructor ────────────────────────────── */

oyl_emitter *oyl_emitter_new(oyl_arena *a) {
    oyl_emitter *e = malloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));

    e->opts = (emit_opts){ OYL_EMIT_BLOCK, 2 };
    e->arena = a;

    e->cap = EMIT_INIT_CAP;
    e->buf = malloc(e->cap);
    if (!e->buf) { free(e); return NULL; }

    e->stack_cap = 8;
    e->stack = malloc((size_t)e->stack_cap * sizeof(emit_ctx));
    if (!e->stack) { free(e->buf); free(e); return NULL; }

    e->first_doc = true;
    return e;
}

void oyl_emitter_set_style(oyl_emitter *e, oyl_emit_style style) {
    if (style == OYL_EMIT_BLOCK || style == OYL_EMIT_FLOW || style == OYL_EMIT_MINIMAL)
        e->opts.style = style;
}

void oyl_emitter_set_indent(oyl_emitter *e, int indent) {
    if (indent < 1) indent = 1;
    if (indent > 10) indent = 10;
    e->opts.indent = indent;
}

/* ── Event constructors ──────────────────────────────────── */

oyl_status oyl_emit_stream_start(oyl_emitter *e) {
    return emit_stream_start(e);
}

oyl_status oyl_emit_stream_end(oyl_emitter *e) {
    return emit_stream_end(e);
}

oyl_status oyl_emit_yaml_directive(oyl_emitter *e, int major, int minor) {
    return emit_yaml_directive(e, major, minor);
}

oyl_status oyl_emit_tag_directive(oyl_emitter *e, oyl_str handle, oyl_str prefix) {
    return emit_tag_directive(e, handle, prefix);
}

oyl_status oyl_emit_document_start(oyl_emitter *e, bool implicit) {
    return emit_document_start(e, implicit);
}

oyl_status oyl_emit_document_end(oyl_emitter *e, bool implicit) {
    return emit_document_end(e, implicit);
}

oyl_status oyl_emit_scalar(oyl_emitter *e, oyl_str value, oyl_scalar_style style,
                           oyl_str anchor, oyl_str tag) {
    return emit_scalar_node(e, value, style, anchor, tag);
}

oyl_status oyl_emit_alias(oyl_emitter *e, oyl_str name) {
    return emit_alias_node(e, name);
}

oyl_status oyl_emit_mapping_start(oyl_emitter *e, oyl_str anchor, oyl_str tag, bool flow) {
    return emit_mapping_start(e, anchor, tag, flow);
}

oyl_status oyl_emit_mapping_end(oyl_emitter *e) {
    return emit_mapping_end(e);
}

oyl_status oyl_emit_sequence_start(oyl_emitter *e, oyl_str anchor, oyl_str tag, bool flow) {
    return emit_sequence_start(e, anchor, tag, flow);
}

oyl_status oyl_emit_sequence_end(oyl_emitter *e) {
    return emit_sequence_end(e);
}

oyl_str oyl_emitter_output(oyl_emitter *e) {
    if (!e || !e->buf) return OYL_STR_NULL;
    if (e->len == 0) return (oyl_str){ "", 0 };   /* NULL means out of memory */
    char *copy = oyl_arena_dup(e->arena, e->buf, e->len);
    if (!copy) return OYL_STR_NULL;
    return (oyl_str){copy, e->len};
}

void oyl_emitter_free(oyl_emitter *e) {
    if (!e) return;
    free(e->buf);
    free(e->stack);
    free(e);
}
