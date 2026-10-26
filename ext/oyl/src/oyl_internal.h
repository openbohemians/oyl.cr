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
#include <math.h>
#include <string.h>

/* ── Schema ──────────────────────────────────────────────── */

/* How a rule parses a scalar */
typedef enum {
    RULE_WORD,         /* the text is `pattern`; the value is `value` */
    RULE_WORD_ICASE,   /* the same, ignoring ASCII case */
    RULE_INT,          /* the built-in int parser, with OYL_INT_ `flags` */
    RULE_FLOAT,        /* the built-in float parser, with OYL_FLOAT_ `flags` */
    RULE_TIMESTAMP,    /* go-yaml v2's timestamps; the value is the text */
    RULE_CUSTOM,       /* `parse(text, out, user)` */
    RULE_NONE,         /* never accepts (an unknown builtin name) */
} oyl_rule_kind;

/* A single tag resolution rule: a plain scalar the rule's parser accepts
 * resolves to `tag`, and the parser gives its value. */
typedef struct {
    oyl_rule_kind     kind;
    const char       *pattern;   /* the word, for RULE_WORD and RULE_WORD_ICASE */
    size_t            plen;      /* its length */
    oyl_str           tag;
    unsigned          flags;
    oyl_value         value;     /* a word's value (STR means the text) */
    oyl_scalar_parser parse;
    void             *user;
} oyl_schema_rule;

/* How a schema reads an explicitly tagged scalar */
typedef enum {
    OYL_TAGGED_RULES,     /* by the rules bound to the tag (YAML 1.2, built schemas) */
    OYL_TAGGED_PYYAML,    /* as PyYAML's constructors do (YAML 1.1) */
    OYL_TAGGED_GOYAML2,   /* as go-yaml v2 does: !!float takes an int */
} oyl_tagged;

struct oyl_schema {
    const oyl_schema_rule *rules;
    int                    rule_count;
    oyl_str                default_plain_tag;   /* unmatched plain scalars */
    oyl_str                default_quoted_tag;  /* all quoted scalars */
    oyl_str                default_seq_tag;     /* untagged sequences */
    oyl_str                default_map_tag;     /* untagged mappings */
    /* the first bytes any rule could accept: a plain scalar starting with
     * another byte is the default at once (NULL: no shortcut) */
    const bool            *first;
    /* by first byte c, the rules that may accept the text, in order:
     * cand[cand_at[c]] up to cand[cand_at[c + 1]], with c 256 for empty
     * text. Every rule is tried when cand_at is NULL or cand_at[257] 0. */
    const uint16_t        *cand_at;
    const uint8_t         *cand;
    /* how an explicit tag reads a scalar: by the rules bound to the tag, or
     * as a preset's library does (oyl_schema.c) */
    oyl_tagged             tagged;
};

/* ── Dialects and warnings ───────────────────────────────── */

struct oyl_dialect {
    const oyl_schema *schema;
    unsigned          syntax;   /* OYL_SYNTAX_ */
    oyl_merge_order   merge;
    int               minor;    /* the YAML version it is a dialect of: 1.minor */
};

/* The minor version of a version string "1.<minor>" (leading zeros allowed,
 * as in %YAML), or -1 */
int oyl_version_minor(const char *version);

/* Where warnings go. The parser re-reads input when it falls back to its
 * eager mode, so a warning is reported only past the last one of its code. */
typedef struct {
    oyl_warn_fn fn;
    void       *user;
    size_t      last[8];   /* per code: the offset of the last one reported */
    bool        seen[8];
} oyl_warn_state;

/* The rule that resolves untagged plain `text`, or -1 for the default (for
 * oyl_tree.c, which keeps it per node) */
int oyl_schema_rule_of(const oyl_schema *schema, oyl_str text);
/* Rule `rule`'s value of `text`: 1 accepted, 0 declined, -1 out of memory,
 * -2 accepted but without a value (oyl_schema_value_of's OYL_ERR_PARSE) */
int oyl_schema_rule_value(const oyl_schema *schema, int rule, oyl_str text, oyl_value *out);

/* ── Hashing ─────────────────────────────────────────────── */

/* The hash tables a document fills (anchors, keys, mappings by node) hash
 * with SipHash-1-3, the variant Python's and Rust's tables use, under a
 * secret key each parser takes from the system's randomness. Without the
 * key no one can tell where an entry goes, so no document can be made
 * whose entries all collide (hash flooding). A table only finds things,
 * never orders them, so the key changes nothing a caller sees. */
typedef struct {
    uint64_t k0, k1;
} oyl_hash_key;

/* A new secret key (oyl_hash.c) */
void oyl_hash_key_new(oyl_hash_key *key);

/* A hash being computed, fed in pieces */
typedef struct {
    uint64_t v0, v1, v2, v3;
    uint64_t buf, len;   /* the bytes not yet in a block, and how many in all */
} oyl_hasher;

#define OYL_ROTL64(x, b) (((x) << (b)) | ((x) >> (64 - (b))))

static inline void oyl_sip_round(oyl_hasher *h) {
    h->v0 += h->v1; h->v1 = OYL_ROTL64(h->v1, 13); h->v1 ^= h->v0; h->v0 = OYL_ROTL64(h->v0, 32);
    h->v2 += h->v3; h->v3 = OYL_ROTL64(h->v3, 16); h->v3 ^= h->v2;
    h->v0 += h->v3; h->v3 = OYL_ROTL64(h->v3, 21); h->v3 ^= h->v0;
    h->v2 += h->v1; h->v1 = OYL_ROTL64(h->v1, 17); h->v1 ^= h->v2; h->v2 = OYL_ROTL64(h->v2, 32);
}

static inline void oyl_sip_block(oyl_hasher *h, uint64_t m) {
    h->v3 ^= m;
    oyl_sip_round(h);
    h->v0 ^= m;
}

static inline void oyl_hash_start(oyl_hasher *h, const oyl_hash_key *key) {
    h->v0 = 0x736f6d6570736575u ^ key->k0;
    h->v1 = 0x646f72616e646f6du ^ key->k1;
    h->v2 = 0x6c7967656e657261u ^ key->k0;
    h->v3 = 0x7465646279746573u ^ key->k1;
    h->buf = h->len = 0;
}

/* SipHash reads its blocks little-endian, on every machine; compilers make
 * this one load where the machine is little-endian too */
static inline uint64_t oyl_load64le(const unsigned char *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 |
           (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static inline void oyl_hash_bytes(oyl_hasher *h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (; n && (h->len & 7); n--) {
        h->buf |= (uint64_t)*p++ << (8 * (h->len++ & 7));
        if (!(h->len & 7)) {
            oyl_sip_block(h, h->buf);
            h->buf = 0;
        }
    }
    for (; n >= 8; n -= 8, p += 8, h->len += 8) oyl_sip_block(h, oyl_load64le(p));
    for (; n; n--) h->buf |= (uint64_t)*p++ << (8 * (h->len++ & 7));
}

static inline uint64_t oyl_hash_end(oyl_hasher *h) {
    uint64_t b = h->len << 56 | h->buf;
    oyl_sip_block(h, b);
    h->v2 ^= 0xff;
    oyl_sip_round(h);
    oyl_sip_round(h);
    oyl_sip_round(h);
    return h->v0 ^ h->v1 ^ h->v2 ^ h->v3;
}

static inline uint64_t oyl_hash(const oyl_hash_key *key, const void *data, size_t n) {
    oyl_hasher h;
    oyl_hash_start(&h, key);
    oyl_hash_bytes(&h, data, n);
    return oyl_hash_end(&h);
}

/* A number's hash: its 8 bytes', little-endian */
static inline uint64_t oyl_hash_u64(const oyl_hash_key *key, uint64_t x) {
    oyl_hasher h;
    oyl_hash_start(&h, key);
    oyl_sip_block(&h, x);
    h.len = 8;
    return oyl_hash_end(&h);
}

/* A scalar as mapping keys are compared: two are equal when their tags
 * and values are (YAML 1.2.2 §3.2.1.3), so 0x1 and 1 are one key, 1 and
 * "1" two. Inline: trees sort keys with these. */
typedef struct {
    oyl_str   tag;
    oyl_value value;
} oyl_key;

/* The tag of a scalar tagged !, the non-specific tag: a string, as YAML
 * 1.2 has it, or with PyYAML's schema the tag `text` resolves to as if
 * plain (oyl_schema.c) */
oyl_str oyl_schema_nonspecific(const oyl_schema *schema, oyl_str text);

/* A key's tag from the scalar's (written, else resolved), and its text */
static inline oyl_str oyl_key_tag(const oyl_schema *schema, oyl_str tag, oyl_str text) {
    return tag.len == 1 && tag.data[0] == '!' ? oyl_schema_nonspecific(schema, text) : tag;
}

/* The key for a scalar with tag `tag` (from oyl_key_tag) and `text`, given
 * the status and value oyl_schema_value_of gave it: one its tag rejects is
 * its text */
static inline void oyl_key_make(oyl_key *k, oyl_str tag, oyl_str text, oyl_status st,
                                const oyl_value *v) {
    k->tag = tag;
    if (st == OYL_OK) k->value = *v;
    else k->value = (oyl_value){ .kind = OYL_VALUE_STR, .as = { .s = text } };
    /* one int, whichever kind a parser gave it */
    if (k->value.kind == OYL_VALUE_UINT && k->value.as.u <= INT64_MAX)
        k->value = (oyl_value){ .kind = OYL_VALUE_INT, .as = { .i = (int64_t)k->value.as.u } };
}

static inline int oyl_bytes_cmp(oyl_str a, oyl_str b) {
    size_t n = a.len < b.len ? a.len : b.len;
    int r = n ? memcmp(a.data, b.data, n) : 0;
    return r ? r : (a.len > b.len) - (a.len < b.len);
}

/* Are two keys' tags the same? */
static inline bool oyl_key_tag_eq(oyl_str a, oyl_str b) {
    return (a.data == b.data && a.len == b.len) ||
           (a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0));
}

/* A key's hash, the same for keys oyl_key_cmp finds equal. What is hashed
 * spells the key one way only (its tag's length first), so that keys
 * hash alike only by chance, which the secret key decides. In two steps,
 * so that keys sharing a tag can share the first: the tag, then the
 * value. */
static inline void oyl_key_hash_tag(oyl_hasher *h, const oyl_hash_key *hk, oyl_str tag) {
    oyl_hash_start(h, hk);
    uint64_t len = tag.len;
    oyl_hash_bytes(h, &len, sizeof len);
    oyl_hash_bytes(h, tag.data, tag.len);
}

static inline uint64_t oyl_key_hash_value(oyl_hasher *h, const oyl_value *v) {
    unsigned char kind = (unsigned char)v->kind;
    oyl_hash_bytes(h, &kind, 1);
    uint64_t x;
    switch (v->kind) {
    case OYL_VALUE_NULL:  x = 0; break;
    case OYL_VALUE_BOOL:  x = v->as.b; break;
    case OYL_VALUE_INT:   x = (uint64_t)v->as.i; break;
    case OYL_VALUE_UINT:  x = v->as.u; break;
    case OYL_VALUE_FLOAT: {
        double f = v->as.f;
        if (isnan(f)) {
            x = 0x7FF8000000000000u;   /* one NaN */
        } else {
            if (f == 0) f = 0;           /* -0 is 0 */
            memcpy(&x, &f, sizeof x);
        }
        break;
    }
    default:   /* text: last, so its length goes without saying */
        oyl_hash_bytes(h, v->as.s.data, v->as.s.len);
        return oyl_hash_end(h);
    }
    oyl_hash_bytes(h, &x, sizeof x);
    return oyl_hash_end(h);
}

static inline uint64_t oyl_key_hash(const oyl_hash_key *hk, const oyl_key *k) {
    oyl_hasher h;
    oyl_key_hash_tag(&h, hk, k->tag);
    return oyl_key_hash_value(&h, &k->value);
}

/* Order keys, 0 when equal: by tag, then value, NaN equal to NaN */
static inline int oyl_key_cmp(const oyl_key *a, const oyl_key *b) {
    if (!oyl_key_tag_eq(a->tag, b->tag)) return oyl_bytes_cmp(a->tag, b->tag);
    const oyl_value *x = &a->value, *y = &b->value;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    switch (x->kind) {
    case OYL_VALUE_NULL:  return 0;
    case OYL_VALUE_BOOL:  return (x->as.b > y->as.b) - (x->as.b < y->as.b);
    case OYL_VALUE_INT:   return (x->as.i > y->as.i) - (x->as.i < y->as.i);
    case OYL_VALUE_UINT:  return (x->as.u > y->as.u) - (x->as.u < y->as.u);
    case OYL_VALUE_FLOAT: {
        /* .nan's canonical form is one, so NaN equals NaN, after the numbers */
        bool nx = isnan(x->as.f), ny = isnan(y->as.f);
        if (nx || ny) return nx - ny;
        return (x->as.f > y->as.f) - (x->as.f < y->as.f);
    }
    default:              return oyl_bytes_cmp(x->as.s, y->as.s);   /* text */
    }
}

/* Report a warning from parsing, once (see oyl_warn_state) */
void oyl_warn(oyl_warn_state *w, oyl_warning code, oyl_mark mark, const char *msg);

/* ── Scanner ─────────────────────────────────────────────── */

/* Scan the next token into caller-provided storage. oyl_scan_next() wraps
 * this for the public API, making its errors final; the parser calls it
 * directly and keeps the first error itself. */
oyl_status oyl_scan_token(oyl_scanner *s, oyl_token *tok);
/* The syntax leniencies to apply (OYL_SYNTAX_), and where to warn of their
 * use; the parser sets them per document. */
void oyl_scanner_set_syntax(oyl_scanner *s, unsigned syntax, oyl_warn_state *w);
/* Copy one scanner's state into another over the same input (the parser's
 * checkpoint at a document start). */
bool oyl_scanner_copy(oyl_scanner *dst, const oyl_scanner *src);

/* ── Tags, for the parser and the emitter ────────────────── */

/* A URI character (YAML's ns-uri-char, but for %-escapes) */
static inline bool oyl_is_uri_char(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("-#;/?:@&=+$,_.!~*'()[]", c));
}

/* A %TAG handle: "!", "!!", or "!name!" of letters, digits and '-' */
static inline bool oyl_tag_handle_ok(const char *s, size_t n) {
    if (n == 0 || s[0] != '!' || s[n - 1] != '!') return false;
    for (size_t i = 1; i + 1 < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '-'))
            return false;
    }
    return true;
}

/* A %TAG prefix as written: URI characters and %-escapes, not starting
 * with a flow indicator */
static inline bool oyl_tag_prefix_ok(const char *s, size_t n) {
    if (n == 0 || s[0] == ',' || s[0] == '[' || s[0] == ']') return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '%') {
            if (!oyl_is_uri_char((uint8_t)s[i])) return false;
            continue;
        }
        if (n - i < 3) return false;
        for (size_t j = i + 1; j <= i + 2; j++) {
            char c = s[j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        i += 2;
    }
    return true;
}

/* ── Emitter, for oyl_tree.c ─────────────────────────────── */

/* Whether the next document's directives have the %TAG @p handle, or with
 * a NULL handle a %YAML */
bool oyl_emitter_has_directive(const oyl_emitter *e, oyl_str handle);

/* Fail the emitter with @p st, as its own errors do: the first error is
 * kept, and every later call returns it */
oyl_status oyl_emitter_fail(oyl_emitter *e, oyl_status st);

/* ── Parser, for oyl_json.c and oyl_tree.c ──────────────────────────────── */

/* Report an error found after parsing (a converter's), as parse errors
 * are: oyl_parser_error() and oyl_parser_error_mark() return it, and
 * every later oyl_parse_next() returns `status` */
void oyl_parser_fail(oyl_parser *p, oyl_status status, oyl_mark mark, const char *msg);
oyl_arena        *oyl_parser_arena(const oyl_parser *p);
const char       *oyl_parser_input(const oyl_parser *p, size_t *len);
/* Whether `d` is a dialect the parser made itself (from a schema alone),
 * which lives only as long as the parser */
bool              oyl_parser_owns_dialect(const oyl_parser *p, const oyl_dialect *d);
int               oyl_parser_depth_limit(const oyl_parser *p);   /* 0: none */
const oyl_schema *oyl_parser_schema(const oyl_parser *p);       /* NULL: none */
/* Report a warning found after parsing (no re-reading to guard against) */
void              oyl_parser_warn(oyl_parser *p, oyl_warning code, oyl_mark mark,
                                  const char *msg);
/* The secret key of the hash tables the parser's documents fill, made the
 * first time one needs it */
const oyl_hash_key *oyl_parser_hash_key(oyl_parser *p);

/* ── Trees, for oyl_json.c ───────────────────────────────── */

/* Numbers by node (as the mappings merges visit), on the stack until they
 * outgrow it. A document picks which nodes go in by where it puts its
 * mappings, so the nodes hash under a secret key. */
typedef struct {
    uint32_t *node, *val;   /* node UINT32_MAX: an empty slot */
    uint32_t  cap, len;
    const oyl_hash_key *key;
    uint32_t  node_buf[32], val_buf[32];
} oyl_node_map;

void     oyl_nmap_init(oyl_node_map *s, const oyl_hash_key *key);
void     oyl_nmap_free(oyl_node_map *s);
/* Node x's number, or UINT32_MAX */
uint32_t oyl_nmap_get(const oyl_node_map *s, uint32_t x);
/* Number node x (not there yet) v; false if out of memory */
bool     oyl_nmap_put(oyl_node_map *s, uint32_t x, uint32_t v);

struct oyl_tree {
    const char        *input;       /* the whole input */
    size_t             input_len;
    size_t             base;        /* where the document starts in it */
    oyl_node          *nodes;
    uint32_t           len, cap;
    oyl_node_props    *props;       /* props[0] is unused: 0 means none */
    uint32_t           plen, pcap;
    char              *pool;
    size_t             pool_len, pool_cap;
    oyl_mark          *marks;       /* marks[k]: node k * MARK_EVERY's start */
    uint32_t           mlen, mcap;
    oyl_mark           doc_mark;    /* the document's start */
    const oyl_dialect *dialect;
    const oyl_schema  *schema;
    oyl_merge_order    merge;
    bool               implicit_start, implicit_end;   /* no "---", no "..." */
    bool               has_version;   /* the document's %YAML */
    int                version_major, version_minor;
    int                tag_dir_count; /* its %TAG directives, in the pool */
    struct { uint32_t handle, prefix; uint16_t handle_len, prefix_len; } tag_dirs[8];
    oyl_dialect        own_dialect;   /* a copy of a dialect the parser owned */
    struct oyl_merges *merges;      /* the pairs of mappings that merge, worked out
                                       as the tree is built (oyl_tree.c); NULL: none */
    bool               merges_costly; /* too costly to work out: lookups walk them */
    oyl_hash_key       hash_key;      /* its parser's, when it has merges */
};

/* A tree as written, its merges not worked out (oyl_json.c works out its
 * own) */
oyl_status oyl_parse_tree_as_written(oyl_parser *p, oyl_tree **tree);
/* For tests: forget tree t's merges worked out, as if too costly */
void       oyl_tree_forget_merges(oyl_tree *t);

/* A scalar's or alias's text */
static inline oyl_str oyl_tree_text(const oyl_tree *t, const oyl_node *n) {
    if (!n->len) return (oyl_str){ "", 0 };
    return n->flags & OYL_NODE_POOLED ? (oyl_str){ t->pool + n->text, n->len }
                                      : (oyl_str){ t->input + t->base + n->text, n->len };
}

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
