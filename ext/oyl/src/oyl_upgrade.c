/*
 * oyl_upgrade.c — a YAML 1.1 file made YAML 1.2 in place
 *
 * oyl_upgrade reads the stream once, as trees in the source's dialect, and
 * rewrites only what a YAML 1.2 reader would read otherwise: scalars by
 * policy (values keep the source's meaning, keys their text), %YAML 1.1,
 * libyaml's leniencies (\' in double quotes, a block scalar ending the
 * input without a line break, lines not indented past their block) and
 * keys go-yaml v2's merge order overrides. Every other byte stays. Then it
 * reads its output in each reader and in YAML 1.2, and fails rather than
 * hand back a file that reads otherwise than it decided.
 */

#include "oyl_internal.h"
#include "oyl_chars.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Readers ─────────────────────────────────────────────── */

/* A reader of the file: its schema, and whether it hands programs a
 * timestamp as its text (go-yaml v2) */
typedef struct {
    const oyl_schema *schema;
    bool              ts_text;
    const char       *name;
} reader;

/* A scalar as a reader takes it: its tag and value, unless its tag
 * rejects it */
typedef struct {
    bool    ok;
    oyl_key key;
} reading;

static reading read_as(const reader *r, oyl_str text, oyl_scalar_style style, oyl_str tag) {
    reading out = { 0 };
    oyl_value v;
    if (oyl_schema_value_of(r->schema, text, style, tag, &v) != OYL_OK) return out;
    oyl_str t = tag.len ? oyl_key_tag(r->schema, tag, text) : oyl_schema_resolve(r->schema, text, style);
    out.ok = true;
    oyl_key_make(&out.key, t, text, OYL_OK, &v);
    if (r->ts_text && oyl_key_tag_eq(t, OYL_TAG_TIMESTAMP)) {
        out.key.tag = OYL_TAG_STR;
        out.key.value = (oyl_value){ .kind = OYL_VALUE_STR, .as = { .s = text } };
    }
    return out;
}

/* Do two readings agree: tag and value, -0.0 apart from 0.0 */
static bool same(const reading *a, const reading *b) {
    if (!a->ok || !b->ok) return a->ok == b->ok;
    if (oyl_key_cmp(&a->key, &b->key) != 0) return false;
    return a->key.value.kind != OYL_VALUE_FLOAT ||
           signbit(a->key.value.as.f) == signbit(b->key.value.as.f);
}

/* ── State ───────────────────────────────────────────────── */

/* What the upgrade does with a node */
enum {
    N_SAME,      /* nothing: every reader reads it as `expect` */
    N_CHANGED,   /* rewritten: every reader reads it as `expect` */
    N_SKIP,      /* left, its readers disagreeing (reported), or not checked */
    N_REMOVED,   /* commented out */
};

typedef struct {
    uint8_t what;
    size_t  fix;      /* its fix, plus one (0: none) */
    oyl_key expect;
} node_plan;

typedef struct {
    oyl_tree  *tree;
    node_plan *plan;       /* by node */
    bool       left;       /* chooses YAML 1.2: left alone */
} doc;

/* A fix, and the input it replaces */
typedef struct {
    oyl_fix fix;
    size_t  start, end;
} fix_rec;

/* A warning heard while reading */
typedef struct {
    size_t      offset;
    const char *msg;
} heard;

typedef struct {
    oyl_parser *p;
    oyl_arena  *arena;      /* the parser's: the output and fixes */
    const char *in;
    size_t      len;
    unsigned    flags;
    reader      src[2];     /* the source's readers: one, or both when unknown */
    int         nsrc;
    reader      core;
    const oyl_dialect *check[2];   /* the readers' dialects, to check the output */
    bool        ordered;    /* a reader merges in go-yaml v2's order: fix overrides */
    bool        merge_disagree;    /* readers merge otherwise: report overrides */
    doc        *docs;
    size_t      ndocs, docs_cap;
    fix_rec    *fixes;
    size_t      nfixes, fixes_cap;
    heard      *heard;
    size_t      nheard, heard_cap;
    oyl_warn_fn user_fn;
    void       *user_data;
    size_t     *lines;      /* each line's start */
    size_t      nlines;
    char       *scratch;    /* a spelling being made */
    size_t      slen, scap;
    size_t     *cover;      /* what rewrites and comment-outs replaced: start, end, sorted */
    size_t      ncover;
    oyl_arena  *tmp;        /* for a merge source's entries, reset for each */
    bool        oom;
} upgrade;

static bool grow(void **arr, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return true;
    size_t nc = *cap ? *cap * 2 : 16;
    while (nc < need) nc *= 2;
    void *na = realloc(*arr, nc * size);
    if (!na) return false;
    *arr = na;
    *cap = nc;
    return true;
}

static void hear(void *user, oyl_warning code, oyl_mark mark, const char *msg) {
    upgrade *u = user;
    if (code == OYL_WARN_LENIENCY &&
        (msg == oyl_msg_flow_indent || msg == oyl_msg_eof_no_break)) {
        if (grow((void **)&u->heard, &u->heard_cap, u->nheard + 1, sizeof *u->heard))
            u->heard[u->nheard++] = (heard){ mark.offset, msg };
        else
            u->oom = true;
    }
    if (u->user_fn) u->user_fn(u->user_data, code, mark, msg);
}

/* Text in the parser's arena */
static oyl_str keep(upgrade *u, const char *s, size_t n) {
    char *d = oyl_arena_alloc(u->arena, n + 1, 1);
    if (!d) { u->oom = true; return (oyl_str){ "", 0 }; }
    if (n) memcpy(d, s, n);
    d[n] = '\0';
    return (oyl_str){ d, n };
}

static const char *keepf(upgrade *u, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof buf) n = sizeof buf - 1;
    return keep(u, buf, (size_t)n).data;
}

/* Where offset `at` is: line and column from 1 */
static oyl_mark mark_at(const upgrade *u, size_t at) {
    size_t lo = 0, hi = u->nlines;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (u->lines[mid] <= at) lo = mid; else hi = mid;
    }
    return (oyl_mark){ at, lo + 1, at - u->lines[lo] + 1 };
}

static fix_rec *add_fix(upgrade *u, oyl_fix_kind kind, size_t start, size_t end, oyl_str after,
                        bool fixed, const char *message) {
    if (!grow((void **)&u->fixes, &u->fixes_cap, u->nfixes + 1, sizeof *u->fixes)) {
        u->oom = true;
        return NULL;
    }
    fix_rec *f = &u->fixes[u->nfixes++];
    f->start = start;
    f->end = end;
    f->fix = (oyl_fix){ .kind = kind, .mark = mark_at(u, start),
                        .before = { u->in + start, end - start },
                        .after = fixed ? after : (oyl_str){ "", 0 },
                        .fixed = fixed, .message = message };
    return f;
}

static int by_offset(const void *x, const void *y) {
    const size_t *a = x, *b = y;
    return (*a > *b) - (*a < *b);
}

/* Note what rewrites and comment-outs replace, for covered() */
static void note_cover(upgrade *u) {
    size_t n = 0;
    for (size_t k = 0; k < u->nfixes; k++) {
        const fix_rec *f = &u->fixes[k];
        if (f->fix.fixed && (f->fix.kind == OYL_FIX_SCALAR || f->fix.kind == OYL_FIX_MERGE) &&
            f->start < f->end)
            n++;
    }
    u->cover = malloc((n ? n : 1) * 2 * sizeof *u->cover);
    if (!u->cover) { u->oom = true; return; }
    for (size_t k = 0; k < u->nfixes; k++) {
        const fix_rec *f = &u->fixes[k];
        if (f->fix.fixed && (f->fix.kind == OYL_FIX_SCALAR || f->fix.kind == OYL_FIX_MERGE) &&
            f->start < f->end) {
            u->cover[2 * u->ncover] = f->start;
            u->cover[2 * u->ncover + 1] = f->end;
            u->ncover++;
        }
    }
    if (u->ncover) qsort(u->cover, u->ncover, 2 * sizeof *u->cover, by_offset);
}

/* Whether [start, end) lies in what a rewrite or comment-out replaced */
static bool covered(const upgrade *u, size_t start, size_t end) {
    size_t lo = 0, hi = u->ncover;
    while (lo < hi) {   /* the first range starting after `start` */
        size_t mid = (lo + hi) / 2;
        if (u->cover[2 * mid] <= start) lo = mid + 1; else hi = mid;
    }
    return lo > 0 && end <= u->cover[2 * (lo - 1) + 1];
}

/* ── The input's text ────────────────────────────────────── */

static size_t line_start(const upgrade *u, size_t at) {
    while (at > 0 && u->in[at - 1] != '\n' && u->in[at - 1] != '\r') at--;
    return at;
}

static size_t line_end(const upgrade *u, size_t at) {
    while (at < u->len && !oyl_is_break((uint8_t)u->in[at])) at++;
    return at;
}

/* Past the line break at `at` */
static size_t past_break(const upgrade *u, size_t at) {
    if (at < u->len && u->in[at] == '\r') at++;
    if (at < u->len && u->in[at] == '\n') at++;
    return at;
}

/* Where a scalar's text ends: before the blanks a plain one's end takes
 * in when a comment follows */
static size_t token_end(const upgrade *u, size_t start, size_t end) {
    while (end > start && oyl_is_blank((uint8_t)u->in[end - 1])) end--;
    return end;
}

/* Where a scalar's own text starts, past its anchor and tag (and the
 * space, line breaks and comments after them) */
static size_t token_start(const upgrade *u, size_t at, size_t end) {
    const char *s = u->in;
    while (at < end && (s[at] == '&' || s[at] == '!')) {
        while (at < end && !oyl_is_blank_or_break((uint8_t)s[at])) at++;
        for (;;) {
            while (at < end && oyl_is_blank_or_break((uint8_t)s[at])) at++;
            if (at < end && s[at] == '#') {
                while (at < end && !oyl_is_break((uint8_t)s[at])) at++;
                continue;
            }
            break;
        }
    }
    return at;
}

static oyl_str explicit_tag(const oyl_tree *t, uint32_t i) {
    const oyl_node *n = &t->nodes[i];
    if (!n->props || n->kind == OYL_NODE_ALIAS) return (oyl_str){ NULL, 0 };
    const oyl_node_props *pr = &t->props[n->props];
    return pr->tag_len ? (oyl_str){ t->pool + pr->tag, pr->tag_len } : (oyl_str){ NULL, 0 };
}

static bool str_is(oyl_str s, const char *t) {
    size_t n = strlen(t);
    return s.len == n && memcmp(s.data, t, n) == 0;
}

/* ── Spellings ───────────────────────────────────────────── */

static void sput(upgrade *u, const char *s, size_t n) {
    if (!grow((void **)&u->scratch, &u->scap, u->slen + n + 1, 1)) { u->oom = true; return; }
    memcpy(u->scratch + u->slen, s, n);
    u->slen += n;
}

static void sputs(upgrade *u, const char *s) { sput(u, s, strlen(s)); }

/* A float both readers read back: the shortest digits, with a dot and a
 * signed exponent, as PyYAML needs */
static void put_float(upgrade *u, double f) {
    if (isnan(f)) { sputs(u, ".nan"); return; }
    if (isinf(f)) { sputs(u, f > 0 ? ".inf" : "-.inf"); return; }
    if (f == 0) { sputs(u, signbit(f) ? "-0.0" : "0.0"); return; }
    char d[24];
    int e;
    int n = oyl_shortest_digits(fabs(f), false, d, &e);
    if (f < 0) sputs(u, "-");
    if (e >= -5 && e < 16) {
        if (e >= 0) {
            if (n <= e + 1) {
                sput(u, d, (size_t)n);
                for (int i = n; i <= e; i++) sputs(u, "0");
                sputs(u, ".0");
            } else {
                sput(u, d, (size_t)e + 1);
                sputs(u, ".");
                sput(u, d + e + 1, (size_t)(n - e - 1));
            }
        } else {
            sputs(u, "0.");
            for (int i = 0; i < -e - 1; i++) sputs(u, "0");
            sput(u, d, (size_t)n);
        }
    } else {
        char ex[16];
        sput(u, d, 1);
        sputs(u, ".");
        if (n > 1) sput(u, d + 1, (size_t)n - 1); else sputs(u, "0");
        snprintf(ex, sizeof ex, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
        sputs(u, ex);
    }
}

/* Text in double quotes, escaped as both readers read it */
static void put_quoted(upgrade *u, oyl_str s) {
    sputs(u, "\"");
    const unsigned char *b = (const unsigned char *)s.data;
    for (size_t i = 0; i < s.len; i++) {
        unsigned char c = b[i];
        char esc[8];
        if (c == '"') sputs(u, "\\\"");
        else if (c == '\\') sputs(u, "\\\\");
        else if (c == '\n') sputs(u, "\\n");
        else if (c == '\t') sputs(u, "\\t");
        else if (c == '\r') sputs(u, "\\r");
        else if (c < 0x20 || c == 0x7F) { snprintf(esc, sizeof esc, "\\x%02X", c); sputs(u, esc); }
        else if (c == 0xC2 && i + 1 < s.len && b[i + 1] >= 0x80 && b[i + 1] <= 0x9F) {
            snprintf(esc, sizeof esc, "\\x%02X", b[i + 1]);   /* C1 controls, NEL among them */
            sputs(u, esc);
            i++;
        } else if (c == 0xE2 && i + 2 < s.len && b[i + 1] == 0x80 && (b[i + 2] == 0xA8 || b[i + 2] == 0xA9)) {
            sputs(u, b[i + 2] == 0xA8 ? "\\L" : "\\P");
            i += 2;
        } else if (c == 0xEF && i + 2 < s.len && b[i + 1] == 0xBB && b[i + 2] == 0xBF) {
            sputs(u, "\\uFEFF");
            i += 2;
        } else {
            sput(u, (const char *)b + i, 1);
        }
    }
    sputs(u, "\"");
}

/* How a document writes tag `tag` (yaml.org's) and a space: "!!int ",
 * unless the document gives !! another prefix */
static size_t tag_text(const oyl_tree *t, oyl_str tag, char *buf, size_t cap) {
    oyl_str h, pf;
    bool own = false;
    for (int i = 0; oyl_tree_tag_directive(t, i, &h, &pf); i++)
        if (str_is(h, "!!")) own = true;
    size_t pre = strlen("tag:yaml.org,2002:");
    int n = own ? snprintf(buf, cap, "!<%.*s> ", (int)tag.len, tag.data)
                : snprintf(buf, cap, "!!%.*s ", (int)(tag.len - pre), tag.data + pre);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* Does the text look like an octal or hex int, after a sign */
static bool looks_octal(oyl_str s) {
    size_t i = s.len && (s.data[0] == '-' || s.data[0] == '+') ? 1 : 0;
    if (i + 1 >= s.len || s.data[i] != '0') return false;
    char c = s.data[i + 1];
    return (c >= '0' && c <= '7') || c == '_' || c == 'o';
}

static bool looks_hex(oyl_str s) {
    size_t i = s.len && (s.data[0] == '-' || s.data[0] == '+') ? 1 : 0;
    return i + 1 < s.len && s.data[i] == '0' && (s.data[i + 1] == 'x' || s.data[i + 1] == 'X');
}

/* A candidate spelling, in the scratch buffer: its text as read, style,
 * and the tag it adds (none if empty) */
typedef struct {
    oyl_scalar_style style;
    oyl_str          tag;
    size_t           value_at, value_len;   /* the text as read, in scratch */
} spelling;

/* Does every reader, and YAML 1.2, read the candidate in scratch as
 * `target`? `own` is the node's written tag, which stays */
static bool reads_as(upgrade *u, const spelling *sp, oyl_str own, const reading *target) {
    oyl_str text = { u->scratch + sp->value_at, sp->value_len };
    oyl_str tag = own.len ? own : sp->tag;
    for (int r = 0; r < u->nsrc; r++) {
        reading x = read_as(&u->src[r], text, sp->style, tag);
        if (!same(&x, target)) return false;
    }
    reading x = read_as(&u->core, text, sp->style, tag);
    return same(&x, target);
}

/* Find a spelling every reader reads as `target`, for scalar i of doc d
 * (written `text`, tagged `own`); the whole new text in the arena, or
 * false. `octal`: the target is the source's reading of an octal int. */
static bool spell(upgrade *u, const oyl_tree *t, oyl_str text, oyl_str own, const reading *target,
                  bool octal, oyl_str *out) {
    const oyl_value *v = &target->key.value;
    oyl_str tag = target->key.tag;
    char num[64];
    /* candidates: text, its style, the tag it adds */
    for (int c = 0; c < 4; c++) {
        u->slen = 0;
        spelling sp = { OYL_SCALAR_PLAIN, { NULL, 0 }, 0, 0 };
        bool any = true;
        switch (v->kind) {
        case OYL_VALUE_NULL:
            if (c > 0) return false;
            sputs(u, "null");
            break;
        case OYL_VALUE_BOOL:
            if (c > 0) return false;
            sputs(u, v->as.b ? "true" : "false");
            break;
        case OYL_VALUE_INT:
        case OYL_VALUE_UINT: {
            bool neg = v->kind == OYL_VALUE_INT && v->as.i < 0;
            unsigned long long m = v->kind == OYL_VALUE_UINT ? v->as.u
                                 : neg ? (unsigned long long)0 - (unsigned long long)v->as.i
                                       : (unsigned long long)v->as.i;
            if (c == 0 && octal && !neg) snprintf(num, sizeof num, "0o%llo", m);
            else if (c == 1 && octal && !neg && !own.len) { snprintf(num, sizeof num, "0o%llo", m); sp.tag = OYL_TAG_INT; }
            else if (c == 2 && looks_hex(text)) {
                size_t k = 0;
                for (size_t i = 0; i < text.len && k + 1 < sizeof num; i++)
                    if (text.data[i] != '_') num[k++] = text.data[i];
                num[k] = '\0';
            } else if (c == 3) snprintf(num, sizeof num, "%s%llu", neg ? "-" : "", m);
            else any = false;
            if (any) sputs(u, num);
            break;
        }
        case OYL_VALUE_FLOAT:
            if (c > 0) return false;
            put_float(u, v->as.f);
            break;
        default:   /* text */
            if (oyl_key_tag_eq(tag, OYL_TAG_TIMESTAMP)) {
                if (c > 0 || own.len) return false;
                sput(u, text.data, text.len);
                sp.tag = OYL_TAG_TIMESTAMP;
            } else if (oyl_key_tag_eq(tag, OYL_TAG_STR)) {
                if (c > 0) return false;
                put_quoted(u, v->as.s);
                sp.style = OYL_SCALAR_DOUBLE_QUOTED;
            } else {
                return false;
            }
        }
        if (!any) continue;
        if (u->oom) return false;
        /* what it reads as: a quoted one's content */
        size_t written = u->slen;
        if (sp.style == OYL_SCALAR_DOUBLE_QUOTED) {
            sput(u, v->as.s.data, v->as.s.len);
            sp.value_at = written;
            sp.value_len = v->as.s.len;
        } else {
            sp.value_at = 0;
            sp.value_len = written;
        }
        if (u->oom) return false;
        if (!reads_as(u, &sp, own, target)) continue;
        /* the new text: the added tag, then the spelling */
        char tb[64];
        size_t tl = sp.tag.len ? tag_text(t, sp.tag, tb, sizeof tb) : 0;
        char *d = oyl_arena_alloc(u->arena, tl + written + 1, 1);
        if (!d) { u->oom = true; return false; }
        memcpy(d, tb, tl);
        memcpy(d + tl, u->scratch, written);
        d[tl + written] = '\0';
        *out = (oyl_str){ d, tl + written };
        return true;
    }
    return false;
}

/* A reading, for a message */
static const char *describe(upgrade *u, const reading *r, char *buf, size_t cap) {
    const oyl_value *v = &r->key.value;
    if (!r->ok) return "an error";
    if (oyl_key_tag_eq(r->key.tag, OYL_TAG_TIMESTAMP)) return "a timestamp";
    switch (v->kind) {
    case OYL_VALUE_NULL: return "null";
    case OYL_VALUE_BOOL: return v->as.b ? "true" : "false";
    case OYL_VALUE_INT:  snprintf(buf, cap, "%lld", (long long)v->as.i); return buf;
    case OYL_VALUE_UINT: snprintf(buf, cap, "%llu", (unsigned long long)v->as.u); return buf;
    case OYL_VALUE_FLOAT: {
        u->slen = 0;
        put_float(u, v->as.f);
        size_t n = u->slen < cap - 1 ? u->slen : cap - 1;
        if (u->scratch && n) memcpy(buf, u->scratch, n);
        buf[n] = '\0';
        return buf;
    }
    default: {   /* text: an int beyond 64 bits as its digits, a string quoted */
        int n = v->as.s.len > 40 ? 40 : (int)v->as.s.len;
        bool number = oyl_key_tag_eq(r->key.tag, OYL_TAG_INT) || oyl_key_tag_eq(r->key.tag, OYL_TAG_FLOAT);
        snprintf(buf, cap, number ? "%.*s%s" : "\"%.*s%s\"", n, v->as.s.data, v->as.s.len > 40 ? "..." : "");
        return buf;
    }
    }
}

/* ── Scalars ─────────────────────────────────────────────── */

static const char *src_name(const upgrade *u) {
    return u->nsrc == 2 ? "PyYAML and go-yaml v2" : u->src[0].name;
}

/* Decide scalar i of doc d: rewrite it, or report it, or nothing */
static void decide(upgrade *u, doc *d, uint32_t i) {
    const oyl_tree *t = d->tree;
    const oyl_node *n = &t->nodes[i];
    node_plan *pl = &d->plan[i];
    oyl_str text = oyl_node_text(t, i);
    oyl_str own = explicit_tag(t, i);
    oyl_scalar_style style = (oyl_scalar_style)n->style;
    bool is_key = (n->flags & OYL_NODE_KEY) != 0;

    reading s[2], c = read_as(&u->core, text, style, own);
    memset(s, 0, sizeof s);
    for (int r = 0; r < u->nsrc; r++) {
        s[r] = read_as(&u->src[r], text, style, own);
        /* one its tag rejects errs in the source already */
        if (!s[r].ok) { pl->what = N_SKIP; return; }
    }
    pl->what = N_SAME;
    pl->expect = s[0].key;
    /* << stays, as the user decided */
    for (int r = 0; r < u->nsrc; r++)
        if (oyl_key_tag_eq(s[r].key.tag, OYL_TAG_MERGE)) { pl->what = N_SKIP; return; }

    bool agree = u->nsrc == 1 || same(&s[0], &s[1]);
    if (agree && same(&s[0], &c)) return;

    size_t base = t->base;
    size_t tok = token_start(u, base + n->start, base + n->end);
    size_t end = token_end(u, tok, base + n->end);
    char b1[64], b2[64], b3[64];
    pl->what = N_SKIP;
    for (int r = 0; r < u->nsrc; r++)
        if (oyl_key_tag_eq(s[r].key.tag, OYL_TAG_VALUE)) {
            add_fix(u, OYL_FIX_SCALAR, tok, end, (oyl_str){ NULL, 0 }, false,
                    "YAML 1.1's = key has no YAML 1.2 spelling: left");
            return;
        }

    const reading *target;
    const char *why;
    if (!agree) {
        if (is_key && !(u->flags & OYL_UPGRADE_KEYS_KEEP_VALUE) && c.ok) {
            target = &c;
            why = keepf(u, "PyYAML: %s; go-yaml v2: %s; YAML 1.2: %s; the text kept",
                        describe(u, &s[0], b1, sizeof b1), describe(u, &s[1], b2, sizeof b2),
                        describe(u, &c, b3, sizeof b3));
        } else {
            add_fix(u, OYL_FIX_SCALAR, tok, end, (oyl_str){ NULL, 0 }, false,
                    keepf(u, "PyYAML: %s; go-yaml v2: %s: name the dialect to fix this",
                          describe(u, &s[0], b1, sizeof b1), describe(u, &s[1], b2, sizeof b2)));
            return;
        }
    } else {
        bool keep_src = is_key ? (u->flags & OYL_UPGRADE_KEYS_KEEP_VALUE) != 0
                               : !(u->flags & OYL_UPGRADE_VALUES_KEEP_TEXT);
        if (!c.ok) {
            keep_src = true;
            why = keepf(u, "%s: %s; YAML 1.2 rejects it: the value kept", src_name(u),
                        describe(u, &s[0], b1, sizeof b1));
        } else {
            why = keepf(u, "%s: %s; YAML 1.2: %s; %s", src_name(u), describe(u, &s[0], b1, sizeof b1),
                        describe(u, &c, b2, sizeof b2),
                        keep_src ? "the value kept" : is_key ? "the text kept" : "YAML 1.2's reading kept");
        }
        target = keep_src ? &s[0] : &c;
    }
    if (u->oom) return;
    bool octal = target == &s[0] && (target->key.value.kind == OYL_VALUE_INT) && looks_octal(text);
    oyl_str spelled;
    if (!spell(u, t, text, own, target, octal, &spelled)) {
        if (!u->oom)
            add_fix(u, OYL_FIX_SCALAR, tok, end, (oyl_str){ NULL, 0 }, false,
                    keepf(u, "%s; no spelling every reader reads so: left", why));
        return;
    }
    if (!add_fix(u, OYL_FIX_SCALAR, tok, end, spelled, true, why)) return;
    pl->what = N_CHANGED;
    pl->fix = u->nfixes;
    pl->expect = target->key;
}

/* Do the source's readers all read scalars a and b as one key? */
static bool same_in_source(upgrade *u, const oyl_tree *t, uint32_t a, uint32_t b) {
    for (int r = 0; r < u->nsrc; r++) {
        reading x = read_as(&u->src[r], oyl_node_text(t, a), (oyl_scalar_style)t->nodes[a].style,
                            explicit_tag(t, a));
        reading y = read_as(&u->src[r], oyl_node_text(t, b), (oyl_scalar_style)t->nodes[b].style,
                            explicit_tag(t, b));
        if (!x.ok || !y.ok || !same(&x, &y)) return false;
    }
    return true;
}

/* Undo the rewrite of keys that would equal another key of their mapping,
 * one the source read as another key (two keys the source already read as
 * one stay one, rewritten alike) */
static void key_clashes(upgrade *u, doc *d, uint32_t m) {
    const oyl_tree *t = d->tree;
    const oyl_node *nodes = t->nodes;
    uint32_t npairs = nodes[m].len;
    uint32_t k = m + 1;
    for (uint32_t pi = 0; pi < npairs; pi++) {
        uint32_t v = k + nodes[k].span;
        if (nodes[k].kind == OYL_NODE_SCALAR && d->plan[k].what == N_CHANGED) {
            bool clash = false;
            uint32_t o = m + 1;
            for (uint32_t pj = 0; pj < npairs && !clash; pj++) {
                uint32_t ov = o + nodes[o].span;
                if (o != k && nodes[o].kind == OYL_NODE_SCALAR && d->plan[o].what != N_REMOVED &&
                    !same_in_source(u, t, k, o)) {
                    oyl_str text = oyl_node_text(t, o), own = explicit_tag(t, o);
                    oyl_scalar_style st = (oyl_scalar_style)nodes[o].style;
                    reading want = { true, d->plan[k].expect };
                    if (d->plan[o].what == N_CHANGED) {
                        reading x = { true, d->plan[o].expect };
                        clash = same(&x, &want);
                    } else {
                        for (int r = 0; r <= u->nsrc && !clash; r++) {
                            reading x = read_as(r < u->nsrc ? &u->src[r] : &u->core, text, st, own);
                            clash = x.ok && same(&x, &want);
                        }
                    }
                }
                o = ov + nodes[ov].span;
            }
            if (clash) {
                fix_rec *r = &u->fixes[d->plan[k].fix - 1];
                r->fix.fixed = false;
                r->fix.after = (oyl_str){ "", 0 };
                r->fix.message = keepf(u, "%s; left, as the rewrite would make two keys equal",
                                       r->fix.message);
                d->plan[k].what = N_SKIP;
            }
        }
        k = v + nodes[v].span;
    }
}

/* ── Merges ──────────────────────────────────────────────── */

static bool is_merge_key(const oyl_tree *t, uint32_t k) {
    const oyl_node *n = &t->nodes[k];
    if (n->kind != OYL_NODE_SCALAR || n->style != OYL_SCALAR_PLAIN) return false;
    oyl_str own = explicit_tag(t, k);
    return str_is(oyl_node_text(t, k), "<<") && (!own.len || oyl_key_tag_eq(own, OYL_TAG_MERGE));
}

/* Can value v be merged: a mapping, or a sequence of them (through
 * aliases)? A << whose value can't is an ordinary key. */
static bool mergeable(const oyl_tree *t, uint32_t v) {
    v = oyl_node_target(t, v);
    const oyl_node *n = &t->nodes[v];
    if (n->kind == OYL_NODE_MAPPING) return true;
    if (n->kind != OYL_NODE_SEQUENCE) return false;
    uint32_t c = v + 1;
    for (uint32_t i = 0; i < n->len; i++) {
        if (t->nodes[oyl_node_target(t, c)].kind != OYL_NODE_MAPPING) return false;
        c += t->nodes[c].span;
    }
    return true;
}

/* Scalar n (or an alias to one) as a key: its tag and value, as trees
 * compare keys */
static oyl_key key_of(const oyl_tree *t, uint32_t n) {
    uint32_t s = oyl_node_target(t, n);
    oyl_str text = oyl_node_text(t, s);
    oyl_value v;
    oyl_status st = oyl_node_value(t, s, &v);
    oyl_key k;
    oyl_key_make(&k, oyl_key_tag(t->schema, oyl_node_tag(t, s), text), text, st, &v);
    return k;
}

/* Has mapping m, its merges applied, key `key`? */
static bool has_key(upgrade *u, const oyl_tree *t, uint32_t m, const oyl_key *key) {
    const oyl_pair *pairs;
    size_t n;
    oyl_arena_reset(u->tmp);
    oyl_status st = oyl_node_entries(t, m, 0, u->tmp, &pairs, &n);
    if (st == OYL_ERR_MEMORY) u->oom = true;
    if (st != OYL_OK) return false;
    for (size_t i = 0; i < n; i++) {
        if (t->nodes[oyl_node_target(t, pairs[i].key)].kind != OYL_NODE_SCALAR) continue;
        oyl_key k = key_of(t, pairs[i].key);
        if (oyl_key_cmp(&k, key) == 0) return true;
    }
    return false;
}

/* Is key k (a scalar, or an alias to one) found in what merge value v
 * merges in? */
static bool merged_in(upgrade *u, const oyl_tree *t, uint32_t v, uint32_t k) {
    oyl_key key = key_of(t, k);
    v = oyl_node_target(t, v);
    const oyl_node *n = &t->nodes[v];
    if (n->kind == OYL_NODE_MAPPING) return has_key(u, t, v, &key);
    if (n->kind != OYL_NODE_SEQUENCE) return false;
    uint32_t c = v + 1;
    for (uint32_t i = 0; i < n->len; i++) {
        uint32_t s = oyl_node_target(t, c);
        if (t->nodes[s].kind == OYL_NODE_MAPPING && has_key(u, t, s, &key)) return true;
        c += t->nodes[c].span;
    }
    return false;
}

/* Comment out pair (k, v) of block mapping m, line by line, if it has its
 * lines to itself and no anchor; false if it can't be */
static bool comment_out(upgrade *u, doc *d, uint32_t k, uint32_t v) {
    const oyl_tree *t = d->tree;
    size_t base = t->base;
    size_t from = line_start(u, base + t->nodes[k].start);
    for (size_t i = from; i < base + t->nodes[k].start; i++)
        if (u->in[i] != ' ') return false;
    size_t last = base + t->nodes[v].end;
    if (base + t->nodes[k].end > last) last = base + t->nodes[k].end;
    /* a block scalar's end takes in the empty lines after it */
    while (last > from && oyl_is_blank_or_break((uint8_t)u->in[last - 1])) last--;
    size_t to = line_end(u, last);
    for (size_t i = last; i < to; i++) {
        if (u->in[i] == '#') break;
        if (!oyl_is_blank((uint8_t)u->in[i])) return false;
    }
    for (uint32_t x = k; x < v + t->nodes[v].span; x++)
        if (t->nodes[x].props && t->nodes[x].kind != OYL_NODE_ALIAS &&
            t->props[t->nodes[x].props].anchor_len)
            return false;
    /* "# " before each line's first character */
    u->slen = 0;
    size_t i = from;
    while (i < to) {
        size_t e = line_end(u, i);
        size_t c = i;
        while (c < e && oyl_is_blank((uint8_t)u->in[c])) c++;
        sput(u, u->in + i, c - i);
        if (c < e) sputs(u, "# ");
        sput(u, u->in + c, e - c);
        size_t nx = e < to ? past_break(u, e) : e;
        sput(u, u->in + e, nx - e);
        i = nx;
    }
    if (u->oom) return false;
    add_fix(u, OYL_FIX_MERGE, from, to, keep(u, u->scratch, u->slen), true,
            "go-yaml v2's merge overrides this key, YAML 1.2's doesn't: commented out, "
            "as go-yaml v2 read it");
    for (uint32_t x = k; x < v + t->nodes[v].span; x++) d->plan[x].what = N_REMOVED;
    return true;
}

/* Keys written before a << whose merge has them: go-yaml v2 reads the
 * merged value, PyYAML and YAML 1.2 the written one */
static void merges(upgrade *u, doc *d, uint32_t m) {
    const oyl_tree *t = d->tree;
    const oyl_node *nodes = t->nodes;
    uint32_t npairs = nodes[m].len;
    /* the << pairs' keys, usually none: then nothing to do */
    uint32_t mk[8], nmk = 0, last_mk = 0;
    bool many = false;
    for (uint32_t pi = 0, k = m + 1; pi < npairs; pi++) {
        uint32_t v = k + nodes[k].span;
        if (is_merge_key(t, k) && mergeable(t, v)) {
            /* go-yaml v2 rejects the merge of an alias to a sequence, which
             * YAML's merge takes: nothing to keep, left */
            if (nodes[v].kind == OYL_NODE_ALIAS && nodes[oyl_node_target(t, v)].kind == OYL_NODE_SEQUENCE) {
                add_fix(u, OYL_FIX_MERGE, t->base + nodes[k].start, t->base + nodes[v].end, (oyl_str){ NULL, 0 },
                        false, "go-yaml v2 rejects a merge of an alias to a sequence, YAML's merge takes it: left");
                d->plan[m].what = N_SKIP;
                return;
            }
            if (nmk < 8) mk[nmk++] = k; else many = true;
            last_mk = k;
        }
        k = v + nodes[v].span;
    }
    if (!nmk) return;
    uint32_t k = m + 1;
    for (uint32_t pi = 0; pi < npairs && k < last_mk; pi++) {
        uint32_t v = k + nodes[k].span;
        uint32_t next = v + nodes[v].span;
        if (!(is_merge_key(t, k) && mergeable(t, v)) && nodes[oyl_node_target(t, k)].kind == OYL_NODE_SCALAR) {
            bool over = false;
            if (!many) {
                for (uint32_t j = 0; j < nmk && !over; j++)
                    if (mk[j] > k) over = merged_in(u, t, mk[j] + nodes[mk[j]].span, k);
            } else {   /* more than 8 merges: each later pair */
                uint32_t o = next;
                for (uint32_t pj = pi + 1; pj < npairs && !over; pj++) {
                    uint32_t ov = o + nodes[o].span;
                    if (is_merge_key(t, o) && mergeable(t, ov)) over = merged_in(u, t, ov, k);
                    o = ov + nodes[ov].span;
                }
            }
            if (over) {
                size_t at = t->base + nodes[k].start, to = t->base + nodes[v].end;
                if (to < at) to = at;
                if (u->merge_disagree) {
                    add_fix(u, OYL_FIX_MERGE, at, to, (oyl_str){ NULL, 0 }, false,
                            "PyYAML reads this key, go-yaml v2 the value its merge brings: "
                            "name the dialect to fix this");
                    d->plan[m].what = N_SKIP;
                } else if (!(nodes[m].flags & OYL_NODE_FLOW) && comment_out(u, d, k, v)) {
                    /* done */
                } else if (!u->oom) {
                    add_fix(u, OYL_FIX_MERGE, at, to, (oyl_str){ NULL, 0 }, false,
                            "go-yaml v2's merge overrides this key, YAML 1.2's doesn't, and it "
                            "can't be commented out (it shares a line or holds an anchor): left");
                    d->plan[m].what = N_SKIP;
                }
            }
        }
        k = next;
    }
}

/* ── Libyaml's leniencies, and %YAML 1.1 ─────────────────── */

/* \' in double-quoted scalar i, each at its backslash: count them, or
 * fix them when `fix` */
static size_t squote_escapes(upgrade *u, doc *d, uint32_t i, bool fix) {
    const oyl_tree *t = d->tree;
    const oyl_node *n = &t->nodes[i];
    size_t end = t->base + n->end, found = 0;
    size_t at = token_start(u, t->base + n->start, end);
    for (at++; at + 1 < end; at++) {
        if (u->in[at] != '\\') continue;
        if (u->in[at + 1] == '\'') {
            found++;
            if (fix && !covered(u, at, at + 1))
                add_fix(u, OYL_FIX_ESCAPE, at, at + 1, (oyl_str){ "", 0 }, true,
                        "\\' in double quotes, go-yaml v2's, is not a YAML 1.2 escape: "
                        "' alone reads the same");
        }
        at++;
    }
    return found;
}

/* The document a position is in: the last one starting before it */
static doc *doc_at(upgrade *u, size_t at) {
    size_t lo = 0, hi = u->ndocs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (u->docs[mid].tree->base <= at) lo = mid + 1; else hi = mid;
    }
    return lo ? &u->docs[lo - 1] : NULL;
}

static bool holder(const oyl_node *n) {
    return (n->kind == OYL_NODE_SCALAR && (n->style == OYL_SCALAR_SINGLE_QUOTED ||
                                           n->style == OYL_SCALAR_DOUBLE_QUOTED)) ||
           ((n->kind == OYL_NODE_SEQUENCE || n->kind == OYL_NODE_MAPPING) && (n->flags & OYL_NODE_FLOW));
}

/* Lines of quoted scalars or flow collections not indented past their
 * block (the warnings at `at`, in order): each indented one past the
 * column where the innermost such scalar or collection starts. One sweep
 * of each document's nodes, the scalars and collections that hold the
 * line on a stack. */
static void indent_lines(upgrade *u, const size_t *at, size_t n) {
    uint32_t *stack = NULL;
    size_t sp = 0, scap = 0;
    doc *cur = NULL;
    uint32_t next = 0;
    for (size_t w = 0; w < n && !u->oom; w++) {
        size_t from = at[w];
        doc *d = doc_at(u, from);
        if (!d || d->left) continue;
        const oyl_tree *t = d->tree;
        if (d != cur) { cur = d; next = 0; sp = 0; }
        size_t c = from;
        while (c < u->len && oyl_is_blank((uint8_t)u->in[c])) c++;
        for (; next < t->len && t->base + t->nodes[next].start < from; next++) {
            if (!holder(&t->nodes[next])) continue;
            while (sp && t->base + t->nodes[stack[sp - 1]].end <= t->base + t->nodes[next].start) sp--;
            if (!grow((void **)&stack, &scap, sp + 1, sizeof *stack)) { u->oom = true; break; }
            stack[sp++] = next;
        }
        while (sp && t->base + t->nodes[stack[sp - 1]].end <= c) sp--;
        if (!sp || u->oom || covered(u, from, c)) continue;
        size_t s = t->base + t->nodes[stack[sp - 1]].start;
        size_t col = s - line_start(u, s) + 1;
        u->slen = 0;
        for (size_t k = 0; k < col; k++) sputs(u, " ");
        if (u->oom) break;
        add_fix(u, OYL_FIX_INDENT, from, c, keep(u, u->scratch, u->slen), true,
                "a line not indented past its block, or with a tab, which libyaml reads and YAML 1.2 "
                "doesn't: indented");
    }
    free(stack);
}

/* A block scalar ending the input without a line break: libyaml gives it
 * no final line feed, YAML 1.2 one */
static void chomp(upgrade *u, size_t at) {
    doc *d = doc_at(u, at);
    if (!d || d->left || covered(u, at, at + 1)) return;
    const oyl_tree *t = d->tree;
    uint32_t i = 0;
    for (; i < t->len; i++) {
        const oyl_node *n = &t->nodes[i];
        if (n->kind == OYL_NODE_SCALAR && (n->style == OYL_SCALAR_LITERAL || n->style == OYL_SCALAR_FOLDED) &&
            token_start(u, t->base + n->start, t->base + n->end) == at)
            break;
    }
    if (i == t->len) return;
    oyl_str text = oyl_node_text(t, i);
    if (u->flags & OYL_UPGRADE_VALUES_KEEP_TEXT) {
        /* YAML 1.2's reading: a line break at the end, for every reader */
        u->slen = 0;
        sput(u, text.data, text.len);
        sputs(u, "\n");
        oyl_str with = keep(u, u->scratch, u->slen);
        if (d->plan[i].what == N_SAME || d->plan[i].what == N_CHANGED) {   /* its tag stays */
            d->plan[i].what = N_CHANGED;
            d->plan[i].expect.value = (oyl_value){ .kind = OYL_VALUE_STR, .as = { .s = with } };
        }
        add_fix(u, OYL_FIX_CHOMP, u->len, u->len, keep(u, "\n", 1), true,
                "a block scalar ending the input without a line break: libyaml gives it no final "
                "line feed, YAML 1.2 one; a line break added, YAML 1.2's reading kept");
        return;
    }
    /* the value kept: when it ends with a line feed, the last line was an
     * empty one (after |+, keeping the line feeds before it): removed; else
     * | becomes |-, |+ |- */
    size_t ls = line_start(u, u->len);
    if (text.len && text.data[text.len - 1] == '\n' && ls > at && ls < u->len) {
        add_fix(u, OYL_FIX_CHOMP, ls, u->len, (oyl_str){ "", 0 }, true,
                "a block scalar ending the input with an empty line without a line break: libyaml "
                "gives it no line feed, YAML 1.2 one; removed, the value kept");
        return;
    }
    size_t h = at + 1;
    for (int i = 0; i < 2 && h < u->len; i++, h++)
        if (u->in[h] == '+') {
            add_fix(u, OYL_FIX_CHOMP, h, h + 1, keep(u, "-", 1), true,
                    "a block scalar ending the input without a line break: libyaml gives it no "
                    "final line feed, YAML 1.2 one; stripped, the value kept");
            return;
        } else if (!(u->in[h] >= '1' && u->in[h] <= '9')) {
            break;
        }
    add_fix(u, OYL_FIX_CHOMP, at + 1, at + 1, keep(u, "-", 1), true,
            "a block scalar ending the input without a line break: libyaml gives it no final "
            "line feed, YAML 1.2 one; stripped, the value kept");
}

/* %YAML 1.1 before document k: removed, with its line */
static void directive(upgrade *u, size_t k) {
    const oyl_tree *t = u->docs[k].tree;
    int major, minor;
    if (!oyl_tree_yaml_version(t, &major, &minor) || major != 1 || minor != 1) return;
    size_t at = k ? u->docs[k - 1].tree->base : 0, found = SIZE_MAX;
    if (line_start(u, at) != at) at = past_break(u, line_end(u, at));
    while (at < t->base) {   /* the last %YAML line before the document */
        if (at + 5 < u->len && memcmp(u->in + at, "%YAML", 5) == 0 && oyl_is_blank((uint8_t)u->in[at + 5]))
            found = at;
        size_t e = line_end(u, at);
        if (e >= u->len) break;
        at = past_break(u, e);
    }
    if (found == SIZE_MAX) return;
    add_fix(u, OYL_FIX_DIRECTIVE, found, past_break(u, line_end(u, found)), (oyl_str){ "", 0 }, true,
            "%YAML 1.1 removed: a YAML 1.2 reader would read the document as 1.1, and go-yaml v2 "
            "rejects %YAML 1.2; without it each reads its own version, alike");
}

/* ── Checking the output ─────────────────────────────────── */

static oyl_status fail(upgrade *u, size_t at, const char *msg) {
    oyl_parser_fail(u->p, OYL_ERR_PARSE, mark_at(u, at), msg);
    return OYL_ERR_PARSE;
}

/* The output, read by dialect `dl` (reader `r`), against the plans: each
 * document's trees in `trees` */
static oyl_status check_one(upgrade *u, oyl_str out, const oyl_dialect *dl, const reader *r,
                            oyl_arena *a, oyl_tree **trees) {
    oyl_parser *q = oyl_parser_new(out.data, out.len, a);
    if (!q) return OYL_ERR_MEMORY;
    oyl_parser_set_dialect(q, dl);
    for (size_t k = 0; k <= u->ndocs; k++) {
        oyl_tree *ot = NULL;
        oyl_status st = oyl_parse_tree(q, &ot);
        if (st == OYL_ERR_MEMORY) { oyl_parser_free(q); return st; }
        if (st != OYL_OK || (k < u->ndocs) != (ot != NULL)) {
            char msg[384];
            snprintf(msg, sizeof msg, "oyl_upgrade's output doesn't read as %s: %s (a bug in "
                     "oyl_upgrade)", r->name, st != OYL_OK ? oyl_parser_error(q) : "documents differ");
            oyl_tree_free(ot);
            oyl_parser_free(q);
            return fail(u, k < u->ndocs ? u->docs[k].tree->base : u->len, msg);
        }
        if (k == u->ndocs) break;
        trees[k] = ot;
        doc *d = &u->docs[k];
        const oyl_tree *it = d->tree;
        uint32_t j = 0;
        for (uint32_t i = 0; i < it->len; i++) {
            if (!d->left && d->plan[i].what == N_REMOVED) continue;
            if (j >= ot->len || ot->nodes[j].kind != it->nodes[i].kind) {
                oyl_parser_free(q);
                return fail(u, it->base + it->nodes[i].start,
                            "oyl_upgrade's output has another structure (a bug in oyl_upgrade)");
            }
            const node_plan *pl = &d->plan[i];
            if (!d->left && it->nodes[i].kind == OYL_NODE_SCALAR &&
                (pl->what == N_SAME || pl->what == N_CHANGED)) {
                reading x = read_as(r, oyl_node_text(ot, j), (oyl_scalar_style)ot->nodes[j].style,
                                    explicit_tag(ot, j));
                reading want = { true, pl->expect };
                if (!same(&x, &want)) {
                    char msg[384], b1[64], b2[64];
                    snprintf(msg, sizeof msg, "after oyl_upgrade %s reads %s here, not %s (a bug in "
                             "oyl_upgrade)", r->name, describe(u, &x, b1, sizeof b1),
                             describe(u, &want, b2, sizeof b2));
                    oyl_parser_free(q);
                    return fail(u, it->base + it->nodes[i].start, msg);
                }
            }
            j++;
        }
        if (j != ot->len) {
            oyl_parser_free(q);
            return fail(u, it->base, "oyl_upgrade's output has another structure (a bug in oyl_upgrade)");
        }
    }
    oyl_parser_free(q);
    return OYL_OK;
}

/* Every mapping as a program reads it (merges applied) alike in each tree */
static oyl_status check_merges(upgrade *u, oyl_tree **trees[], int n, oyl_arena *a) {
    for (size_t k = 0; k < u->ndocs; k++) {
        doc *d = &u->docs[k];
        if (d->left) continue;
        /* output node j's input node, for the mappings whose merges disagree */
        const oyl_tree *it = d->tree;
        uint32_t j = 0;
        for (uint32_t i = 0; i < it->len; i++) {
            if (d->plan[i].what == N_REMOVED) continue;
            bool merging = false, checked = d->plan[i].what != N_SKIP;
            if (it->nodes[i].kind == OYL_NODE_MAPPING) {
                uint32_t c = i + 1;
                for (uint32_t pi = 0; pi < it->nodes[i].len; pi++) {
                    if (is_merge_key(it, c)) merging = true;
                    if (d->plan[c].what == N_SKIP) checked = false;
                    c += it->nodes[c].span;
                    c += it->nodes[c].span;
                }
            }
            if (merging && checked) {
                const oyl_pair *p0, *pr;
                size_t n0, nr;
                oyl_status st = oyl_node_entries(trees[0][k], j, 0, a, &p0, &n0);
                if (st == OYL_ERR_MEMORY) return st;
                for (int r = 1; r < n && st == OYL_OK; r++) {
                    oyl_status sr = oyl_node_entries(trees[r][k], j, 0, a, &pr, &nr);
                    if (sr == OYL_ERR_MEMORY) return sr;
                    if (sr != OYL_OK || nr != n0 || memcmp(pr, p0, n0 * sizeof *p0) != 0)
                        return fail(u, it->base + it->nodes[i].start,
                                    "after oyl_upgrade the readers merge this mapping otherwise "
                                    "(a bug in oyl_upgrade)");
                }
            }
            j++;
        }
    }
    return OYL_OK;
}

static oyl_status check(upgrade *u, oyl_str out) {
    int n = u->nsrc + 1;
    oyl_tree **trees[3] = { NULL, NULL, NULL };
    oyl_arena *a = oyl_arena_new(4096);
    oyl_status st = a ? OYL_OK : OYL_ERR_MEMORY;
    for (int r = 0; r < n && st == OYL_OK; r++) {
        trees[r] = calloc(u->ndocs ? u->ndocs : 1, sizeof *trees[r]);
        if (!trees[r]) { st = OYL_ERR_MEMORY; break; }
        const reader *rd = r < u->nsrc ? &u->src[r] : &u->core;
        st = check_one(u, out, r < u->nsrc ? u->check[r] : oyl_dialect_yaml12(), rd, a, trees[r]);
    }
    if (st == OYL_OK) st = check_merges(u, trees, n, a);
    for (int r = 0; r < n; r++) {
        if (!trees[r]) continue;
        for (size_t k = 0; k < u->ndocs; k++) oyl_tree_free(trees[r][k]);
        free(trees[r]);
    }
    oyl_arena_free(a);
    return st;
}

/* ── The upgrade ─────────────────────────────────────────── */

static int by_start(const void *x, const void *y) {
    const fix_rec *a = x, *b = y;
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    return (a->end > b->end) - (a->end < b->end);
}

static const char *dialect_name(const oyl_dialect *d) {
    return d == oyl_dialect_yaml11() ? "PyYAML" : d == oyl_dialect_goyaml2() ? "go-yaml v2" : "the source";
}

static oyl_status run(upgrade *u, oyl_str *out) {
    oyl_parser *p = u->p;
    const oyl_dialect *dl = oyl_parser_default_dialect(p);
    int major, minor;
    oyl_dialect_version(dl, &major, &minor);
    bool unknown = minor != 1;
    u->core = (reader){ oyl_schema_core(), false, "YAML 1.2" };
    if (unknown) {
        /* read with both 1.1 readers' leniencies; each one's types below */
        const oyl_dialect *both = oyl_dialect_new(u->arena, "1.1", oyl_schema_yaml11(),
                                                  OYL_SYNTAX_SQUOTE_ESCAPE | OYL_SYNTAX_EOF_NO_BREAK |
                                                  OYL_SYNTAX_FLOW_INDENT, OYL_MERGE_YAML);
        if (!both) return OYL_ERR_MEMORY;
        oyl_parser_set_dialect(p, both);
    } else {
        const oyl_schema *sc = oyl_dialect_schema(dl);
        u->src[0] = (reader){ sc, (u->flags & OYL_UPGRADE_TIMESTAMPS_TEXT) || dl == oyl_dialect_goyaml2() ||
                                  sc == oyl_schema_goyaml2(), dialect_name(dl) };
        u->nsrc = 1;
        u->check[0] = dl;
        u->ordered = oyl_dialect_merge(dl) == OYL_MERGE_ORDERED;
    }

    /* read every document */
    for (;;) {
        oyl_tree *t = NULL;
        oyl_status st = oyl_parse_tree(p, &t);
        if (u->oom) { oyl_tree_free(t); return OYL_ERR_MEMORY; }
        if (st != OYL_OK) return st;
        if (!t) break;
        if (!grow((void **)&u->docs, &u->docs_cap, u->ndocs + 1, sizeof *u->docs)) {
            oyl_tree_free(t);
            return OYL_ERR_MEMORY;
        }
        doc *d = &u->docs[u->ndocs++];
        d->tree = t;
        d->plan = calloc(t->len ? t->len : 1, sizeof *d->plan);
        if (!d->plan) return OYL_ERR_MEMORY;
        int dmaj, dmin;
        oyl_dialect_version(oyl_tree_dialect(t), &dmaj, &dmin);
        d->left = dmin != 1;
    }

    /* lines, for marks */
    u->nlines = 1;
    for (size_t i = 0; i < u->len; i++)
        if (u->in[i] == '\n' || (u->in[i] == '\r' && (i + 1 >= u->len || u->in[i + 1] != '\n'))) u->nlines++;
    u->lines = malloc(u->nlines * sizeof *u->lines);
    if (!u->lines) return OYL_ERR_MEMORY;
    u->lines[0] = 0;
    for (size_t i = 0, l = 1; i < u->len; i++)
        if (u->in[i] == '\n' || (u->in[i] == '\r' && (i + 1 >= u->len || u->in[i + 1] != '\n')))
            u->lines[l++] = i + 1;

    /* the reader unknown: what the file rules out */
    if (unknown) {
        bool has12 = false;
        size_t sq = 0, sq_at = 0;
        for (size_t k = 0; k < u->ndocs; k++) {
            doc *d = &u->docs[k];
            if (d->left) { has12 = true; continue; }
            for (uint32_t i = 0; i < d->tree->len; i++)
                if (d->tree->nodes[i].kind == OYL_NODE_SCALAR &&
                    d->tree->nodes[i].style == OYL_SCALAR_DOUBLE_QUOTED && !sq &&
                    (sq = squote_escapes(u, d, i, false)) != 0)
                    sq_at = d->tree->base + d->tree->nodes[i].start;
        }
        reader py = { oyl_schema_yaml11(), (u->flags & OYL_UPGRADE_TIMESTAMPS_TEXT) != 0, "PyYAML" };
        reader go = { oyl_schema_goyaml2(), true, "go-yaml v2" };
        if (sq && has12)
            return fail(u, sq_at, "neither PyYAML nor go-yaml v2 reads this file: \\' in double quotes "
                                  "is an error to PyYAML, %YAML 1.2 to go-yaml v2");
        if (sq) {
            u->src[0] = go;
            u->check[0] = oyl_dialect_goyaml2();
            u->nsrc = 1;
            u->ordered = true;
        } else if (has12) {
            u->src[0] = py;
            u->check[0] = oyl_dialect_yaml11();
            u->nsrc = 1;
        } else {
            u->src[0] = py;
            u->src[1] = go;
            u->check[0] = oyl_dialect_yaml11();
            u->check[1] = oyl_dialect_goyaml2();
            u->nsrc = 2;
            u->merge_disagree = true;
        }
    }

    /* decide: merges first, as what they comment out needs nothing else;
     * then scalars, and keys made equal */
    for (size_t k = 0; k < u->ndocs && !u->oom; k++) {
        doc *d = &u->docs[k];
        if (d->left) continue;
        if (u->ordered || u->merge_disagree)
            for (uint32_t i = 0; i < d->tree->len; i++)
                if (d->tree->nodes[i].kind == OYL_NODE_MAPPING && d->plan[i].what != N_REMOVED)
                    merges(u, d, i);
        for (uint32_t i = 0; i < d->tree->len && !u->oom; i++)
            if (d->tree->nodes[i].kind == OYL_NODE_SCALAR && d->plan[i].what != N_REMOVED)
                decide(u, d, i);
        for (uint32_t i = 0; i < d->tree->len; i++)
            if (d->tree->nodes[i].kind == OYL_NODE_MAPPING && d->plan[i].what != N_REMOVED)
                key_clashes(u, d, i);
    }
    if (u->oom) return OYL_ERR_MEMORY;
    /* the rest, outside what those replaced */
    note_cover(u);
    for (size_t k = 0; k < u->ndocs && !u->oom; k++) {
        doc *d = &u->docs[k];
        if (d->left) continue;
        for (uint32_t i = 0; i < d->tree->len; i++)
            if (d->tree->nodes[i].kind == OYL_NODE_SCALAR && d->plan[i].what != N_REMOVED &&
                d->plan[i].what != N_CHANGED && d->tree->nodes[i].style == OYL_SCALAR_DOUBLE_QUOTED)
                squote_escapes(u, d, i, true);
        directive(u, k);
    }
    size_t *ats = malloc((u->nheard ? u->nheard : 1) * sizeof *ats), nl = 0;
    if (!ats) return OYL_ERR_MEMORY;
    for (size_t h = 0; h < u->nheard && !u->oom; h++) {
        if (u->heard[h].msg == oyl_msg_flow_indent) ats[nl++] = u->heard[h].offset;
        else chomp(u, u->heard[h].offset);
    }
    if (nl) qsort(ats, nl, sizeof *ats, by_offset);
    indent_lines(u, ats, nl);
    free(ats);
    if (u->oom) return OYL_ERR_MEMORY;

    /* the output */
    if (u->nfixes) qsort(u->fixes, u->nfixes, sizeof *u->fixes, by_start);
    size_t total = u->len, last = 0;
    for (size_t f = 0; f < u->nfixes; f++) {
        fix_rec *r = &u->fixes[f];
        if (!r->fix.fixed) continue;
        if (r->start < last)
            return fail(u, r->start, "oyl_upgrade's fixes overlap here (a bug in oyl_upgrade)");
        last = r->end;
        total = total - (r->end - r->start) + r->fix.after.len;
    }
    char *buf = oyl_arena_alloc(u->arena, total + 1, 1);
    if (!buf) return OYL_ERR_MEMORY;
    size_t o = 0, at = 0;
    for (size_t f = 0; f < u->nfixes; f++) {
        fix_rec *r = &u->fixes[f];
        if (!r->fix.fixed) continue;
        if (r->start > at) memcpy(buf + o, u->in + at, r->start - at);
        o += r->start - at;
        if (r->fix.after.len) memcpy(buf + o, r->fix.after.data, r->fix.after.len);
        o += r->fix.after.len;
        at = r->end;
    }
    if (u->len > at) memcpy(buf + o, u->in + at, u->len - at);
    o += u->len - at;
    buf[o] = '\0';
    *out = (oyl_str){ buf, o };
    return check(u, *out);
}

oyl_status oyl_upgrade(oyl_parser *p, unsigned flags, oyl_str *out, const oyl_fix **fixes,
                       size_t *nfixes) {
    if (!p || !out || !fixes || !nfixes) return OYL_ERR_INPUT;
    *out = (oyl_str){ NULL, 0 };
    *fixes = NULL;
    *nfixes = 0;
    upgrade u = { .p = p, .arena = oyl_parser_arena(p), .flags = flags };
    u.in = oyl_parser_input(p, &u.len);
    u.tmp = oyl_arena_new(4096);
    if (!u.tmp) {
        oyl_parser_fail(p, OYL_ERR_MEMORY, (oyl_mark){ 0, 0, 0 }, "out of memory");
        return OYL_ERR_MEMORY;
    }
    oyl_parser_get_warn(p, &u.user_fn, &u.user_data);
    oyl_parser_set_warn(p, hear, &u);

    oyl_status st = run(&u, out);
    if (st == OYL_OK) {
        oyl_fix *arr = oyl_arena_alloc(u.arena, (u.nfixes ? u.nfixes : 1) * sizeof *arr, _Alignof(oyl_fix));
        if (!arr) st = OYL_ERR_MEMORY;
        else {
            for (size_t f = 0; f < u.nfixes; f++) arr[f] = u.fixes[f].fix;
            *fixes = arr;
            *nfixes = u.nfixes;
        }
    }
    if (st == OYL_ERR_MEMORY)
        oyl_parser_fail(p, OYL_ERR_MEMORY, (oyl_mark){ 0, 0, 0 }, "out of memory");
    if (st != OYL_OK) *out = (oyl_str){ NULL, 0 };

    oyl_parser_set_warn(p, u.user_fn, u.user_data);
    for (size_t k = 0; k < u.ndocs; k++) {
        oyl_tree_free(u.docs[k].tree);
        free(u.docs[k].plan);
    }
    free(u.docs);
    free(u.fixes);
    free(u.heard);
    free(u.lines);
    free(u.scratch);
    free(u.cover);
    oyl_arena_free(u.tmp);
    return st;
}
