/*
 * oyl_json.c — YAML documents written as JSON (oyl_parse_json)
 *
 * sigs.k8s.io/yaml's YAMLToJSON lets go-yaml v2 decode the YAML into Go
 * values (merging, expanding aliases), turns the keys into strings, and
 * has Go's encoding/json write the result, which sorts the keys and
 * formats numbers and strings its own way. To give the same bytes, a
 * document is read into a tree first (oyl_tree.c), so that a mapping's
 * keys can be sorted and its merges and duplicates settled before it is
 * written. The tree typed its scalars while building; with another
 * schema, they're typed here. First, every node is checked as decoding
 * checks it (go-yaml v2 and PyYAML decode even the values of keys set
 * again later), and in go-yaml v2's merge order the document is walked as
 * go-yaml v2 decodes it, to count aliasing as it does. In YAML's merge,
 * each mapping merged, and each sequence of them merged through an
 * alias, is worked out once and its entries kept; go-yaml v2's merge
 * order is followed step by step.
 */

#include "oyl_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#  include <emmintrin.h>
#endif

static bool same(oyl_str a, oyl_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

/* ── Growable buffers ────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len, cap;
} buffer;

NOINLINE bool buf_grow(buffer *b, size_t more) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap - b->len < more) {
        if (cap > SIZE_MAX / 2) return false;
        cap *= 2;
    }
    char *data = realloc(b->data, cap);
    if (!data) return false;
    b->data = data;
    b->cap = cap;
    return true;
}

static inline bool buf_reserve(buffer *b, size_t more) {
    return more <= b->cap - b->len || buf_grow(b, more);
}

static bool buf_put(buffer *b, const void *s, size_t n) {
    if (!buf_reserve(b, n)) return false;
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    return true;
}

static bool buf_putc(buffer *b, char c) { return buf_put(b, &c, 1); }
static bool buf_puts(buffer *b, const char *s) { return buf_put(b, s, strlen(s)); }

/* An integer in decimal, without snprintf */
static bool buf_uint(buffer *b, uint64_t u, bool neg) {
    char digits[24];
    int n = sizeof digits;
    do digits[--n] = (char)('0' + u % 10); while ((u /= 10) != 0);
    if (neg) digits[--n] = '-';
    return buf_put(b, digits + n, sizeof digits - (size_t)n);
}

static bool buf_int(buffer *b, int64_t i) {
    return buf_uint(b, i < 0 ? (uint64_t)0 - (uint64_t)i : (uint64_t)i, i < 0);
}

/* ── The document ────────────────────────────────────────── */

/* A mapping entry while its mapping is settled: the highest priority wins */
typedef struct {
    union {
        const char *ptr;          /* a text key: in the document */
        size_t      off;          /* a formatted or decoded key: in doc.keys or doc.memo_keys */
    } key_at;
    uint64_t prio;
    uint32_t key_len;
    uint32_t key, value;          /* nodes */
    unsigned in_keys : 2;         /* key_at is an offset: 1 into doc.keys, 2 doc.memo_keys */
    unsigned bad : 2;             /* a key JSON can't hold, failing if written: 1 null, 2 uint64 */
    unsigned zero : 1;            /* a float key 0 or -0: one key to Go (==), another than
                                     the int 0 or the string "0"; written "0" or "-0" */
    unsigned written : 1;         /* in the mapping, not merged into it */
    unsigned aliased : 1;         /* merged through an alias: its value counts as aliased */
} entry;

typedef struct {
    oyl_parser        *p;
    oyl_tree          *t;         /* the document (oyl_tree.c) */
    const oyl_node    *n;         /* its nodes */
    const oyl_schema  *schema;
    bool               typed;     /* the tree's types are this schema's */
    unsigned          flags;
    buffer            out;
    buffer            keys;
    buffer            bin;       /* a decoded !!binary */
    entry            *entries;   /* a stack: each mapping settles its own range */
    size_t            elen, ecap;
    entry            *sort_tmp;  /* merge sort's buffer, reused */
    size_t            tmp_cap;
    uint32_t         *pending;   /* a stack: YAML's merges waiting in each mapping collected */
    size_t            plen, pcap;
    size_t            decodes, aliased;   /* go-yaml v2's aliasing count */
    int               alias_depth, depth, max_depth;
    bool              quiet;     /* not counted in it: checking, working out YAML's
                                    merge, or writing once counted (count_node) */
    bool              prepared;  /* prepare_merges has run */
    /* YAML's merge: each mapping merged, with its own merges applied and
     * settled, worked out once (merged_entries) */
    entry            *memo;
    size_t            mlen, mcap;
    struct span { size_t off, n; } *spans;   /* each mapping's entries in memo */
    uint32_t          nspans, spans_cap;
    oyl_node_map      memo_at;   /* by mapping: its span */
    buffer            memo_keys; /* the formatted and decoded keys of entries in memo */
    size_t            merge_budget;   /* the entries merges may still copy */
    oyl_status        status;    /* the first error */
} doc;

static oyl_mark node_mark(const doc *d, uint32_t i) { return oyl_node_mark(d->t, i); }

static bool fail(doc *d, oyl_status status, oyl_mark mark, const char *fmt, ...) {
    if (d->status == OYL_OK) {
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        oyl_parser_fail(d->p, status, mark, msg);
        d->status = status;
    }
    return false;
}

static bool out_of_memory(doc *d) {
    return fail(d, OYL_ERR_MEMORY, (oyl_mark){ 0, 0, 0 }, "out of memory");
}

/* How much of a text a message shows, for "%.*s": an int can't hold every
 * length, and a message needn't */
static int shown(size_t len) { return len > 100 ? 100 : (int)len; }

/* A tag as go-yaml writes it in messages: !!int for tag:yaml.org,2002:int */
static const char *short_tag(oyl_str tag, int *len) {
    static const char prefix[] = "tag:yaml.org,2002:";
    size_t plen = sizeof prefix - 1;
    if (tag.len > plen && memcmp(tag.data, prefix, plen) == 0) {
        *len = shown(tag.len - plen);
        return tag.data + plen;
    }
    *len = shown(tag.len);
    return tag.data;
}

/* A node's text, and the tag written on it */
static inline oyl_str text(const doc *d, uint32_t i) { return oyl_tree_text(d->t, &d->n[i]); }

static inline oyl_str written_tag(const doc *d, uint32_t i) {
    const oyl_node *n = &d->n[i];
    if (n->kind == OYL_NODE_ALIAS || !n->props) return OYL_STR_NULL;
    const oyl_node_props *pr = &d->t->props[n->props];
    return pr->tag_len ? (oyl_str){ d->t->pool + pr->tag, pr->tag_len } : OYL_STR_NULL;
}

/* Is alias i inside the node it refers to? */
static bool contains_itself(const doc *d, uint32_t i) {
    uint32_t to = d->n[i].props;
    return to < i && i < to + d->n[to].span;
}

/* ── Go's number formatting ──────────────────────────────── */

/* Does d1.d2...dn × 10^e read back as `a` (or as its float32)? Written
 * d1d2...dn × 10^(e - n + 1), with no decimal point, which would be the
 * locale's */
static bool reads_back(const char *digits, int n, int e, double a, bool single) {
    char s[48];
    memcpy(s, digits, (size_t)n);
    snprintf(s + n, sizeof s - (size_t)n, "e%d", e - n + 1);
    return single ? strtof(s, NULL) == (float)a : strtod(s, NULL) == a;
}

/* The n-digit decimal next to digits × 10^e, up (+1) or down (-1) */
static void neighbor(const char *digits, int n, int dir, char *out, int *e) {
    memcpy(out, digits, (size_t)n);
    int i = n - 1;
    if (dir > 0) {
        while (i >= 0 && out[i] == '9') out[i--] = '0';
        if (i >= 0) {
            out[i]++;
        } else {   /* 99 -> 100: one more digit, so drop the last */
            out[0] = '1';
            memset(out + 1, '0', (size_t)n - 1);
            (*e)++;
        }
    } else {
        while (i >= 0 && out[i] == '0') out[i--] = '9';
        if (i >= 0) out[i]--;
        if (out[0] == '0') {   /* 100 -> 099: one digit fewer */
            memset(out, '9', (size_t)n);
            (*e)--;
        }
    }
}

/* The shortest decimal digits that read back as `a` (> 0, finite), as Go's
 * strconv finds them: the closest of that length. Returns their count;
 * `a` = d1.d2... × 10^*e. Under `single`, for a's float32. */
static int shortest_digits(double a, bool single, char *digits, int *e) {
    double x = single ? (double)(float)a : a;
    *e = 0;
    for (int n = 1; n <= 17; n++) {
        char buf[48];
        snprintf(buf, sizeof buf, "%.*e", n - 1, x);
        int k = 0, j = 0;
        while (buf[j] && buf[j] != 'e') {
            if (buf[j] >= '0' && buf[j] <= '9') digits[k++] = buf[j];
            j++;
        }
        int ex = buf[j] == 'e' ? atoi(buf + j + 1) : 0;
        *e = ex;
        if (reads_back(digits, n, ex, a, single)) return n;
        /* the nearest n digits can miss on the narrow side of the interval
         * that reads back (next to a power of two), where a neighbor fits */
        char up[24], down[24];
        int eu = ex, ed = ex;
        neighbor(digits, n, 1, up, &eu);
        neighbor(digits, n, -1, down, &ed);
        bool u = reads_back(up, n, eu, a, single);
        bool dn = reads_back(down, n, ed, a, single);
        if (u || dn) {
            bool take_up = u;
            if (u && dn) {   /* both: the closer */
                char s[48];
                memcpy(s, up, (size_t)n);
                snprintf(s + n, sizeof s - (size_t)n, "e%d", eu - n + 1);
                long double vu = strtold(s, NULL);
                memcpy(s, down, (size_t)n);
                snprintf(s + n, sizeof s - (size_t)n, "e%d", ed - n + 1);
                long double vd = strtold(s, NULL);
                take_up = fabsl(vu - x) < fabsl(vd - x);
            }
            memcpy(digits, take_up ? up : down, (size_t)n);
            *e = take_up ? eu : ed;
            return n;
        }
    }
    return 17;   /* not reached: 17 digits always read back */
}

/* digits × 10^e in plain notation, as Go's 'f' format writes it */
static bool put_fixed(buffer *b, const char *d, int n, int e) {
    bool ok = true;
    if (e >= 0) {
        if (n <= e + 1) {
            ok = buf_put(b, d, (size_t)n);
            for (int i = n; ok && i <= e; i++) ok = buf_putc(b, '0');
        } else {
            ok = buf_put(b, d, (size_t)e + 1) && buf_putc(b, '.') &&
                 buf_put(b, d + e + 1, (size_t)(n - e - 1));
        }
    } else {
        ok = buf_puts(b, "0.");
        for (int i = 0; ok && i < -e - 1; i++) ok = buf_putc(b, '0');
        ok = ok && buf_put(b, d, (size_t)n);
    }
    return ok;
}

/* d1[.d2...]e±XX, the exponent padded to two digits */
static bool put_exponent(buffer *b, const char *d, int n, int e, bool trim_negative) {
    char ex[16];
    int ae = e < 0 ? -e : e;
    if (trim_negative && e < 0 && ae < 10) snprintf(ex, sizeof ex, "e-%d", ae);
    else snprintf(ex, sizeof ex, "e%c%02d", e < 0 ? '-' : '+', ae);
    return buf_putc(b, d[0]) && (n == 1 || (buf_putc(b, '.') && buf_put(b, d + 1, (size_t)n - 1))) &&
           buf_puts(b, ex);
}

/* A float as encoding/json writes it: the shortest digits, in plain
 * notation from 1e-6 up to 1e21 and otherwise e notation, as ES6 does */
static bool put_json_float(buffer *b, double f) {
    if (f == 0) return buf_puts(b, signbit(f) ? "-0" : "0");
    char d[24];
    int e;
    double a = fabs(f);
    int n = shortest_digits(a, false, d, &e);
    if (f < 0 && !buf_putc(b, '-')) return false;
    if (a < 1e-6 || a >= 1e21) return put_exponent(b, d, n, e, true);
    return put_fixed(b, d, n, e);
}

/* A float key as sigs.k8s.io/yaml writes it: strconv.FormatFloat(f, 'g',
 * -1, 32), with Go's +Inf, -Inf and NaN as YAML's .inf, -.inf and .nan */
static bool put_float_key(buffer *b, double f) {
    float g = (float)f;
    if (isnan(g)) return buf_puts(b, ".nan");
    if (isinf(g)) return buf_puts(b, g > 0 ? ".inf" : "-.inf");
    if (g == 0) return buf_puts(b, signbit(g) ? "-0" : "0");
    char d[24];
    int e;
    int n = shortest_digits(fabs((double)g), true, d, &e);
    if (g < 0 && !buf_putc(b, '-')) return false;
    if (e < -4 || e >= 6) return put_exponent(b, d, n, e, false);
    return put_fixed(b, d, n, e);
}

/* ── Go's string escaping ────────────────────────────────── */

/* The length of the valid UTF-8 sequence at s, as Go's utf8.DecodeRune
 * accepts them (no overlong forms, surrogates or values past U+10FFFF), or
 * 0; its code point in *cp */
static int utf8_seq(const unsigned char *s, size_t n, uint32_t *cp) {
    unsigned char c = s[0];
    int len;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;
        if (c == 0xED) hi = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if (n < (size_t)len || s[1] < lo || s[1] > hi) return 0;
    for (int i = 2; i < len; i++)
        if (s[i] < 0x80 || s[i] > 0xBF) return 0;
    *cp = len == 2 ? (uint32_t)(c & 0x1F) << 6 | (s[1] & 0x3F)
        : len == 3 ? (uint32_t)(c & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F)
        : (uint32_t)(c & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 |
              (s[3] & 0x3F);
    return len;
}

/* The ASCII bytes a JSON string writes as they are */
static const bool json_plain[128] = {
    [' '] = 1, ['!'] = 1, ['#'] = 1, ['$'] = 1, ['%'] = 1, ['\''] = 1, ['('] = 1, [')'] = 1,
    ['*'] = 1, ['+'] = 1, [','] = 1, ['-'] = 1, ['.'] = 1, ['/'] = 1, ['0'] = 1, ['1'] = 1,
    ['2'] = 1, ['3'] = 1, ['4'] = 1, ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    [':'] = 1, [';'] = 1, ['='] = 1, ['?'] = 1, ['@'] = 1, ['A'] = 1, ['B'] = 1, ['C'] = 1,
    ['D'] = 1, ['E'] = 1, ['F'] = 1, ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1,
    ['L'] = 1, ['M'] = 1, ['N'] = 1, ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1, ['S'] = 1,
    ['T'] = 1, ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1, ['Z'] = 1, ['['] = 1,
    [']'] = 1, ['^'] = 1, ['_'] = 1, ['`'] = 1, ['a'] = 1, ['b'] = 1, ['c'] = 1, ['d'] = 1,
    ['e'] = 1, ['f'] = 1, ['g'] = 1, ['h'] = 1, ['i'] = 1, ['j'] = 1, ['k'] = 1, ['l'] = 1,
    ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1, ['q'] = 1, ['r'] = 1, ['s'] = 1, ['t'] = 1,
    ['u'] = 1, ['v'] = 1, ['w'] = 1, ['x'] = 1, ['y'] = 1, ['z'] = 1, ['{'] = 1, ['|'] = 1,
    ['}'] = 1, ['~'] = 1, [127] = 1,
};

/* A string as encoding/json writes it, HTML-safe: <, > and & escaped,
 * U+2028 and U+2029 too, and each invalid UTF-8 byte as U+FFFD. Go 1.27
 * writes that character itself; older versions wrote the escape \ufffd. */
static bool put_json_string(buffer *b, const char *str, size_t n) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *s = (const unsigned char *)str;
    if (!buf_reserve(b, n + 2) || !buf_putc(b, '"')) return false;
    size_t i = 0;
    while (i < n) {
        size_t run = i;
#if defined(__SSE2__)
        /* 16 bytes at a time while none needs escaping or is past ASCII
         * (a signed compare with 0x20 catches control characters and
         * bytes from 0x80 alike) */
        const __m128i space = _mm_set1_epi8(0x20), quote = _mm_set1_epi8('"'),
                      backslash = _mm_set1_epi8('\\'), lt = _mm_set1_epi8('<'),
                      gt = _mm_set1_epi8('>'), amp = _mm_set1_epi8('&');
        while (run + 16 <= n) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + run));
            __m128i hit = _mm_or_si128(_mm_or_si128(_mm_cmplt_epi8(v, space), _mm_cmpeq_epi8(v, quote)),
                                       _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, backslash),
                                                                 _mm_cmpeq_epi8(v, lt)),
                                                    _mm_or_si128(_mm_cmpeq_epi8(v, gt), _mm_cmpeq_epi8(v, amp))));
            int mask = _mm_movemask_epi8(hit);
            if (mask) {
                run += (size_t)__builtin_ctz((unsigned)mask);
                break;
            }
            run += 16;
        }
#endif
        while (run < n && s[run] < 0x80 && json_plain[s[run]]) run++;
        if (run > i && !buf_put(b, s + i, run - i)) return false;
        i = run;
        if (i >= n) break;
        unsigned char c = s[i];
        if (c < 0x80) {
            char esc[7] = { '\\', 0 };
            size_t elen = 2;
            switch (c) {
            case '"':  esc[1] = '"'; break;
            case '\\': esc[1] = '\\'; break;
            case '\n': esc[1] = 'n'; break;
            case '\r': esc[1] = 'r'; break;
            case '\t': esc[1] = 't'; break;
            case '\b': esc[1] = 'b'; break;
            case '\f': esc[1] = 'f'; break;
            default:
                memcpy(esc + 1, "u00", 3);
                esc[4] = hex[c >> 4];
                esc[5] = hex[c & 15];
                elen = 6;
            }
            if (!buf_put(b, esc, elen)) return false;
            i++;
            continue;
        }
        uint32_t cp;
        int len = utf8_seq(s + i, n - i, &cp);
        bool ok = len == 0 ? buf_puts(b, "\xEF\xBF\xBD")
                : cp == 0x2028 ? buf_puts(b, "\\u2028")
                : cp == 0x2029 ? buf_puts(b, "\\u2029")
                : buf_put(b, s + i, (size_t)len);
        if (!ok) return false;
        i += len ? (size_t)len : 1;
    }
    return buf_putc(b, '"');
}

/* ── Scalars ─────────────────────────────────────────────── */

/* A base64 digit's value, or -1 (computed, not a table filled on first
 * use, which threads converting at once would race on) */
static int base64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* Go's base64.StdEncoding.DecodeString, which skips \r and \n: 1 decoded,
 * 0 not base64, -1 out of memory */
static int base64_decode(oyl_str in, buffer *out) {
    out->len = 0;
    int quad[4], q = 0, pad = 0;
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = (unsigned char)in.data[i];
        if (c == '\r' || c == '\n') continue;
        if (pad) {   /* only padding may follow padding, and at most two */
            if (c != '=' || q == 0 || ++pad > 2) return 0;
            quad[q++] = 0;
        } else if (c == '=') {
            if (q < 2) return 0;
            pad = 1;
            quad[q++] = 0;
        } else {
            int v = base64_digit(c);
            if (v < 0) return 0;
            quad[q++] = v;
        }
        if (q == 4) {
            unsigned char bytes[3] = { (unsigned char)(quad[0] << 2 | quad[1] >> 4),
                                       (unsigned char)(quad[1] << 4 | quad[2] >> 2),
                                       (unsigned char)(quad[2] << 6 | quad[3]) };
            if (!buf_put(out, bytes, 3 - (size_t)pad)) return -1;
            q = 0;
            if (pad) pad = 3;   /* the end: nothing may follow */
        }
    }
    return q == 0;   /* 1 or 0 */
}

/* A scalar node's value, with a !!binary decoded into d->bin */
static bool scalar_value(doc *d, uint32_t i, oyl_value *v) {
    const oyl_node *n = &d->n[i];
    oyl_str t = text(d, i);
    if (d->typed && !n->props) {   /* untagged, typed while the tree was built */
        if (n->type == 0) {
            *v = (oyl_value){ .kind = OYL_VALUE_STR, .as = { .s = t } };
            return true;
        }
        int r = n->type < 255 ? oyl_schema_rule_value(d->schema, n->type - 1, t, v) : 0;
        if (r > 0) return true;
        if (r == -1) return fail(d, OYL_ERR_MEMORY, node_mark(d, i), "out of memory");
    }
    oyl_str tag = written_tag(d, i);
    if (same(tag, OYL_TAG_BINARY)) {
        int r = base64_decode(t, &d->bin);
        if (r < 0) return fail(d, OYL_ERR_MEMORY, node_mark(d, i), "out of memory");
        if (r == 0)
            return fail(d, OYL_ERR_PARSE, node_mark(d, i), "!!binary value contains invalid base64 data");
        *v = (oyl_value){ .kind = OYL_VALUE_STR, .as = { .s = { d->bin.data, d->bin.len } } };
        return true;
    }
    /* the tree typed its scalars by the document's schema while building */
    oyl_status st = d->typed ? oyl_node_value(d->t, i, v)
                             : oyl_schema_value_of(d->schema, t, (oyl_scalar_style)d->n[i].style, tag, v);
    if (st == OYL_ERR_PARSE) {
        if (!tag.len) tag = oyl_schema_resolve(d->schema, t, (oyl_scalar_style)n->style);
        int tl;
        const char *ts = short_tag(tag, &tl);
        return fail(d, OYL_ERR_PARSE, node_mark(d, i), "cannot decode `%.*s` as !!%.*s",
                    shown(t.len), t.data, tl, ts);
    }
    if (st != OYL_OK) return fail(d, st, node_mark(d, i), "out of memory");
    return true;
}

static bool write_scalar(doc *d, uint32_t i) {
    oyl_value v;
    if (!scalar_value(d, i, &v)) return false;
    buffer *b = &d->out;
    bool ok;
    switch (v.kind) {
    case OYL_VALUE_NULL:
        ok = buf_puts(b, "null");
        break;
    case OYL_VALUE_BOOL:
        ok = buf_puts(b, v.as.b ? "true" : "false");
        break;
    case OYL_VALUE_INT:
        ok = buf_int(b, v.as.i);
        break;
    case OYL_VALUE_UINT:
        ok = buf_uint(b, v.as.u, false);
        break;
    case OYL_VALUE_FLOAT:
        if (isnan(v.as.f) || isinf(v.as.f))
            return fail(d, OYL_ERR_PARSE, node_mark(d, i), "json: unsupported value: %s",
                        isnan(v.as.f) ? "NaN" : v.as.f > 0 ? "+Inf" : "-Inf");
        ok = put_json_float(b, v.as.f);
        break;
    default:
        ok = put_json_string(b, v.as.s.data, v.as.s.len);
        break;
    }
    return ok || out_of_memory(d);
}

/* ── Writing ─────────────────────────────────────────────── */

/* go-yaml v2's check, made at each node it decodes: past 1,000 nodes and
 * 100 through aliases, too large a share through aliases fails */
static bool count_decode(doc *d, uint32_t i) {
    if (d->quiet) return true;
    d->decodes++;
    if (d->alias_depth > 0) d->aliased++;
    if (d->aliased > 100 && d->decodes > 1000) {
        double ratio = d->decodes <= 400000    ? 0.99
                     : d->decodes >= 4000000   ? 0.10
                     : 0.99 - 0.89 * ((double)(d->decodes - 400000) / 3600000.0);
        if ((double)d->aliased / (double)d->decodes > ratio)
            return fail(d, OYL_ERR_LIMIT, node_mark(d, i), "document contains excessive aliasing");
    }
    return true;
}

static bool enter(doc *d, uint32_t i) {
    if (d->max_depth && ++d->depth > d->max_depth)
        return fail(d, OYL_ERR_LIMIT, node_mark(d, i), "nesting depth limit exceeded");
    if (!d->max_depth) d->depth++;
    return true;
}

static bool write_node(doc *d, uint32_t i);

/* A key's JSON string, into entry `e`: go-yaml v2 decodes the key and
 * sigs.k8s.io/yaml makes it a string. Text stays in the document; a number,
 * a boolean or decoded binary is written to d->keys. A key no JSON string
 * stands for is marked bad, and fails only if written (write_mapping), as
 * sigs.k8s.io/yaml sees only the keys that are left. */
static bool key_string(doc *d, uint32_t k, entry *e) {
    if (!count_decode(d, k)) return false;
    uint32_t i = k;
    if (d->n[i].kind == OYL_NODE_ALIAS) {   /* an alias's target is never an alias */
        d->alias_depth++;
        i = d->n[i].props;
        bool ok = count_decode(d, i);
        d->alias_depth--;
        if (!ok) return false;
    }
    const oyl_node *n = &d->n[i];
    if (n->kind != OYL_NODE_SCALAR)
        return fail(d, OYL_ERR_PARSE, node_mark(d, k), "invalid map key: a %s",
                    n->kind == OYL_NODE_MAPPING ? "mapping" : "sequence");
    oyl_value v;
    if (!scalar_value(d, i, &v)) return false;
    buffer *b = &d->keys;
    size_t start = b->len;
    bool ok;
    if (v.kind == OYL_VALUE_STR && !(d->bin.data && v.as.s.data == d->bin.data)) {
        e->key_at.ptr = v.as.s.data;
        e->key_len = (uint32_t)v.as.s.len;
        e->in_keys = 0;
        return v.as.s.len <= UINT32_MAX || fail(d, OYL_ERR_LIMIT, node_mark(d, k), "key too long");
    }
    switch (v.kind) {
    case OYL_VALUE_NULL:
        e->bad = 1;
        ok = true;
        break;
    case OYL_VALUE_UINT:   /* sigs.k8s.io/yaml takes int, int64, float64, bool and string keys */
        e->bad = 2;
        ok = buf_uint(b, v.as.u, false);
        break;
    case OYL_VALUE_BOOL:
        ok = buf_puts(b, v.as.b ? "true" : "false");
        break;
    case OYL_VALUE_INT:
        ok = buf_int(b, v.as.i);
        break;
    case OYL_VALUE_FLOAT:
        ok = put_float_key(b, v.as.f);
        e->zero = v.as.f == 0;
        break;
    default:
        ok = buf_put(b, v.as.s.data, v.as.s.len);
        break;
    }
    if (!ok) return out_of_memory(d);
    e->key_at.off = start;
    e->key_len = (uint32_t)(b->len - start);
    e->in_keys = 1;
    return true;
}

static inline const char *key_bytes(const doc *d, const entry *e) {
    return e->in_keys == 0 ? e->key_at.ptr
         : e->in_keys == 1 ? d->keys.data + e->key_at.off : d->memo_keys.data + e->key_at.off;
}

static int bytes_cmp(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    return c ? c : alen < blen ? -1 : alen > blen;
}

static bool push_entry(doc *d, entry e) {
    if (d->elen == d->ecap) {
        size_t cap = d->ecap ? d->ecap * 2 : 64;
        entry *es = realloc(d->entries, cap * sizeof *es);
        if (!es) return out_of_memory(d);
        d->entries = es;
        d->ecap = cap;
    }
    d->entries[d->elen++] = e;
    return true;
}

/* Is key node k a merge key? As go-yaml v2 sees it: << untagged and plain,
 * tagged !!merge, or tagged ! in any style, which leaves it to be resolved
 * as if plain (libyaml's reading, PyYAML's too). (A tag the parser's
 * schema gave isn't in the tree.) */
static bool is_merge(const doc *d, uint32_t k) {
    const oyl_node *n = &d->n[k];
    if (n->kind != OYL_NODE_SCALAR || n->len != 2 || memcmp(text(d, k).data, "<<", 2) != 0) return false;
    oyl_str tag = written_tag(d, k);
    if (tag.len == 1 && tag.data[0] == '!') return true;
    if (tag.len) return same(tag, OYL_TAG_MERGE);
    return n->style == OYL_SCALAR_PLAIN;
}

static bool collect(doc *d, uint32_t map, uint64_t *counter, bool written);

/* The mapping a merge item stands for: a mapping, or an alias to one */
static bool merge_item(doc *d, uint32_t item, uint32_t *map, bool *alias) {
    *alias = d->n[item].kind == OYL_NODE_ALIAS;
    *map = *alias ? d->n[item].props : item;
    if (d->n[*map].kind != OYL_NODE_MAPPING)
        return fail(d, OYL_ERR_PARSE, node_mark(d, item),
                    "map merge requires map or sequence of maps as the value");
    return true;
}

/* Merge items in go-yaml v2's order, each overriding what came before:
 * a sequence's items from the last, as earlier ones take precedence. Each
 * mapping merged is a level of nesting, as merges can chain. */
static bool merge_ordered(doc *d, uint32_t value, uint64_t *counter) {
    uint32_t items[1] = { value };
    uint32_t *list = items;
    size_t count = 1;
    uint32_t *seq = NULL;
    if (d->n[value].kind == OYL_NODE_SEQUENCE) {
        if (!count_decode(d, value)) return false;
        count = d->n[value].len;
        seq = malloc((count ? count : 1) * sizeof *seq);
        if (!seq) return out_of_memory(d);
        uint32_t c = value + 1;
        for (size_t j = 0; j < count; j++, c += d->n[c].span) seq[j] = c;
        list = seq;
    }
    bool ok = true;
    for (size_t j = count; ok && j-- > 0;) {
        uint32_t map;
        bool alias;
        ok = merge_item(d, list[j], &map, &alias);
        if (!ok) break;
        if (alias) {
            ok = count_decode(d, list[j]);
            d->alias_depth++;
        }
        ok = ok && count_decode(d, map) && enter(d, map) && collect(d, map, counter, false);
        if (ok) d->depth--;
        if (alias) d->alias_depth--;
    }
    free(seq);
    return ok;
}

/* YAML's merge, from collect: keys written in a mapping win, then the
 * merges, a later << over an earlier one (as PyYAML; collect takes them
 * from the last), a sequence's items in order. Each merged mapping's
 * entries, its own merges applied, fill the keys not yet set, at a lower
 * priority than everything before them; they are worked out once
 * (merged_entries), so merges along many paths to one mapping cost no
 * more than one. */
static bool settle(doc *d, size_t base, size_t *count);

/* Keep the entries d->entries[base, base + count) as mapping `map`'s */
static bool memo_save(doc *d, uint32_t map, size_t base, size_t count) {
    if (count > d->mcap - d->mlen) {
        size_t cap = d->mcap ? d->mcap : 64;
        while (cap - d->mlen < count) {
            if (cap > SIZE_MAX / 2 / sizeof *d->memo) return false;
            cap *= 2;
        }
        entry *m = realloc(d->memo, cap * sizeof *m);
        if (!m) return false;
        d->memo = m;
        d->mcap = cap;
    }
    if (d->nspans == d->spans_cap) {
        uint32_t cap = d->spans_cap ? d->spans_cap * 2 : 16;
        struct span *sp = cap > d->spans_cap ? realloc(d->spans, cap * sizeof *sp) : NULL;
        if (!sp) return false;
        d->spans = sp;
        d->spans_cap = cap;
    }
    if (!buf_reserve(&d->memo_keys, 1)) return false;   /* never offsets from NULL */
    for (size_t j = 0; j < count; j++) {
        entry e = d->entries[base + j];
        if (e.in_keys == 1) {
            size_t off = d->memo_keys.len;
            if (!buf_put(&d->memo_keys, d->keys.data + e.key_at.off, e.key_len)) return false;
            e.key_at.off = off;
            e.in_keys = 2;
        }
        d->memo[d->mlen + j] = e;
    }
    if (!oyl_nmap_put(&d->memo_at, map, d->nspans)) return false;
    d->spans[d->nspans++] = (struct span){ d->mlen, count };
    d->mlen += count;
    return true;
}

/* Mapping `map`'s entries as YAML's merge takes them, its own merges
 * applied and settled: d->memo[*at, *at + *n), worked out the first time */
static bool merged_entries(doc *d, uint32_t map, size_t *at, size_t *n) {
    uint32_t s = oyl_nmap_get(&d->memo_at, map);
    if (s == UINT32_MAX) {
        size_t base = d->elen, kbase = d->keys.len, count = 0;
        uint64_t counter = 0;
        bool quiet = d->quiet;
        d->quiet = true;
        bool ok = collect(d, map, &counter, false) && settle(d, base, &count);
        d->quiet = quiet;
        if (!ok) return false;
        if (!memo_save(d, map, base, count)) return out_of_memory(d);
        d->elen = base;
        d->keys.len = kbase;
        s = d->nspans - 1;
    }
    *at = d->spans[s].off;
    *n = d->spans[s].n;
    return true;
}

static bool prepare_merges(doc *d, uint32_t count);

/* Copy d->memo[at, at + count) onto d->entries at priority `prio`, as
 * merged: within the merge budget */
static bool push_merged(doc *d, uint32_t from, size_t at, size_t count, uint64_t prio, bool alias) {
    if (count > d->merge_budget)
        return fail(d, OYL_ERR_LIMIT, node_mark(d, from), "merges too costly to expand");
    d->merge_budget -= count;
    for (size_t j = 0; j < count; j++) {
        entry e = d->memo[at + j];
        e.prio = prio;
        e.written = false;
        e.aliased = e.aliased || alias;   /* a copy, as an alias's value is */
        if (!push_entry(d, e)) return false;
    }
    return true;
}

/* Sequence `seq`'s mappings as YAML's merge takes them, earlier ones
 * winning, settled: d->memo[*at, *at + *n), worked out the first time, so
 * that a sequence many mappings merge (through aliases) costs no more
 * than its entries each time */
static bool merged_items(doc *d, uint32_t seq, size_t *at, size_t *n) {
    uint32_t s = oyl_nmap_get(&d->memo_at, seq);
    if (s == UINT32_MAX) {
        size_t base = d->elen, count;
        uint64_t prio = (uint64_t)1 << 62;
        uint32_t item = seq + 1;
        for (uint32_t c = 0; c < d->n[seq].len; c++, item += d->n[item].span) {
            uint32_t map;
            bool alias;
            size_t mat, mn;
            if (!merge_item(d, item, &map, &alias) || !merged_entries(d, map, &mat, &mn) ||
                !push_merged(d, item, mat, mn, --prio, alias))
                return false;
        }
        if (!settle(d, base, &count)) return false;
        if (!memo_save(d, seq, base, count)) return out_of_memory(d);
        d->elen = base;
        s = d->nspans - 1;
    }
    *at = d->spans[s].off;
    *n = d->spans[s].n;
    return true;
}

static bool merge_by_yaml(doc *d, uint32_t value, uint64_t *group) {
    if (!d->prepared) {   /* the document's first merge */
        d->prepared = true;
        if (!prepare_merges(d, d->t->len)) return false;
    }
    /* an alias to a sequence of mappings merges as the sequence */
    bool seq_alias = d->n[value].kind == OYL_NODE_ALIAS && d->n[d->n[value].props].kind == OYL_NODE_SEQUENCE;
    if (seq_alias) value = d->n[value].props;
    uint32_t map;
    bool alias = seq_alias;
    size_t at, count;
    if (d->n[value].kind == OYL_NODE_SEQUENCE ? !merged_items(d, value, &at, &count)
        : !merge_item(d, value, &map, &alias) || !merged_entries(d, map, &at, &count))
        return false;
    uint64_t prio = (uint64_t)1 << 62;
    prio = *group < prio ? prio - ++*group : 0;
    return push_merged(d, value, at, count, prio, alias);
}

/* Work out each mapping YAML's merge takes, at the document's first
 * merge, in the order mappings end: a merge's mappings end before the
 * mapping merging them (an alias's anchor ends before it, as
 * contains_itself checked), so theirs are ready and working one out
 * never recurses */
static bool prepare_merges(doc *d, uint32_t count) {
    oyl_node_map merged;
    oyl_nmap_init(&merged, &d->t->hash_key);
    d->memo_at.key = &d->t->hash_key;
    bool ok = true, any = false;
    for (uint32_t m = 0; m < count && ok; m++) {
        if (d->n[m].kind != OYL_NODE_MAPPING) continue;
        uint32_t k = m + 1, v;
        for (uint32_t c = 0; c < d->n[m].len && ok; c++, k = v + d->n[v].span) {
            v = k + d->n[k].span;
            if (!is_merge(d, k)) continue;
            uint32_t value = v;
            if (d->n[value].kind == OYL_NODE_ALIAS && d->n[d->n[value].props].kind == OYL_NODE_SEQUENCE)
                value = d->n[value].props;
            bool is_seq = d->n[value].kind == OYL_NODE_SEQUENCE;
            if (is_seq) {   /* a sequence many mappings merge is scanned once */
                any = true;
                if (oyl_nmap_get(&merged, value) != UINT32_MAX) continue;
                ok = oyl_nmap_put(&merged, value, 1);
            }
            uint32_t items = is_seq ? d->n[value].len : 1, item = is_seq ? value + 1 : value;
            for (uint32_t j = 0; j < items && ok; j++, item += d->n[item].span) {
                uint32_t s = d->n[item].kind == OYL_NODE_ALIAS ? d->n[item].props : item;
                if (d->n[s].kind == OYL_NODE_MAPPING && oyl_nmap_get(&merged, s) == UINT32_MAX)
                    ok = oyl_nmap_put(&merged, s, 1);
                any = true;
            }
        }
    }
    uint32_t *stack = NULL, top = 0, cap = 0;
    for (uint32_t i = 0; ok && any && i <= count; i++) {
        while (ok && top && (i == count || stack[top - 1] + d->n[stack[top - 1]].span <= i)) {
            uint32_t m = stack[--top];
            size_t at, n;
            if (oyl_nmap_get(&merged, m) != UINT32_MAX && !merged_entries(d, m, &at, &n)) {
                oyl_nmap_free(&merged);
                free(stack);
                return false;
            }
        }
        if (i < count && d->n[i].kind == OYL_NODE_MAPPING) {
            if (top == cap) {
                uint32_t *ns = realloc(stack, (cap ? cap * 2 : 64) * sizeof *ns);
                if (!ns) {
                    ok = false;
                    break;
                }
                stack = ns;
                cap = cap ? cap * 2 : 64;
            }
            stack[top++] = i;
        }
    }
    oyl_nmap_free(&merged);
    free(stack);
    return ok || out_of_memory(d);
}

/* Push mapping `map`'s entries onto d->entries with rising priorities */
static bool collect(doc *d, uint32_t map, uint64_t *counter, bool written) {
    bool merging = (d->flags & OYL_JSON_MERGE) != 0;
    bool ordered = (d->flags & OYL_JSON_MERGE_ORDER) != 0;
    uint64_t group = 0;
    size_t pbase = d->plen;   /* this mapping's merges wait at d->pending[pbase...] */
    uint32_t k = map + 1, v;
    for (uint32_t c = 0; c < d->n[map].len; c++, k = v + d->n[v].span) {
        v = k + d->n[k].span;
        if (merging && is_merge(d, k)) {
            if (ordered) {
                if (!merge_ordered(d, v, counter)) return false;
                continue;
            }
            if (d->plen == d->pcap) {
                size_t cap = d->pcap ? d->pcap * 2 : 16;
                uint32_t *ps = cap <= SIZE_MAX / sizeof *ps ? realloc(d->pending, cap * sizeof *ps) : NULL;
                if (!ps) return out_of_memory(d);
                d->pending = ps;
                d->pcap = cap;
            }
            d->pending[d->plen++] = v;
            continue;
        }
        entry e = { .key = k, .value = v, .written = written };
        if (!key_string(d, k, &e)) return false;
        e.prio = ordered || !merging ? ++*counter : ((uint64_t)1 << 63) + ++*counter;
        if (!push_entry(d, e)) return false;
    }
    /* merge_by_yaml may collect other mappings (the document's first merge
     * prepares them all), which push and pop above this one's */
    for (size_t j = d->plen; j-- > pbase;)
        if (!merge_by_yaml(d, d->pending[j], &group)) return false;
    d->plen = pbase;
    return true;
}

/* Compare entries by key as keys are compared, then priority; bad keys
 * (each kind its own) first */
static int entry_cmp(const doc *d, const entry *a, const entry *b) {
    if (a->bad != b->bad) return a->bad > b->bad ? -1 : 1;
    if (a->zero != b->zero) return a->zero ? -1 : 1;
    int c = a->zero ? 0 : bytes_cmp(key_bytes(d, a), a->key_len, key_bytes(d, b), b->key_len);
    return c ? c : a->prio < b->prio ? -1 : a->prio > b->prio;
}

static void sort_entries(const doc *d, entry *e, entry *tmp, size_t n) {
    if (n < 2) return;
    if (n <= 16) {   /* insertion sort */
        for (size_t i = 1; i < n; i++) {
            entry x = e[i];
            size_t j = i;
            while (j > 0 && entry_cmp(d, &e[j - 1], &x) > 0) { e[j] = e[j - 1]; j--; }
            e[j] = x;
        }
        return;
    }
    size_t h = n / 2;
    sort_entries(d, e, tmp, h);
    sort_entries(d, e + h, tmp, n - h);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = entry_cmp(d, &e[j], &e[i]) < 0 ? e[j++] : e[i++];
    while (i < h) tmp[k++] = e[i++];
    while (j < n) tmp[k++] = e[j++];
    memcpy(e, tmp, n * sizeof *e);
}

/* Sort the entries from `base` by key and keep the winner of each key.
 * Under OYL_JSON_STRICT a key set twice fails: by keys written in one
 * mapping, or, in go-yaml v2's merge order, by a merge too. */
static bool settle(doc *d, size_t base, size_t *count) {
    size_t n = d->elen - base;
    *count = 0;
    if (n == 0) return true;
    if (n > 16 && n > d->tmp_cap) {   /* insertion sort needs no buffer */
        entry *tmp = realloc(d->sort_tmp, n * sizeof *tmp);
        if (!tmp) return out_of_memory(d);
        d->sort_tmp = tmp;
        d->tmp_cap = n;
    }
    entry *e = d->entries + base;
    sort_entries(d, e, d->sort_tmp, n);
    bool strict = (d->flags & OYL_JSON_STRICT) != 0;
    bool any_counts = (d->flags & OYL_JSON_MERGE_ORDER) != 0;
    size_t out = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1, written = e[i].written;
        while (j < n && e[j].bad == e[i].bad && e[j].zero == e[i].zero &&
               (e[i].zero || bytes_cmp(key_bytes(d, &e[j]), e[j].key_len, key_bytes(d, &e[i]), e[i].key_len) == 0))
            written += e[j++].written;
        if (strict && (any_counts ? j - i > 1 : written > 1))
            return e[i].bad == 1
                 ? fail(d, OYL_ERR_PARSE, node_mark(d, e[j - 1].key), "null key already set in map")
                 : fail(d, OYL_ERR_PARSE, node_mark(d, e[j - 1].key), "key \"%.*s\" already set in map",
                        shown(e[i].key_len), key_bytes(d, &e[i]));
        if (any_counts && written && !e[j - 1].written) {
            /* a merge won over a key written in the mapping: go-yaml v2's
             * order, but every other YAML reader keeps the written key */
            size_t w = j - 1;
            while (w > i && !e[w].written) w--;
            char msg[256];
            snprintf(msg, sizeof msg, "key \"%.*s\" is overridden by a later << merge, as go-yaml v2 "
                     "reads it; YAML's merge keeps it", (int)(e[w].key_len > 100 ? 100 : e[w].key_len),
                     key_bytes(d, &e[w]));
            oyl_parser_warn(d->p, OYL_WARN_MERGE_ORDER, node_mark(d, e[w].key), msg);
        }
        e[out++] = e[j - 1];   /* the highest priority */
        i = j;
    }
    *count = out;
    return true;
}

/* The float key 0 or -0, settled apart (it sorts first, after bad keys),
 * is written as the last one was, sign and all, as Go keeps it; that
 * string may be another key's ("0", the int 0), where the later one wins
 * (or OYL_JSON_STRICT fails), as for any two keys that become one string.
 * It goes where its string sorts. */
static bool place_zero(doc *d, size_t base, size_t *count) {
    entry *e = d->entries + base, z = e[0];
    size_t n = *count, j = 1;
    while (j < n && bytes_cmp(key_bytes(d, &e[j]), e[j].key_len, key_bytes(d, &z), z.key_len) < 0) j++;
    if (j < n && bytes_cmp(key_bytes(d, &e[j]), e[j].key_len, key_bytes(d, &z), z.key_len) == 0) {
        if (d->flags & OYL_JSON_STRICT)
            return fail(d, OYL_ERR_PARSE, node_mark(d, z.prio > e[j].prio ? z.key : e[j].key),
                        "key \"%.*s\" already set in map", shown(z.key_len), key_bytes(d, &z));
        if (z.prio > e[j].prio) e[j] = z;
        memmove(e, e + 1, (n - 1) * sizeof *e);   /* one of the two goes */
        *count = n - 1;
        d->elen--;
        return true;
    }
    memmove(e, e + 1, (j - 1) * sizeof *e);
    e[j - 1] = z;
    return true;
}

static bool write_mapping(doc *d, uint32_t m) {
    if (!enter(d, m)) return false;
    size_t base = d->elen, kbase = d->keys.len;
    uint64_t counter = 0;
    size_t count = 0;
    if (!collect(d, m, &counter, true) || !settle(d, base, &count)) return false;
    d->elen = base + count;
    /* bad keys sort first; the first in the document fails */
    const entry *bad = NULL;
    for (size_t j = 0; j < count && d->entries[base + j].bad; j++)
        if (!bad || d->entries[base + j].key < bad->key) bad = &d->entries[base + j];
    if (bad)
        return fail(d, OYL_ERR_PARSE, node_mark(d, bad->key), bad->bad == 1
                    ? "unsupported map key: null" : "unsupported map key of type: uint64");
    if (count && d->entries[base].zero && !place_zero(d, base, &count)) return false;
    if (!buf_putc(&d->out, '{')) return out_of_memory(d);
    for (size_t j = 0; j < count; j++) {
        entry e = d->entries[base + j];
        if ((j && !buf_putc(&d->out, ',')) ||
            !put_json_string(&d->out, key_bytes(d, &e), e.key_len) ||
            !buf_putc(&d->out, ':'))
            return out_of_memory(d);
        if (e.aliased) d->alias_depth++;
        bool ok = write_node(d, e.value);
        if (e.aliased) d->alias_depth--;
        if (!ok) return false;
    }
    if (!buf_putc(&d->out, '}')) return out_of_memory(d);
    d->elen = base;
    d->keys.len = kbase;
    d->depth--;
    return true;
}

static bool write_node(doc *d, uint32_t i) {
    if (!count_decode(d, i)) return false;
    const oyl_node *n = &d->n[i];
    switch (n->kind) {
    case OYL_NODE_ALIAS: {
        d->alias_depth++;
        bool ok = write_node(d, n->props);
        d->alias_depth--;
        return ok;
    }
    case OYL_NODE_SCALAR:
        return write_scalar(d, i);
    case OYL_NODE_SEQUENCE: {
        if (!enter(d, i)) return false;
        if (!buf_putc(&d->out, '[')) return out_of_memory(d);
        uint32_t c = i + 1;
        for (uint32_t j = 0; j < n->len; j++, c += d->n[c].span) {
            if (j && !buf_putc(&d->out, ',')) return out_of_memory(d);
            if (!write_node(d, c)) return false;
        }
        if (!buf_putc(&d->out, ']')) return out_of_memory(d);
        d->depth--;
        return true;
    }
    case OYL_NODE_MAPPING:
        return write_mapping(d, i);
    }
    return false;
}

/* ── Checking and counting, before writing ───────────────── */

/* Does rule r take some plain scalars without a value? Only YAML 1.1's
 * timestamp rule does, for dates that don't exist. */
static bool refusing_rule(const oyl_schema *s, int r) {
    return r >= 0 && r < s->rule_count && s->rules[r].kind == RULE_TIMESTAMP && s->rules[r].flags;
}

static bool any_refusing_rule(const oyl_schema *s) {
    for (int r = 0; r < s->rule_count; r++)
        if (refusing_rule(s, r)) return true;
    return false;
}

/* What a << with value v can't merge, or UINT32_MAX: it takes a mapping,
 * an alias to one, or a sequence of them, and under YAML's merge an alias
 * to such a sequence */
static uint32_t unmergeable(const doc *d, uint32_t v) {
    uint32_t s = v;
    if (d->n[v].kind == OYL_NODE_ALIAS) {
        s = d->n[v].props;
        if (d->n[s].kind == OYL_NODE_SEQUENCE && (d->flags & OYL_JSON_MERGE_ORDER)) return v;
    }
    if (d->n[s].kind == OYL_NODE_MAPPING) return UINT32_MAX;
    if (d->n[s].kind != OYL_NODE_SEQUENCE) return v;
    uint32_t c = s + 1;
    for (uint32_t j = 0; j < d->n[s].len; j++, c += d->n[c].span)
        if (d->n[d->n[c].kind == OYL_NODE_ALIAS ? d->n[c].props : c].kind != OYL_NODE_MAPPING) return c;
    return UINT32_MAX;
}

/* What decoding finds wherever it is: go-yaml v2 and PyYAML decode every
 * node, even a value whose key is set again later, so an alias inside its
 * own anchor, a scalar its tag (or YAML 1.1's timestamp rule) rejects, a
 * collection as a key, a merge of what isn't a mapping and, under
 * OYL_JSON_STRICT, a key set twice in one mapping fail wherever they are.
 * What only JSON can't hold (a null key, an infinity) fails if it is
 * written. Sets *aliases if the document has any. */
static bool check_nodes(doc *d, uint32_t count, bool *aliases, oyl_node_map *seqs) {
    bool merging = (d->flags & OYL_JSON_MERGE) != 0;
    bool strict = (d->flags & OYL_JSON_STRICT) != 0;
    bool refusing = any_refusing_rule(d->schema);
    *aliases = false;
    for (uint32_t i = 0; i < count; i++) {
        const oyl_node *n = &d->n[i];
        if (n->kind == OYL_NODE_ALIAS) {
            *aliases = true;
            if (contains_itself(d, i)) {
                oyl_str name = text(d, i);
                return fail(d, OYL_ERR_PARSE, node_mark(d, i), "anchor '%.*s' value contains itself",
                            shown(name.len), name.data);
            }
        } else if (n->kind == OYL_NODE_SCALAR) {
            oyl_value v;
            if ((written_tag(d, i).len ||
                 (refusing && n->style == OYL_SCALAR_PLAIN &&
                  refusing_rule(d->schema, d->typed && n->type < 255 ? n->type - 1
                                                    : oyl_schema_rule_of(d->schema, text(d, i))))) &&
                !scalar_value(d, i, &v))
                return false;
        } else if (n->kind == OYL_NODE_MAPPING) {
            size_t base = d->elen, kbase = d->keys.len, settled;
            uint64_t counter = 0;
            uint32_t k = i + 1, v;
            for (uint32_t c = 0; c < n->len; c++, k = v + d->n[v].span) {
                v = k + d->n[k].span;
                uint32_t key = d->n[k].kind == OYL_NODE_ALIAS ? d->n[k].props : k;
                if (d->n[key].kind != OYL_NODE_SCALAR)
                    return fail(d, OYL_ERR_PARSE, node_mark(d, k), "invalid map key: a %s",
                                d->n[key].kind == OYL_NODE_MAPPING ? "mapping" : "sequence");
                if (merging && is_merge(d, k)) {
                    /* a sequence many mappings merge through aliases is checked once */
                    uint32_t seq = d->n[v].kind == OYL_NODE_ALIAS ? d->n[v].props : v;
                    bool shared = seq != v && d->n[seq].kind == OYL_NODE_SEQUENCE;
                    if (shared && oyl_nmap_get(seqs, seq) != UINT32_MAX) continue;
                    uint32_t bad = unmergeable(d, v);
                    if (bad != UINT32_MAX)
                        return fail(d, OYL_ERR_PARSE, node_mark(d, bad),
                                    "map merge requires map or sequence of maps as the value");
                    if (shared && !oyl_nmap_put(seqs, seq, 1)) return out_of_memory(d);
                    continue;
                }
                if (!strict) continue;
                entry e = { .key = k, .value = v, .written = true, .prio = ++counter };
                if (!key_string(d, k, &e) || !push_entry(d, e)) return false;
            }
            if (strict && !settle(d, base, &settled)) return false;
            d->elen = base;
            d->keys.len = kbase;
        }
    }
    return true;
}

static bool check(doc *d, uint32_t count, bool *aliases) {
    oyl_node_map seqs;
    oyl_nmap_init(&seqs, &d->t->hash_key);
    bool ok = check_nodes(d, count, aliases, &seqs);
    oyl_nmap_free(&seqs);
    return ok;
}

static bool count_node(doc *d, uint32_t i);

/* A << value as go-yaml v2's merge decodes it: a sequence's items from the
 * last, the sequence itself not decoded */
static bool count_merge(doc *d, uint32_t v) {
    if (d->n[v].kind != OYL_NODE_SEQUENCE) return count_node(d, v);
    uint32_t len = d->n[v].len, few[16];
    uint32_t *items = len <= 16 ? few : malloc(len * sizeof *items);
    if (!items) return out_of_memory(d);
    uint32_t c = v + 1;
    for (uint32_t j = 0; j < len; j++, c += d->n[c].span) items[j] = c;
    bool ok = true;
    for (uint32_t j = len; ok && j-- > 0;) ok = count_node(d, items[j]);
    if (items != few) free(items);
    return ok;
}

/* go-yaml v2's aliasing count, in go-yaml v2's merge order, before anything
 * is written: node i and all that decoding it decodes, as go-yaml v2 does
 * (decode.go: unmarshal, alias, mapping, merge), every key and value in
 * document order, those overridden later too */
static bool count_node(doc *d, uint32_t i) {
    if (!count_decode(d, i)) return false;
    const oyl_node *n = &d->n[i];
    if (n->kind == OYL_NODE_ALIAS) {
        d->alias_depth++;
        bool ok = count_node(d, n->props);
        d->alias_depth--;
        return ok;
    }
    if (n->kind == OYL_NODE_SCALAR) return true;
    if (!enter(d, i)) return false;
    bool ok = true;
    if (n->kind == OYL_NODE_SEQUENCE) {
        uint32_t c = i + 1;
        for (uint32_t j = 0; ok && j < n->len; j++, c += d->n[c].span) ok = count_node(d, c);
    } else {
        bool merging = (d->flags & OYL_JSON_MERGE) != 0;
        uint32_t k = i + 1, v;
        for (uint32_t j = 0; ok && j < n->len; j++, k = v + d->n[v].span) {
            v = k + d->n[k].span;
            ok = merging && is_merge(d, k) ? count_merge(d, v) : count_node(d, k) && count_node(d, v);
        }
    }
    d->depth--;
    return ok;
}

/* ── The entry point ─────────────────────────────────────── */

oyl_status oyl_parse_json(oyl_parser *p, const oyl_schema *schema, unsigned flags,
                          oyl_str *json) {
    if (!p || !json) return OYL_ERR_INPUT;
    *json = (oyl_str){ NULL, 0 };
    doc d = { .p = p, .schema = schema, .flags = flags,
              .max_depth = oyl_parser_depth_limit(p) };
    oyl_nmap_init(&d.memo_at, NULL);   /* keyed by prepare_merges, once there is a tree */
    /* both buffers exist even when nothing is written to them, so that
     * offsets into them are never taken from NULL */
    d.entries = malloc(64 * sizeof *d.entries);
    d.ecap = d.entries ? 64 : 0;
    if (!d.entries || !buf_reserve(&d.out, 256) || !buf_reserve(&d.keys, 256)) {
        free(d.entries);
        free(d.out.data);
        free(d.keys.data);
        oyl_parser_fail(p, OYL_ERR_MEMORY, (oyl_mark){ 0, 0, 0 }, "out of memory");
        return OYL_ERR_MEMORY;
    }
    oyl_status st = oyl_parse_tree_as_written(p, &d.t);
    if (st == OYL_OK && d.t) {
        uint32_t count;
        d.n = oyl_tree_nodes(d.t, &count);
        const oyl_dialect *dialect = oyl_tree_dialect(d.t);
        const oyl_schema *typed_by = oyl_dialect_schema(dialect);
        if (!schema) {   /* the document's dialect */
            d.schema = typed_by;
            oyl_merge_order m = oyl_dialect_merge(dialect);
            d.flags = (flags & ~(unsigned)(OYL_JSON_MERGE | OYL_JSON_MERGE_ORDER)) |
                      (m == OYL_MERGE_YAML ? OYL_JSON_MERGE
                       : m == OYL_MERGE_ORDERED ? OYL_JSON_MERGE | OYL_JSON_MERGE_ORDER : 0);
        }
        d.typed = d.schema == typed_by;
        uint64_t budget = ((uint64_t)count * 16 + 100000) * 4;   /* as the parser's for events */
        d.merge_budget = budget > SIZE_MAX ? SIZE_MAX : (size_t)budget;
        d.decodes = 1;   /* go-yaml v2 counts the document node */
        bool aliases, ordered = (d.flags & OYL_JSON_MERGE_ORDER) != 0;
        d.quiet = true;
        bool ok = check(&d, count, &aliases);
        /* go-yaml v2's merge order counts as go-yaml v2 decodes, first, and
         * writing then counts nothing; otherwise writing counts what it
         * writes. Without aliases, nothing counts against the limit. */
        if (ok && ordered && aliases) {
            d.quiet = false;
            ok = count_node(&d, 0);
        }
        d.quiet = ordered;
        if (ok && write_node(&d, 0)) {
            char *s = oyl_arena_dup(oyl_parser_arena(p), d.out.data ? d.out.data : "", d.out.len);
            if (s) *json = (oyl_str){ s, d.out.len };
            else out_of_memory(&d);
        }
        st = d.status;
    }
    oyl_tree_free(d.t);
    free(d.out.data);
    free(d.keys.data);
    free(d.bin.data);
    free(d.entries);
    free(d.sort_tmp);
    free(d.pending);
    free(d.memo);
    free(d.spans);
    free(d.memo_keys.data);
    oyl_nmap_free(&d.memo_at);
    return st;
}
