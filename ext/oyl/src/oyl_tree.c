/*
 * oyl_tree.c — documents in memory (oyl_parse_tree)
 *
 * A tree is a document's nodes in document order, each collection with
 * the size of its subtree instead of links to its children (oyl.h,
 * "Trees"). It is built by appending the parser's events. Positions are
 * 32-bit offsets from the document's start, and text the parser rewrote
 * is copied into the tree's pool, so a tree holds no pointers: a binding
 * can copy it whole, and parts built separately can be joined.
 */

#include "oyl_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE OYL_NO_NODE

/* A mark is kept for every MARK_EVERY-th node; oyl_node_mark counts lines
 * from the nearest one */
#define MARK_EVERY 64

/* struct oyl_tree is in oyl_internal.h, for oyl_json.c */

static bool same(oyl_str a, oyl_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.data, b.data, a.len) == 0);
}

/* Make room for `need` more elements in a growable array */
NOINLINE bool grow_slow(void **data, uint32_t *cap, uint32_t len, uint32_t need, size_t size) {
    uint64_t want = (uint64_t)len + need, c = *cap ? *cap : 64;
    while (c < want) c *= 2;
    if (c > UINT32_MAX) c = UINT32_MAX;
    if (c < want || c > SIZE_MAX / size) return false;
    void *p = realloc(*data, (size_t)c * size);
    if (!p) return false;
    *data = p;
    *cap = (uint32_t)c;
    return true;
}

static inline bool grow(void **data, uint32_t *cap, uint32_t len, uint32_t need, size_t size) {
    return need <= *cap - len || grow_slow(data, cap, len, need, size);
}

/* ── Building ────────────────────────────────────────────── */

typedef struct {
    oyl_str  name;
    uint32_t node;
} anchor;

typedef struct {
    oyl_parser       *p;
    oyl_tree         *t;
    struct open { uint32_t node, last, count; } *stack;   /* open collections */
    uint32_t          slen, scap;
    anchor           *anchors;      /* open addressing; name.data NULL is empty */
    size_t            acap, alen;
    const oyl_hash_key *key;        /* the parser's, once there are anchors */
    uintptr_t         lo, hi;       /* the document's text: from its start to the input's end */
    size_t            base;
    bool              small;        /* under 4 GB from the document's start: no offset overflows */
    oyl_str           last_tag;     /* the last tag pooled, and where */
    uint32_t          last_tag_at;
    bool              merge_key;    /* a key written << */
    oyl_status        status;
} builder;

static bool fail(builder *b, oyl_status status, oyl_mark mark, const char *fmt, ...) {
    if (b->status == OYL_OK) {
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        oyl_parser_fail(b->p, status, mark, msg);
        b->status = status;
    }
    return false;
}

static bool out_of_memory(builder *b) {
    return fail(b, OYL_ERR_MEMORY, (oyl_mark){ 0, 0, 0 }, "out of memory");
}

static anchor *anchor_slot(builder *b, oyl_str name) {
    size_t i = oyl_hash(b->key, name.data, name.len) & (b->acap - 1);
    while (b->anchors[i].name.data && !same(b->anchors[i].name, name)) i = (i + 1) & (b->acap - 1);
    return &b->anchors[i];
}

/* Bind `name` to node `n`; a later anchor of the same name replaces it, so
 * an alias refers to the closest one before it */
static bool anchor_set(builder *b, oyl_str name, uint32_t n) {
    if (2 * (b->alen + 1) > b->acap) {
        if (!b->key) b->key = oyl_parser_hash_key(b->p);
        size_t cap = b->acap ? b->acap * 2 : 64;
        anchor *old = b->anchors;
        size_t old_cap = b->acap;
        b->anchors = calloc(cap, sizeof *b->anchors);
        if (!b->anchors) { b->anchors = old; return out_of_memory(b); }
        b->acap = cap;
        for (size_t i = 0; i < old_cap; i++)
            if (old[i].name.data) *anchor_slot(b, old[i].name) = old[i];
        free(old);
    }
    anchor *a = anchor_slot(b, name);
    if (!a->name.data) b->alen++;
    *a = (anchor){ name, n };
    return true;
}

static uint32_t anchor_get(builder *b, oyl_str name) {
    if (!b->acap) return NONE;
    anchor *a = anchor_slot(b, name);
    return a->name.data ? a->node : NONE;
}

/* An input offset as a position in the document */
static inline bool position(builder *b, const oyl_event *e, size_t offset, uint32_t *out) {
    if (b->small) {   /* nodes don't start before their document */
        *out = (uint32_t)(offset - b->base);
        return true;
    }
    size_t rel = offset >= b->base ? offset - b->base : 0;
    if (rel > UINT32_MAX)
        return fail(b, OYL_ERR_LIMIT, e->start, "document too large for a tree (over 4 GB)");
    *out = (uint32_t)rel;
    return true;
}

static bool pool_add(builder *b, oyl_str s, uint32_t *at) {
    oyl_tree *t = b->t;
    if (s.len > UINT32_MAX - t->pool_len)
        return fail(b, OYL_ERR_LIMIT, (oyl_mark){ 0, 0, 0 }, "document too large for a tree (over 4 GB)");
    if (s.len > t->pool_cap - t->pool_len) {
        size_t cap = t->pool_cap ? t->pool_cap : 256;
        while (cap - t->pool_len < s.len) cap *= 2;
        char *pool = realloc(t->pool, cap);
        if (!pool) return out_of_memory(b);
        t->pool = pool;
        t->pool_cap = cap;
    }
    if (s.len) memcpy(t->pool + t->pool_len, s.data, s.len);
    *at = (uint32_t)t->pool_len;
    t->pool_len += s.len;
    return true;
}

/* Where text is: in the document, or (rewritten by the parser) copied into
 * the pool */
static inline bool place_text(builder *b, oyl_str s, oyl_node *n) {
    n->len = (uint32_t)s.len;
    if (s.len == 0) { n->text = 0; return true; }
    uintptr_t p = (uintptr_t)s.data;
    if (p >= b->lo && p <= b->hi && s.len <= b->hi - p && (b->small || p - b->lo <= UINT32_MAX)) {
        n->text = (uint32_t)(p - b->lo);
        return true;
    }
    if (s.len > UINT32_MAX)
        return fail(b, OYL_ERR_LIMIT, (oyl_mark){ 0, 0, 0 }, "scalar too large for a tree (over 4 GB)");
    n->flags |= OYL_NODE_POOLED;
    return pool_add(b, s, &n->text);
}

/* A tag into the pool; nodes often repeat the last one */
static bool place_tag(builder *b, oyl_str tag, uint32_t *at) {
    if (b->last_tag.len && same(tag, b->last_tag)) {
        *at = b->last_tag_at;
        return true;
    }
    if (!pool_add(b, tag, at)) return false;
    b->last_tag = tag;
    b->last_tag_at = *at;
    return true;
}

static bool add_props(builder *b, oyl_str tag, oyl_str anchor_name, oyl_node *n) {
    oyl_tree *t = b->t;
    if (!grow((void **)&t->props, &t->pcap, t->plen, 1, sizeof *t->props)) return out_of_memory(b);
    oyl_node_props pr = { 0, 0, 0, 0 };
    if (tag.len) {
        if (!place_tag(b, tag, &pr.tag)) return false;
        pr.tag_len = (uint32_t)tag.len;
    }
    if (anchor_name.len) {
        if (!pool_add(b, anchor_name, &pr.anchor)) return false;
        pr.anchor_len = (uint32_t)anchor_name.len;
    }
    n->props = t->plen;
    t->props[t->plen++] = pr;
    return true;
}

/* The tag a node keeps: the one written. A parser with a schema tags
 * every node, and marks the tags it gave implicit. */
static oyl_str written_tag(const oyl_event *e) { return e->implicit ? OYL_STR_NULL : e->tag; }

static bool close_collection(builder *b, const oyl_event *e) {
    oyl_tree *t = b->t;
    if (!b->slen) return true;
    struct open o = b->stack[--b->slen];
    oyl_node *n = &t->nodes[o.node];
    n->span = t->len - o.node;
    n->len = n->kind == OYL_NODE_MAPPING ? o.count / 2 : o.count;
    /* a block collection ends where its last child does (its end event
     * comes at the next token, past comments and blank lines) */
    if ((n->flags & OYL_NODE_FLOW) || o.last == NONE) return position(b, e, e->end.offset, &n->end);
    n->end = t->nodes[o.last].end;
    return true;
}

ALWAYS_INLINE bool add_event(builder *b, const oyl_event *e) {
    oyl_tree *t = b->t;
    oyl_node n = { 0 };
    switch (e->type) {
    case OYL_EVT_SCALAR:         n.kind = OYL_NODE_SCALAR; break;
    case OYL_EVT_ALIAS:          n.kind = OYL_NODE_ALIAS; break;
    case OYL_EVT_SEQUENCE_START: n.kind = OYL_NODE_SEQUENCE; break;
    case OYL_EVT_MAPPING_START:  n.kind = OYL_NODE_MAPPING; break;
    case OYL_EVT_SEQUENCE_END:
    case OYL_EVT_MAPPING_END:    return close_collection(b, e);
    default:                     return true;
    }
    if (t->len >= UINT32_MAX - 1)
        return fail(b, OYL_ERR_LIMIT, e->start, "document too large for a tree (over 4 billion nodes)");
    uint32_t i = t->len;
    if (b->slen == 0 && i != 0) return true;   /* a document has one root */
    if (!grow((void **)&t->nodes, &t->cap, t->len, 1, sizeof *t->nodes)) return out_of_memory(b);
    if (i % MARK_EVERY == 0) {
        if (!grow((void **)&t->marks, &t->mcap, t->mlen, 1, sizeof *t->marks)) return out_of_memory(b);
        t->marks[t->mlen++] = e->start;
    }
    if (!position(b, e, e->start.offset, &n.start)) return false;
    oyl_str tag = OYL_STR_NULL;
    if (n.kind == OYL_NODE_ALIAS) {
        n.props = anchor_get(b, e->value);
        if (n.props == NONE)
            return fail(b, OYL_ERR_PARSE, e->start, "unknown anchor '%.*s' referenced",
                        (int)(e->value.len > 100 ? 100 : e->value.len), e->value.data);
    } else {
        tag = written_tag(e);
    }
    if (n.kind == OYL_NODE_SCALAR || n.kind == OYL_NODE_ALIAS) {
        n.span = 1;
        n.style = (uint8_t)e->scalar_style;
        if (!place_text(b, e->value, &n) || !position(b, e, e->end.offset, &n.end)) return false;
        if (n.kind == OYL_NODE_SCALAR && !tag.len && e->scalar_style == OYL_SCALAR_PLAIN) {
            int r = oyl_schema_rule_of(t->schema, e->value);
            n.type = (uint8_t)(r < 0 ? 0 : r < 254 ? r + 1 : 255);
        }
    } else if (e->flow) {
        n.flags |= OYL_NODE_FLOW;
    }
    if ((tag.len || e->anchor.len) && !add_props(b, tag, e->anchor, &n)) return false;
    if (b->slen) {
        struct open *o = &b->stack[b->slen - 1];
        n.parent = i - o->node;
        if (t->nodes[o->node].kind == OYL_NODE_MAPPING && o->count % 2 == 0) {
            n.flags |= OYL_NODE_KEY;
            if (e->value.len == 2 && n.kind == OYL_NODE_SCALAR && !memcmp(e->value.data, "<<", 2))
                b->merge_key = true;
        }
        o->count++;
        o->last = i;
    }
    t->nodes[t->len++] = n;
    if (e->anchor.len && !anchor_set(b, e->anchor, i)) return false;
    if (n.kind == OYL_NODE_SEQUENCE || n.kind == OYL_NODE_MAPPING) {
        if (!grow((void **)&b->stack, &b->scap, b->slen, 1, sizeof *b->stack)) return out_of_memory(b);
        b->stack[b->slen++] = (struct open){ i, NONE, 0 };
    }
    return true;
}

static bool work_out_merges(oyl_tree *t);

/* The document's directives, asked of the parser at its DOC_START */
static bool add_directives(builder *b) {
    oyl_tree *t = b->t;
    oyl_str h, pf;
    t->has_version = oyl_parser_yaml_version(b->p, &t->version_major, &t->version_minor);
    for (int i = 0; i < 8 && oyl_parser_tag_directive(b->p, i, &h, &pf); i++) {
        if (!pool_add(b, h, &t->tag_dirs[i].handle) || !pool_add(b, pf, &t->tag_dirs[i].prefix))
            return false;
        t->tag_dirs[i].handle_len = (uint16_t)h.len;
        t->tag_dirs[i].prefix_len = (uint16_t)pf.len;
        t->tag_dir_count = i + 1;
    }
    return true;
}

static oyl_status parse_tree(oyl_parser *p, oyl_tree **tree, bool merges) {
    if (!p || !tree) return OYL_ERR_INPUT;
    *tree = NULL;
    const oyl_event *e;
    oyl_status st;
    for (;;) {
        st = oyl_parse_next(p, &e);
        if (st != OYL_OK) return st;
        if (e->type == OYL_EVT_STREAM_START) continue;
        if (e->type == OYL_EVT_STREAM_END || e->type == OYL_EVT_NONE) return OYL_OK;
        if (e->type == OYL_EVT_DOC_START) break;
        oyl_parser_fail(p, OYL_ERR_INPUT, e->start, "oyl_parse_tree: not at the start of a document");
        return OYL_ERR_INPUT;
    }
    /* from here the document is being read: failing leaves it half read */
    oyl_tree *t = calloc(1, sizeof *t);
    if (!t) {
        oyl_parser_fail(p, OYL_ERR_MEMORY, e->start, "out of memory");
        return OYL_ERR_MEMORY;
    }
    t->input = oyl_parser_input(p, &t->input_len);
    t->base = e->start.offset;
    t->doc_mark = e->start;
    t->dialect = oyl_parser_document_dialect(p);
    if (oyl_parser_owns_dialect(p, t->dialect)) {   /* the tree may outlive the parser */
        t->own_dialect = *t->dialect;
        t->dialect = &t->own_dialect;
    }
    t->schema = oyl_dialect_schema(t->dialect);
    t->merge = oyl_dialect_merge(t->dialect);
    t->implicit_start = e->implicit;
    t->plen = 1;
    builder b = { .p = p, .t = t, .base = t->base,
                  .lo = (uintptr_t)t->input + t->base, .hi = (uintptr_t)t->input + t->input_len,
                  .small = t->input_len - t->base <= UINT32_MAX };
    if (!grow((void **)&t->props, &t->pcap, 0, 1, sizeof *t->props)) {
        oyl_tree_free(t);
        oyl_parser_fail(p, OYL_ERR_MEMORY, e->start, "out of memory");
        return OYL_ERR_MEMORY;
    }
    t->props[0] = (oyl_node_props){ 0, 0, 0, 0 };
    bool ok = add_directives(&b);
    while (ok && (st = oyl_parse_next(p, &e)) == OYL_OK && e->type != OYL_EVT_DOC_END)
        if (!add_event(&b, e)) break;
    if (st == OYL_OK) t->implicit_end = e->implicit;
    if (st == OYL_OK && b.status == OYL_OK && t->len == 0) {
        /* no node: the document is an empty scalar */
        oyl_event empty = { .type = OYL_EVT_SCALAR, .start = t->doc_mark, .end = t->doc_mark };
        add_event(&b, &empty);
    }
    free(b.stack);
    free(b.anchors);
    if (b.merge_key) t->hash_key = *oyl_parser_hash_key(p);
    if (st == OYL_OK) st = b.status;
    if (st == OYL_OK && merges && b.merge_key && t->merge != OYL_MERGE_NONE && !work_out_merges(t)) {
        oyl_parser_fail(p, OYL_ERR_MEMORY, t->doc_mark, "out of memory");
        st = OYL_ERR_MEMORY;
    }
    if (st != OYL_OK) {
        oyl_tree_free(t);
        return st;
    }
    *tree = t;
    return OYL_OK;
}

oyl_status oyl_parse_tree(oyl_parser *p, oyl_tree **tree) { return parse_tree(p, tree, true); }

oyl_status oyl_parse_tree_as_written(oyl_parser *p, oyl_tree **tree) { return parse_tree(p, tree, false); }

static void free_merges(struct oyl_merges *mg);

void oyl_tree_forget_merges(oyl_tree *t) {
    free_merges(t->merges);
    t->merges = NULL;
    t->merges_costly = true;
}

void oyl_tree_free(oyl_tree *t) {
    if (!t) return;
    free(t->nodes);
    free(t->props);
    free(t->pool);
    free(t->marks);
    free_merges(t->merges);
    free(t);
}

/* ── Reading ─────────────────────────────────────────────── */

const oyl_node *oyl_tree_nodes(const oyl_tree *t, uint32_t *count) {
    if (count) *count = t ? t->len : 0;
    return t ? t->nodes : NULL;
}

const oyl_node_props *oyl_tree_props(const oyl_tree *t, uint32_t *count) {
    if (count) *count = t ? t->plen : 0;
    return t ? t->props : NULL;
}

oyl_str oyl_tree_pool(const oyl_tree *t) {
    return t && t->pool_len ? (oyl_str){ t->pool, t->pool_len } : (oyl_str){ "", 0 };
}

size_t oyl_tree_base(const oyl_tree *t) { return t ? t->base : 0; }

const oyl_dialect *oyl_tree_dialect(const oyl_tree *t) { return t ? t->dialect : NULL; }

static oyl_str pooled(const oyl_tree *t, uint32_t at, uint32_t len) {
    return len ? (oyl_str){ t->pool + at, len } : OYL_STR_NULL;
}

bool oyl_tree_yaml_version(const oyl_tree *t, int *major, int *minor) {
    if (!t || !t->has_version) return false;
    if (major) *major = t->version_major;
    if (minor) *minor = t->version_minor;
    return true;
}

bool oyl_tree_tag_directive(const oyl_tree *t, int i, oyl_str *handle, oyl_str *prefix) {
    if (!t || i < 0 || i >= t->tag_dir_count) return false;
    if (handle) *handle = pooled(t, t->tag_dirs[i].handle, t->tag_dirs[i].handle_len);
    if (prefix) *prefix = pooled(t, t->tag_dirs[i].prefix, t->tag_dirs[i].prefix_len);
    return true;
}


oyl_str oyl_node_text(const oyl_tree *t, uint32_t i) {
    if (!t || i >= t->len) return OYL_STR_NULL;
    const oyl_node *n = &t->nodes[i];
    if (n->kind != OYL_NODE_SCALAR && n->kind != OYL_NODE_ALIAS) return OYL_STR_NULL;
    return oyl_tree_text(t, n);
}

static oyl_str written(const oyl_tree *t, const oyl_node *n) {
    if (n->kind == OYL_NODE_ALIAS || !n->props) return OYL_STR_NULL;
    return pooled(t, t->props[n->props].tag, t->props[n->props].tag_len);
}

oyl_str oyl_node_tag(const oyl_tree *t, uint32_t i) {
    if (!t || i >= t->len) return OYL_STR_NULL;
    const oyl_node *n = &t->nodes[i];
    if (n->kind == OYL_NODE_ALIAS) return OYL_STR_NULL;
    oyl_str tag = written(t, n);
    if (tag.len) return tag;
    const oyl_schema *s = t->schema;
    switch (n->kind) {
    case OYL_NODE_SEQUENCE: return s->default_seq_tag;
    case OYL_NODE_MAPPING:  return s->default_map_tag;
    default: break;
    }
    if (n->style != OYL_SCALAR_PLAIN) return s->default_quoted_tag;
    if (n->type == 0) return s->default_plain_tag;
    if (n->type == 255) return oyl_schema_resolve(s, oyl_tree_text(t, n), OYL_SCALAR_PLAIN);
    return s->rules[n->type - 1].tag;
}

oyl_str oyl_node_anchor(const oyl_tree *t, uint32_t i) {
    if (!t || i >= t->len) return OYL_STR_NULL;
    const oyl_node *n = &t->nodes[i];
    if (n->kind == OYL_NODE_ALIAS || !n->props) return OYL_STR_NULL;
    return pooled(t, t->props[n->props].anchor, t->props[n->props].anchor_len);
}

uint32_t oyl_node_target(const oyl_tree *t, uint32_t i) {
    if (!t || i >= t->len) return NONE;
    /* an alias can't have an anchor, so its target is never an alias */
    return t->nodes[i].kind == OYL_NODE_ALIAS ? t->nodes[i].props : i;
}

oyl_status oyl_node_value(const oyl_tree *t, uint32_t i, oyl_value *v) {
    if (!t || !v || i >= t->len) return OYL_ERR_INPUT;
    i = oyl_node_target(t, i);
    const oyl_node *n = &t->nodes[i];
    if (n->kind != OYL_NODE_SCALAR) return OYL_ERR_INPUT;
    oyl_str text = oyl_tree_text(t, n), tag = written(t, n);
    if (tag.len || n->type == 255)
        return oyl_schema_value_of(t->schema, text, (oyl_scalar_style)n->style, tag, v);
    if (n->type == 0) {
        *v = (oyl_value){ .kind = OYL_VALUE_STR, .as = { .s = text } };
        return OYL_OK;
    }
    int r = oyl_schema_rule_value(t->schema, n->type - 1, text, v);
    if (r == -2) return OYL_ERR_PARSE;
    if (r < 0) return OYL_ERR_MEMORY;
    if (r == 0) return oyl_schema_value_of(t->schema, text, OYL_SCALAR_PLAIN, OYL_STR_NULL, v);
    return OYL_OK;
}

oyl_mark oyl_node_mark(const oyl_tree *t, uint32_t i) {
    if (!t || i >= t->len) return (oyl_mark){ 0, 0, 0 };
    size_t off = t->base + t->nodes[i].start;
    /* positions aren't always in node order (a block mapping starts after
     * its first key's anchor or tag), so a mark may be past the node */
    uint32_t at = i / MARK_EVERY;
    oyl_mark m = t->marks[at];
    while (m.offset > off && at > 0) m = t->marks[--at];
    if (m.offset > off) m = t->doc_mark;
    if (m.offset > off) return (oyl_mark){ off, 0, 0 };
    /* count line breaks as YAML does: \n, \r\n, or \r alone */
    for (size_t j = m.offset; j < off; j++) {
        char c = t->input[j];
        if (c == '\n' || (c == '\r' && (j + 1 >= t->input_len || t->input[j + 1] != '\n'))) {
            m.line++;
            m.col = 1;
        } else if (c != '\r') {
            m.col++;
        }
    }
    m.offset = off;
    return m;
}

/* ── Finding nodes ───────────────────────────────────────── */

uint32_t oyl_node_at(const oyl_tree *t, uint32_t seq, uint32_t k) {
    seq = oyl_node_target(t, seq);
    if (seq == NONE || t->nodes[seq].kind != OYL_NODE_SEQUENCE || k >= t->nodes[seq].len) return NONE;
    uint32_t c = seq + 1;
    while (k--) c += t->nodes[c].span;
    return c;
}

/* Is key node k a merge key? << untagged and plain, tagged !!merge, or
 * tagged ! in any style, which leaves it to be resolved as if plain
 * (libyaml's reading, which PyYAML and go-yaml v2 share) */
static bool is_merge_key(const oyl_tree *t, uint32_t k) {
    const oyl_node *n = &t->nodes[k];
    if (n->kind != OYL_NODE_SCALAR || n->len != 2 || memcmp(oyl_tree_text(t, n).data, "<<", 2) != 0)
        return false;
    oyl_str tag = written(t, n);
    if (tag.len == 1 && tag.data[0] == '!') return true;
    return tag.len ? same(tag, OYL_TAG_MERGE) : n->style == OYL_SCALAR_PLAIN;
}

/* What value v of a << merges: v, or under YAML's merge the sequence an
 * alias refers to (go-yaml v2 takes only a mapping through an alias) */
static uint32_t merge_value(const oyl_tree *t, uint32_t v) {
    if (t->merge == OYL_MERGE_YAML && t->nodes[v].kind == OYL_NODE_ALIAS) {
        uint32_t s = oyl_node_target(t, v);
        if (s != NONE && t->nodes[s].kind == OYL_NODE_SEQUENCE) return s;
    }
    return v;
}

/* Can value v of a << in mapping m be merged? A mapping, an alias to one,
 * or a sequence of those, none of them m itself or a mapping containing
 * it. Aliases refer back, so a merge's source always ends before the
 * mapping merging it does: merges can't go round in a cycle. */
static bool mergeable(const oyl_tree *t, uint32_t m, uint32_t v) {
    const oyl_node *n = t->nodes;
    v = merge_value(t, v);
    bool seq = n[v].kind == OYL_NODE_SEQUENCE;
    uint32_t count = seq ? n[v].len : 1, item = seq ? v + 1 : v;
    for (uint32_t c = 0; c < count; c++, item += n[item].span) {
        uint32_t s = oyl_node_target(t, item);
        if (n[s].kind != OYL_NODE_MAPPING) return false;
        if (s <= m && m < s + n[s].span) return false;
    }
    return true;
}

/* A merge's items: the value, or a sequence's items */
static uint32_t merge_items(const oyl_tree *t, uint32_t v, uint32_t *first) {
    v = merge_value(t, v);
    bool seq = t->nodes[v].kind == OYL_NODE_SEQUENCE;
    *first = seq ? v + 1 : v;
    return seq ? t->nodes[v].len : 1;
}

/* Key k as keys are compared (k a scalar, not an alias). False if its
 * value can't be read for want of memory, which mustn't make it another
 * key: its text. */
static bool key_of(const oyl_tree *t, uint32_t k, oyl_key *key) {
    const oyl_node *n = &t->nodes[k];
    if (!n->props && (n->style != OYL_SCALAR_PLAIN || n->type == 0)) {   /* a string, as most keys are */
        *key = (oyl_key){ n->style != OYL_SCALAR_PLAIN ? t->schema->default_quoted_tag
                                                      : t->schema->default_plain_tag,
                          { .kind = OYL_VALUE_STR, .as = { .s = oyl_tree_text(t, n) } } };
        return true;
    }
    oyl_value v;
    oyl_status st = oyl_node_value(t, k, &v);
    if (st == OYL_ERR_MEMORY) return false;
    oyl_str text = oyl_node_text(t, k);
    oyl_key_make(key, oyl_key_tag(t->schema, oyl_node_tag(t, k), text), text, st, &v);
    return true;
}

/* ── Merges ──────────────────────────────────────────────── */

/* A mapping's merges make a graph of mappings, each merging others in
 * turn. It has no cycles (aliases refer back, and a mapping can't merge
 * one containing it), but a mapping may be reached along many paths: in a
 * chain of mappings each merging the one before twice, 2^n lead to its
 * start. So as a tree is built, the pairs of each mapping that merges are
 * worked out (work_out_merges, below) from those of the mappings it merges
 * directly, which end before it does and so are worked out first: each
 * mapping's once, however many paths reach it, and a lookup reads them.
 * The work is bounded as the parser's merges for events are. Past the
 * bound nothing is kept: oyl_node_entries gives OYL_ERR_LIMIT for a
 * mapping that merges, and oyl_node_get walks its graph, visiting each
 * mapping once in the order keys win, at most the tree. */

/* A pair of a mapping that merges, as oyl_node_entries gives them, and
 * its rank in the order keys win (0 wins) */
typedef struct {
    uint32_t key, value, rank;
} merged_pair;

/* A mapping's pairs worked out, then the keys merged into it that a
 * pair's key overrides, spelled otherwise (0x2 to a 2 written), each
 * spelling's first in the order keys win: oyl_node_get finds by text */
typedef struct {
    uint32_t first, n;   /* pairs[first, first + n), in the order keys first appear */
    uint32_t spelled;    /* the keys spelled otherwise: the next pairs */
    uint32_t bad;        /* the first key written again in it, or NONE */
} merged_map;

/* The pairs worked out: of the mappings that merge, and of those merged */
struct oyl_merges {
    merged_pair *pairs;
    merged_map  *maps;
    uint32_t     npairs, pairs_cap, nmaps, maps_cap;
    oyl_node_map index;   /* each mapping's maps[], by node */
};

static void free_merges(struct oyl_merges *mg) {
    if (!mg) return;
    free(mg->pairs);
    free(mg->maps);
    oyl_nmap_free(&mg->index);
    free(mg);
}

static const merged_map *merged_of(const oyl_tree *t, uint32_t m) {
    if (!t->merges) return NULL;
    uint32_t i = oyl_nmap_get(&t->merges->index, m);
    return i == NONE ? NULL : &t->merges->maps[i];
}

/* Room for one more in an array that starts in a buffer on the stack
 * (most mappings are small), and moves to the heap when it's full */
static bool room(void **data, const void *buf, uint32_t *cap, uint32_t len, size_t size) {
    if (len < *cap) return true;
    if (*data != buf) return grow_slow(data, cap, len, 1, size);
    void *p = malloc((size_t)*cap * 2 * size);
    if (!p) return false;
    memcpy(p, buf, (size_t)len * size);
    *data = p;
    *cap *= 2;
    return true;
}

/* oyl_node_map (oyl_internal.h) */

void oyl_nmap_init(oyl_node_map *s, const oyl_hash_key *key) {
    s->node = s->node_buf;
    s->val = s->val_buf;
    s->cap = 32;
    s->len = 0;
    s->key = key;
    memset(s->node_buf, 0xFF, sizeof s->node_buf);
}

void oyl_nmap_free(oyl_node_map *s) {
    if (s->node != s->node_buf) {
        free(s->node);
        free(s->val);
    }
}

static uint32_t nmap_slot(const oyl_node_map *s, uint32_t x) {
    uint32_t i = (uint32_t)oyl_hash_u64(s->key, x) & (s->cap - 1);
    while (s->node[i] != NONE && s->node[i] != x) i = (i + 1) & (s->cap - 1);
    return i;
}

uint32_t oyl_nmap_get(const oyl_node_map *s, uint32_t x) {
    uint32_t i = nmap_slot(s, x);
    return s->node[i] == x ? s->val[i] : NONE;
}

bool oyl_nmap_put(oyl_node_map *s, uint32_t x, uint32_t v) {
    if ((s->len + 1) * 2 > s->cap) {
        if (s->cap > UINT32_MAX / 4) return false;
        oyl_node_map big = { .cap = s->cap * 2, .len = s->len, .key = s->key };
        big.node = malloc((size_t)big.cap * sizeof *big.node);
        big.val = malloc((size_t)big.cap * sizeof *big.val);
        if (!big.node || !big.val) {
            free(big.node);
            free(big.val);
            return false;
        }
        memset(big.node, 0xFF, (size_t)big.cap * sizeof *big.node);
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->node[i] != NONE) {
                uint32_t j = nmap_slot(&big, s->node[i]);
                big.node[j] = s->node[i];
                big.val[j] = s->val[i];
            }
        oyl_nmap_free(s);
        s->node = big.node;
        s->val = big.val;
        s->cap = big.cap;
    }
    uint32_t i = nmap_slot(s, x);
    s->node[i] = x;
    s->val[i] = v;
    s->len++;
    return true;
}

/* ── Finding keys ────────────────────────────────────────── */

/* A lookup: by key, or by text */
typedef struct {
    const oyl_tree *t;
    const oyl_key  *key;    /* the key equal to this, or */
    oyl_str         text;   /* any scalar key with this text */
    bool           *oom;    /* set if memory ran out: the answer can't be trusted */
} getter;

static bool matches(const getter *g, uint32_t k) {
    const oyl_tree *t = g->t;
    k = oyl_node_target(t, k);
    const oyl_node *n = &t->nodes[k];
    if (n->kind != OYL_NODE_SCALAR) return false;
    if (!g->key)
        return n->len == g->text.len &&
               (g->text.len == 0 || memcmp(oyl_tree_text(t, n).data, g->text.data, g->text.len) == 0);
    oyl_key key;
    if (!key_of(t, k, &key)) {
        *g->oom = true;
        return false;
    }
    return oyl_key_cmp(&key, g->key) == 0;
}

/* A mapping in a lookup's walk: its entries list[lo, at) still to look
 * at, from the last, and the items of the merge being looked at: its own,
 * or a shared sequence's in its note */
typedef struct {
    uint32_t map, lo, at;
    uint32_t item, left;
    uint32_t note;   /* the shared sequence's note, or NONE */
} visit;

/* A sequence that a walk meets through an alias, which many mappings may
 * merge: its items are gone through once, however many merge it, and
 * whether it can merge into a mapping follows from two facts. A mapping
 * inside the sequence is inside one of its items, so never merges it; for
 * one outside, only those of its items' mappings that contain the
 * sequence can contain the mapping, and they nest: the outermost decides. */
typedef struct {
    uint32_t outer;        /* the outermost of its items' mappings that contains it, or NONE */
    uint32_t item, left;   /* its items not yet gone through */
    bool     maps;         /* its items are all mappings */
} seq_note;

typedef struct {
    oyl_node_map index;    /* each sequence's note, by node */
    seq_note    *note, note_buf[8];
    uint32_t     n, cap;
} seq_notes;

/* Can value v of a << in mapping m be merged (as mergeable says)? An alias
 * to a sequence is looked at once in a walk. *note: the sequence's note,
 * or NONE; -1 if out of memory. */
static int walk_mergeable(const oyl_tree *t, seq_notes *sn, uint32_t m, uint32_t v, uint32_t *note) {
    const oyl_node *n = t->nodes;
    uint32_t s = merge_value(t, v), i;
    *note = NONE;
    if (s == v) return mergeable(t, m, v);
    if ((i = oyl_nmap_get(&sn->index, s)) == NONE) {
        if (!room((void **)&sn->note, sn->note_buf, &sn->cap, sn->n, sizeof *sn->note) ||
            !oyl_nmap_put(&sn->index, s, sn->n))
            return -1;
        seq_note q = { NONE, s + 1, n[s].len, true };
        for (uint32_t c = 0, item = s + 1; c < n[s].len; c++, item += n[item].span) {
            uint32_t x = oyl_node_target(t, item);
            if (n[x].kind != OYL_NODE_MAPPING) q.maps = false;
            else if (x < q.outer && x <= s && s < x + n[x].span) q.outer = x;
        }
        sn->note[i = sn->n++] = q;
    }
    *note = i;
    const seq_note *q = &sn->note[i];
    if (s < m && m < s + n[s].span) return 0;
    return q->maps && !(q->outer != NONE && q->outer <= m && m < q->outer + n[q->outer].span);
}

/* The key that counts among a mapping's pairs worked out: the first that
 * matches in the order keys win (a key matches once, text more often) */
static uint32_t get_worked_out(const getter *g, const merged_pair *p, uint32_t n) {
    uint32_t found = NONE, best = NONE;
    for (uint32_t i = 0; i < n; i++)
        if (p[i].rank < best && matches(g, p[i].key)) {
            found = p[i].key;
            best = p[i].rank;
            if (g->key) break;
        }
    return found;
}

/* The key that counts in mapping m, which merges: the first that matches
 * in the order keys win, each mapping visited once. In YAML's merge, a
 * mapping's own keys (the last written of them), then its merges, a later
 * << first and a sequence's items in order; in go-yaml v2's, its entries
 * from the last, a merge's items in order. NONE also if out of memory,
 * with *oom set. */
static uint32_t get_merged(const getter *g, uint32_t m) {
    const oyl_tree *t = g->t;
    const oyl_node *n = t->nodes;
    bool ordered = t->merge == OYL_MERGE_ORDERED;
    oyl_node_map seen;
    oyl_nmap_init(&seen, &t->hash_key);
    seq_notes sn = { .cap = 8 };
    sn.note = sn.note_buf;
    oyl_nmap_init(&sn.index, &t->hash_key);
    uint32_t list_buf[64], *list = list_buf, nlist = 0, list_cap = 64;
    visit stack_buf[16], *stack = stack_buf;
    uint32_t nstack = 0, stack_cap = 16;
    uint32_t found = NONE, next = m;   /* next: a mapping to enter */
    bool ok = oyl_nmap_put(&seen, m, 0);
    while (ok) {
        if (next != NONE) {
            /* YAML's merge looks at its own keys now and keeps its merges
             * for later; go-yaml v2's order keeps all its entries */
            uint32_t s = next, lo = nlist, k = s + 1, note;
            next = NONE;
            for (uint32_t c = 0; c < n[s].len && ok; c++) {
                uint32_t v = k + n[k].span;
                int merges = !ordered && is_merge_key(t, k) ? walk_mergeable(t, &sn, s, v, &note) : 0;
                if (merges < 0) {
                    ok = false;
                } else if (!ordered && !merges) {
                    if (matches(g, k)) found = k;
                } else if ((ok = room((void **)&list, list_buf, &list_cap, nlist, sizeof *list))) {
                    list[nlist++] = k;
                }
                k = v + n[v].span;
            }
            if (!ok || found != NONE) break;
            if ((ok = room((void **)&stack, stack_buf, &stack_cap, nstack, sizeof *stack)))
                stack[nstack++] = (visit){ .map = s, .lo = lo, .at = nlist, .note = NONE };
            continue;
        }
        if (!nstack) break;
        visit *f = &stack[nstack - 1];
        /* a shared sequence's items before its note's are visited, or about
         * to be by a walk below this one: it goes on where that is */
        uint32_t *item = f->note != NONE ? &sn.note[f->note].item : &f->item;
        uint32_t *left = f->note != NONE ? &sn.note[f->note].left : &f->left;
        if (*left) {   /* the merge's next mapping, unless visited */
            uint32_t x = oyl_node_target(t, *item);
            *item += n[*item].span;
            (*left)--;
            if (oyl_nmap_get(&seen, x) == NONE && (ok = oyl_nmap_put(&seen, x, 0))) next = x;
        } else if (f->at > f->lo) {
            uint32_t k = list[--f->at], v = k + n[k].span, note;
            int merges = is_merge_key(t, k) ? walk_mergeable(t, &sn, f->map, v, &note) : 0;
            if (merges < 0) {
                ok = false;
            } else if (merges) {
                f->note = note;
                if (note == NONE) f->left = merge_items(t, v, &f->item);
            } else if (matches(g, k)) {
                found = k;
                break;
            }
        } else {
            nlist = f->lo;
            nstack--;
        }
    }
    oyl_nmap_free(&seen);
    oyl_nmap_free(&sn.index);
    if (sn.note != sn.note_buf) free(sn.note);
    if (list != list_buf) free(list);
    if (stack != stack_buf) free(stack);
    if (!ok) *g->oom = true;
    return ok ? found : NONE;
}

/* The key in mapping `map` that counts for `key`, or NONE; if memory runs
 * out, *oom is set and the answer is NONE */
static uint32_t get_key(const oyl_tree *t, uint32_t map, const oyl_key *key, oyl_str text, bool *oom) {
    const oyl_node *n = t->nodes;
    getter g = { .t = t, .key = key, .text = text, .oom = oom };
    /* with the merges worked out, a mapping that merges has its pairs, and
     * one without them merges nothing: its << keys are ordinary keys */
    if (t->merges) {
        const merged_map *r = merged_of(t, map);
        if (r) {
            uint32_t found = get_worked_out(&g, &t->merges->pairs[r->first], r->n + (key ? 0 : r->spelled));
            return *oom ? NONE : found;
        }
    }
    bool merging = t->merge != OYL_MERGE_NONE && !t->merges;
    uint32_t found = NONE, last_merge = NONE, k = map + 1;
    for (uint32_t c = 0; c < n[map].len; c++) {
        uint32_t v = k + n[k].span;
        if (merging && is_merge_key(t, k) && mergeable(t, map, v)) last_merge = k;
        else if (matches(&g, k)) found = k;
        k = v + n[v].span;
    }
    if (*oom) return NONE;
    /* a key written wins, in go-yaml v2's order over the merges before it */
    if (last_merge == NONE || (found != NONE && (t->merge == OYL_MERGE_YAML || found > last_merge)))
        return found;
    found = get_merged(&g, map);
    return *oom ? NONE : found;
}

static uint32_t value_of(const oyl_tree *t, uint32_t k) {
    return k == NONE ? NONE : k + t->nodes[k].span;
}

uint32_t oyl_node_get(const oyl_tree *t, uint32_t map, oyl_str key) {
    map = oyl_node_target(t, map);
    if (map == NONE || t->nodes[map].kind != OYL_NODE_MAPPING) return NONE;
    /* the string, else the key a scalar written as `key` is (80 for "80") */
    oyl_key str = { t->schema->default_quoted_tag, { .kind = OYL_VALUE_STR, .as = { .s = key } } };
    bool oom = false;
    uint32_t k = get_key(t, map, &str, key, &oom);
    if (oom) return NONE;   /* not "no string key": by text it could find another */
    if (k != NONE) return value_of(t, k);
    if ((k = get_key(t, map, NULL, key, &oom)) == NONE) return NONE;
    k = oyl_node_target(t, k);
    oyl_key written;
    if (!key_of(t, k, &written)) return NONE;
    return value_of(t, get_key(t, map, &written, key, &oom));
}

uint32_t oyl_node_get_value(const oyl_tree *t, uint32_t map, const oyl_value *key) {
    map = oyl_node_target(t, map);
    if (!key || map == NONE || t->nodes[map].kind != OYL_NODE_MAPPING) return NONE;
    oyl_str tag;
    switch (key->kind) {
    case OYL_VALUE_NULL:  tag = OYL_TAG_NULL; break;
    case OYL_VALUE_BOOL:  tag = OYL_TAG_BOOL; break;
    case OYL_VALUE_INT:
    case OYL_VALUE_UINT:  tag = OYL_TAG_INT; break;
    case OYL_VALUE_FLOAT: tag = OYL_TAG_FLOAT; break;
    default:              tag = t->schema->default_quoted_tag; break;
    }
    oyl_key want;
    oyl_key_make(&want, tag, OYL_STR_NULL, OYL_OK, key);
    bool oom = false;
    return value_of(t, get_key(t, map, &want, OYL_STR_NULL, &oom));
}

/* ── A mapping's entries ─────────────────────────────────── */

/* An entry while a mapping's entries are settled: of those with the same
 * key, the highest priority wins, at the first position */
typedef struct {
    uint32_t key, value;
    uint64_t prio;
    uint32_t pos;
    uint32_t form;      /* a scalar key's in the collector's forms; else NONE, by node */
    bool     written;   /* written in the mapping asked about */
} ent;

typedef struct {
    const oyl_tree *t;
    unsigned        flags;
    ent            *e, *tmp, *e_buf;
    uint32_t        len, cap, tmp_cap;
    oyl_key        *forms, *forms_buf;   /* the scalar keys as compared, apart: entries move */
    uint32_t        nforms, forms_cap;
    oyl_status      status;
    uint32_t        bad;   /* the repeated key, under OYL_ENTRIES_STRICT */
} collector;

ALWAYS_INLINE bool push_ent(collector *c, ent e) {
    if (!room((void **)&c->e, c->e_buf, &c->cap, c->len, sizeof *c->e)) { c->status = OYL_ERR_MEMORY; return false; }
    uint32_t k = oyl_node_target(c->t, e.key);
    e.form = NONE;
    if (c->t->nodes[k].kind == OYL_NODE_SCALAR) {
        if (!room((void **)&c->forms, c->forms_buf, &c->forms_cap, c->nforms, sizeof *c->forms)) {
            c->status = OYL_ERR_MEMORY;
            return false;
        }
        if (!key_of(c->t, k, &c->forms[c->nforms])) {
            c->status = OYL_ERR_MEMORY;
            return false;
        }
        e.form = c->nforms++;
    }
    c->e[c->len++] = e;
    return true;
}

/* Order by key (scalars by tag and value, collections by node), then
 * priority */
static int by_key(const collector *c, const ent *a, const ent *b) {
    bool sa = a->form != NONE, sb = b->form != NONE;
    if (sa != sb) return sa ? -1 : 1;
    if (sa) {
        int r = oyl_key_cmp(&c->forms[a->form], &c->forms[b->form]);
        if (r) return r;
    } else {
        uint32_t ka = oyl_node_target(c->t, a->key), kb = oyl_node_target(c->t, b->key);
        if (ka != kb) return ka < kb ? -1 : 1;
    }
    return a->prio < b->prio ? -1 : a->prio > b->prio;
}

static int by_pos(const collector *c, const ent *a, const ent *b) {
    (void)c;
    return a->pos < b->pos ? -1 : a->pos > b->pos;
}

typedef int (*ent_cmp)(const collector *, const ent *, const ent *);

static void sort_ents(const collector *c, ent *e, ent *tmp, uint32_t n, ent_cmp cmp) {
    if (n < 2) return;
    if (n <= 16) {   /* insertion sort */
        for (uint32_t i = 1; i < n; i++) {
            ent x = e[i];
            uint32_t j = i;
            while (j > 0 && cmp(c, &e[j - 1], &x) > 0) { e[j] = e[j - 1]; j--; }
            e[j] = x;
        }
        return;
    }
    uint32_t h = n / 2;
    sort_ents(c, e, tmp, h, cmp);
    sort_ents(c, e + h, tmp, n - h, cmp);
    uint32_t i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = cmp(c, &e[j], &e[i]) < 0 ? e[j++] : e[i++];
    while (i < h) tmp[k++] = e[i++];
    while (j < n) tmp[k++] = e[j++];
    memcpy(e, tmp, n * sizeof *e);
}

static bool same_key(const collector *c, const ent *a, const ent *b) {
    ent x = *a, y = *b;
    x.prio = y.prio = 0;
    return by_key(c, &x, &y) == 0;
}


/* Settle the entries: one per key, the highest priority at the first
 * position, in order of position */
static bool settle(collector *c) {
    uint32_t n = c->len;
    if (n == 0) return true;
    if (n > 16 && n > c->tmp_cap) {
        ent *tmp = realloc(c->tmp, n * sizeof *tmp);
        if (!tmp) { c->status = OYL_ERR_MEMORY; return false; }
        c->tmp = tmp;
        c->tmp_cap = n;
    }
    ent *e = c->e;
    sort_ents(c, e, c->tmp, n, by_key);
    uint32_t out = 0, repeat = NONE;   /* the first key written again */
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i + 1, pos = e[i].pos, written = e[i].written;
        while (j < n && same_key(c, &e[j], &e[i])) {
            if (e[j].pos < pos) pos = e[j].pos;
            written += e[j++].written;
        }
        if (written > 1 && (c->flags & OYL_ENTRIES_STRICT)) {
            uint32_t first = NONE, second = NONE;
            for (uint32_t w = i; w < j; w++) {
                if (!e[w].written) continue;
                if (e[w].key < first) { second = first; first = e[w].key; }
                else if (e[w].key < second) second = e[w].key;
            }
            if (second < repeat) repeat = second;
        }
        e[out] = e[j - 1];
        e[out++].pos = pos;
        i = j;
    }
    if (repeat != NONE) {
        c->status = OYL_ERR_PARSE;
        c->bad = repeat;
        return false;
    }
    sort_ents(c, e, c->tmp, out, by_pos);
    c->len = out;
    return true;
}

/* Mapping m's entries, settled, into c->e. A mapping that merges has its
 * pairs worked out as the tree is built, unless that was too costly. */
static bool collect(collector *c, uint32_t m) {
    const oyl_tree *t = c->t;
    const oyl_node *n = t->nodes;
    bool merging = t->merge != OYL_MERGE_NONE;
    uint32_t k = m + 1;
    for (uint32_t i = 0; i < n[m].len; i++) {
        uint32_t v = k + n[k].span;
        if (merging && is_merge_key(t, k) && mergeable(t, m, v)) {
            c->status = OYL_ERR_LIMIT;
            return false;
        }
        if (!push_ent(c, (ent){ .key = k, .value = v, .prio = i, .pos = i, .written = true })) return false;
        k = v + n[v].span;
    }
    return settle(c);
}

oyl_status oyl_node_entries(const oyl_tree *t, uint32_t map, unsigned flags, oyl_arena *a,
                            const oyl_pair **pairs, size_t *n) {
    if (!pairs || !n) return OYL_ERR_INPUT;
    *pairs = NULL;
    *n = 0;
    if (!t || !a) return OYL_ERR_INPUT;
    map = oyl_node_target(t, map);
    if (map == NONE || t->nodes[map].kind != OYL_NODE_MAPPING) return OYL_ERR_INPUT;
    const merged_map *r = merged_of(t, map);
    if (r) {
        if ((flags & OYL_ENTRIES_STRICT) && r->bad != NONE) {
            *n = r->bad;
            return OYL_ERR_PARSE;
        }
        oyl_pair *out = oyl_arena_alloc(a, (r->n ? r->n : 1) * sizeof *out, _Alignof(oyl_pair));
        if (!out) return OYL_ERR_MEMORY;
        const merged_pair *p = &t->merges->pairs[r->first];
        for (uint32_t i = 0; i < r->n; i++) out[i] = (oyl_pair){ p[i].key, p[i].value };
        *pairs = out;
        *n = r->n;
        return OYL_OK;
    }
    ent e_buf[32];
    oyl_key forms_buf[32];
    collector c = { .t = t, .flags = flags, .e = e_buf, .e_buf = e_buf, .cap = 32,
                    .forms = forms_buf, .forms_buf = forms_buf, .forms_cap = 32 };
    oyl_status st = OYL_OK;
    if (!collect(&c, map)) {
        st = c.status;
        if (st == OYL_ERR_PARSE) *n = c.bad;
    } else {
        oyl_pair *out = oyl_arena_alloc(a, (c.len ? c.len : 1) * sizeof *out, _Alignof(oyl_pair));
        if (!out) {
            st = OYL_ERR_MEMORY;
        } else {
            for (uint32_t i = 0; i < c.len; i++) out[i] = (oyl_pair){ c.e[i].key, c.e[i].value };
            *pairs = out;
            *n = c.len;
        }
    }
    if (c.e != e_buf) free(c.e);
    if (c.forms != forms_buf) free(c.forms);
    free(c.tmp);
    return st;
}

/* ── Working out merges ──────────────────────────────────── */

/* A mapping in the graph of a mapping's merges, one deep: the mapping, or
 * one it merges, whose pairs are worked out */
typedef struct {
    uint32_t base, n;           /* its entries: c->e[base, base + n) */
    uint32_t pairs;             /* a mapping merged: its pairs, mg->pairs[pairs, pairs + n) */
    uint32_t merges, nmerges;   /* its merges: g->mrg[merges, merges + nmerges) */
    uint32_t w, m, i;           /* a walk's place: own entries, merges, items */
    bool     worked;            /* a mapping merged: each entry's prio is its rank */
    bool     seen;              /* by the walk */
} source;

/* A merge, in the mapping that has it */
typedef struct {
    uint32_t before;    /* the mapping's own entries before it */
    uint32_t items, n;  /* its mappings, as sources: g->items[items, items + n) */
} merge_ref;

/* A key, while a mapping's pairs are worked out */
typedef struct {
    uint32_t win;            /* its entry that wins: the highest priority */
    uint32_t first, second;  /* its two least keys written in the mapping */
} key_group;

/* A spelling of a key (its text) */
typedef struct {
    oyl_str  text;
    uint64_t prio;
    uint32_t entry;   /* the entry */
    bool     wins;    /* its key's entry that wins */
} spelling;

/* What working out the merges needs, kept from mapping to mapping */
typedef struct {
    const oyl_tree     *t;
    struct oyl_merges  *mg;
    collector           c;
    uint64_t            left;       /* the entries and merge items it may still go through */
    source             *src;        /* a mapping's graph */
    merge_ref          *mrg;
    uint32_t           *items, *stack;
    uint32_t            nsrc, src_cap, nmrg, mrg_cap, nitems, items_cap, stack_cap;
    oyl_node_map        index;      /* each source, by where its pairs start */
    uint32_t           *order;      /* settling: the entries by position */
    uint32_t           *group;      /* each entry's key */
    uint32_t           *ranked;     /* the pairs kept, by priority from the highest */
    uint32_t           *table;      /* the keys, by hash */
    oyl_str             tag;        /* the last key's tag, and its part of a key's hash */
    oyl_hasher          tag_hash;
    key_group          *groups;
    spelling           *spellings;
    uint32_t            scap, tcap;
} worker;

static const merged_map *merged_in(const struct oyl_merges *mg, uint32_t m) {
    uint32_t i = oyl_nmap_get(&mg->index, m);
    return i == NONE ? NULL : &mg->maps[i];
}

/* Mapping m's graph: m with its own entries, then each mapping it merges
 * with its pairs worked out, once (mappings whose pairs are the same
 * pairs, once). OYL_ERR_LIMIT past the entries left, which it takes. */
static bool gather(worker *wk, uint32_t m) {
    const oyl_tree *t = wk->t;
    const oyl_node *n = t->nodes;
    collector *c = &wk->c;
    wk->nsrc = wk->nmrg = wk->nitems = 0;
    if (!grow((void **)&wk->src, &wk->src_cap, 0, 1, sizeof *wk->src)) goto oom;
    wk->src[wk->nsrc++] = (source){ 0 };
    uint32_t k = m + 1, own = 0;
    for (uint32_t i = 0; i < n[m].len; i++) {
        uint32_t v = k + n[k].span;
        if (is_merge_key(t, k) && mergeable(t, m, v)) {
            uint32_t item, count = merge_items(t, v, &item);
            if (!grow((void **)&wk->mrg, &wk->mrg_cap, wk->nmrg, 1, sizeof *wk->mrg) ||
                !grow((void **)&wk->items, &wk->items_cap, wk->nitems, count, sizeof *wk->items))
                goto oom;
            merge_ref *r = &wk->mrg[wk->nmrg++];
            *r = (merge_ref){ .before = own, .items = wk->nitems };
            for (uint32_t j = 0; j < count; j++, item += n[item].span) {
                const merged_map *x = merged_in(wk->mg, oyl_node_target(t, item));
                if (!x->n) continue;   /* nothing to add */
                uint32_t at = oyl_nmap_get(&wk->index, x->first);
                if (at == NONE) {
                    at = wk->nsrc;
                    if (!grow((void **)&wk->src, &wk->src_cap, wk->nsrc, 1, sizeof *wk->src) ||
                        !oyl_nmap_put(&wk->index, x->first, at))
                        goto oom;
                    wk->src[wk->nsrc++] = (source){ .pairs = x->first, .n = x->n + x->spelled, .worked = true };
                }
                wk->items[wk->nitems++] = at;
                r->n++;
            }
        } else {
            if (!push_ent(c, (ent){ .key = k, .value = v, .written = true })) return false;
            own++;
        }
        k = v + n[v].span;
    }
    wk->src[0] = (source){ .n = own, .nmerges = wk->nmrg };
    /* the pairs of the mappings merged, after m's own entries */
    for (uint32_t q = 1; q < wk->nsrc; q++) {
        source *s = &wk->src[q];
        if (s->n > wk->left || c->len > wk->left - s->n) {
            c->status = OYL_ERR_LIMIT;
            return false;
        }
        s->base = c->len;
        const merged_pair *p = &wk->mg->pairs[s->pairs];
        for (uint32_t j = 0; j < s->n; j++)
            if (!push_ent(c, (ent){ .key = p[j].key, .value = p[j].value, .prio = p[j].rank })) return false;
    }
    if (c->len > wk->left) {
        c->status = OYL_ERR_LIMIT;
        return false;
    }
    wk->left -= c->len;
    return true;
oom:
    c->status = OYL_ERR_MEMORY;
    return false;
}

/* Number the entries in the order they are written: the mapping's own
 * entries, a merge's mappings where it stands, each mapping once */
static void walk_written(worker *wk) {
    ent *e = wk->c.e;
    uint32_t pos = 0, top = 0;
    for (uint32_t q = 0; q < wk->nsrc; q++) {
        source *s = &wk->src[q];
        s->w = s->m = s->i = 0;
        s->seen = false;
    }
    wk->stack[top++] = 0;
    wk->src[0].seen = true;
    while (top) {
        source *s = &wk->src[wk->stack[top - 1]];
        if (s->m < s->nmerges) {
            const merge_ref *r = &wk->mrg[s->merges + s->m];
            while (s->w < r->before) e[s->base + s->w++].pos = pos++;
            if (s->i < r->n) {
                uint32_t x = wk->items[r->items + s->i++];
                if (!wk->src[x].seen) {
                    wk->src[x].seen = true;
                    wk->stack[top++] = x;
                }
            } else {
                s->m++;
                s->i = 0;
            }
            continue;
        }
        while (s->w < s->n) e[s->base + s->w++].pos = pos++;
        top--;
    }
}

/* Rank the entries in the order keys win, the first highest. YAML's
 * merge: a mapping's own entries, the last written first, then its
 * merges, a later << first, a sequence's items in order. go-yaml v2's
 * order: its entries from the last, a merge's items in order. A mapping
 * merged, its pairs in the order they win there. */
static void walk_winning(worker *wk, bool ordered) {
    ent *e = wk->c.e;
    uint64_t prio = UINT64_MAX;
    uint32_t top = 0;
    for (uint32_t q = 0; q < wk->nsrc; q++) {
        source *s = &wk->src[q];
        s->w = s->n;
        s->m = s->nmerges;
        s->i = 0;
        s->seen = false;
    }
    wk->stack[top++] = 0;
    wk->src[0].seen = true;
    while (top) {
        source *s = &wk->src[wk->stack[top - 1]];
        if (s->worked) {
            for (uint32_t j = 0; j < s->n; j++) e[s->base + j].prio = prio - e[s->base + j].prio;
            prio -= s->n;
            top--;
            continue;
        }
        if (!ordered) while (s->w > 0) e[s->base + --s->w].prio = prio--;
        if (s->m > 0) {
            const merge_ref *r = &wk->mrg[s->merges + s->m - 1];
            while (s->w > r->before) e[s->base + --s->w].prio = prio--;
            if (s->i < r->n) {
                uint32_t x = wk->items[r->items + s->i++];
                if (!wk->src[x].seen) {
                    wk->src[x].seen = true;
                    wk->stack[top++] = x;
                }
            } else {
                s->m--;
                s->i = 0;
            }
            continue;
        }
        while (s->w > 0) e[s->base + --s->w].prio = prio--;
        top--;
    }
}

/* Key k's text, if a scalar's */
static oyl_str key_text(const oyl_tree *t, uint32_t k) {
    k = oyl_node_target(t, k);
    return t->nodes[k].kind == OYL_NODE_SCALAR ? oyl_tree_text(t, &t->nodes[k]) : OYL_STR_NULL;
}

/* An entry's key's hash: of its tag and value, or of the collection it
 * is (a document picks both). Most keys share a tag, their schema's, so
 * the hash's part for the last tag is kept. */
static uint64_t ent_hash(worker *wk, const ent *e) {
    const collector *c = &wk->c;
    const oyl_hash_key *hk = &wk->t->hash_key;
    if (e->form == NONE) return oyl_hash_u64(hk, oyl_node_target(wk->t, e->key));
    const oyl_key *k = &c->forms[e->form];
    if (k->tag.data != wk->tag.data || k->tag.len != wk->tag.len) {
        oyl_key_hash_tag(&wk->tag_hash, hk, k->tag);
        wk->tag = k->tag;
    }
    oyl_hasher h = wk->tag_hash;
    return oyl_key_hash_value(&h, &k->value);
}

static int by_spelling(const void *pa, const void *pb) {
    const spelling *a = pa, *b = pb;
    int r = oyl_bytes_cmp(a->text, b->text);
    if (r) return r;
    return a->prio > b->prio ? -1 : a->prio < b->prio;
}

/* Room for settling w entries, with a table of `tcap` keys */
static bool settling_room(worker *wk, uint32_t w, uint32_t tcap) {
    if (tcap > wk->tcap) {
        uint32_t *table = realloc(wk->table, (size_t)tcap * sizeof *table);
        if (!table) return false;
        wk->table = table;
        wk->tcap = tcap;
    }
    if (w <= wk->scap) return true;
    uint32_t cap = wk->scap ? wk->scap : 64;
    while (cap < w) cap *= 2;
    uint32_t *order = realloc(wk->order, (size_t)cap * sizeof *order);
    if (order) wk->order = order;
    uint32_t *group = realloc(wk->group, (size_t)cap * sizeof *group);
    if (group) wk->group = group;
    uint32_t *ranked = realloc(wk->ranked, (size_t)cap * sizeof *ranked);
    if (ranked) wk->ranked = ranked;
    key_group *groups = realloc(wk->groups, (size_t)cap * sizeof *groups);
    if (groups) wk->groups = groups;
    spelling *sp = realloc(wk->spellings, (size_t)cap * sizeof *sp);
    if (sp) wk->spellings = sp;
    if (!order || !group || !ranked || !groups || !sp) return false;
    wk->scap = cap;
    return true;
}

/* Keep mapping m's pairs, worked out from the entries gathered, which the
 * walks numbered: their positions 0 to w - 1 and their priorities the w
 * highest. In the order they are written, each key at its first place
 * with its entry that wins. Then the keys merged that one of them
 * overrides, spelled otherwise, each the first of its spelling in the
 * order keys win (oyl_node_get finds keys by text). Each ranked in that
 * order. */
static bool keep(worker *wk, uint32_t m) {
    collector *c = &wk->c;
    struct oyl_merges *mg = wk->mg;
    const ent *e = c->e;
    uint32_t w = c->len, ng = 0, repeat = NONE, tcap = 16;
    while (tcap < 2 * (uint64_t)w) tcap *= 2;
    if (!settling_room(wk, w, tcap)) return false;
    for (uint32_t i = 0; i < w; i++) wk->order[e[i].pos] = i;
    uint32_t mask = tcap - 1;
    for (uint32_t i = 0; i <= mask; i++) wk->table[i] = NONE;
    for (uint32_t p = 0; p < w; p++) {
        uint32_t i = wk->order[p], g, s = (uint32_t)ent_hash(wk, &e[i]) & mask;
        while ((g = wk->table[s]) != NONE && !same_key(c, &e[wk->groups[g].win], &e[i])) s = (s + 1) & mask;
        if (g == NONE) {
            g = wk->table[s] = ng++;
            wk->groups[g] = (key_group){ i, NONE, NONE };
        } else if (e[i].prio > e[wk->groups[g].win].prio) {
            wk->groups[g].win = i;
        }
        key_group *kg = &wk->groups[g];
        if (e[i].written) {
            if (e[i].key < kg->first) { kg->second = kg->first; kg->first = e[i].key; }
            else if (e[i].key < kg->second) kg->second = e[i].key;
        }
        wk->group[i] = g;
    }
    for (uint32_t g = 0; g < ng; g++)
        if (wk->groups[g].second < repeat) repeat = wk->groups[g].second;
    /* the keys spelled otherwise: those whose text isn't their winner's,
     * among the spellings of all, the first of each */
    uint32_t nsp = 0, spelled = 0;
    for (uint32_t i = 0; i < w; i++) {
        uint32_t win = wk->groups[wk->group[i]].win;
        if (i == win || e[i].form == NONE) continue;
        oyl_str text = key_text(wk->t, e[i].key);
        if (!same(text, key_text(wk->t, e[win].key))) wk->spellings[nsp++] = (spelling){ text, e[i].prio, i, false };
    }
    if (nsp) {
        for (uint32_t g = 0; g < ng; g++) {
            uint32_t win = wk->groups[g].win;
            if (e[win].form != NONE)
                wk->spellings[nsp++] = (spelling){ key_text(wk->t, e[win].key), e[win].prio, win, true };
        }
        qsort(wk->spellings, nsp, sizeof *wk->spellings, by_spelling);
        for (uint32_t i = 0; i < nsp; i++)
            if ((i == 0 || !same(wk->spellings[i].text, wk->spellings[i - 1].text)) && !wk->spellings[i].wins)
                wk->spellings[spelled++] = wk->spellings[i];
    }
    uint32_t total = ng + spelled;
    if (!grow((void **)&mg->pairs, &mg->pairs_cap, mg->npairs, total, sizeof *mg->pairs) ||
        !grow((void **)&mg->maps, &mg->maps_cap, mg->nmaps, 1, sizeof *mg->maps) ||
        !oyl_nmap_put(&mg->index, m, mg->nmaps))
        return false;
    /* rank them: priorities are the w highest, so by how far below the top */
    for (uint32_t i = 0; i < w; i++) wk->ranked[i] = NONE;
    for (uint32_t g = 0; g < ng; g++) wk->ranked[UINT64_MAX - e[wk->groups[g].win].prio] = g;
    for (uint32_t j = 0; j < spelled; j++) wk->ranked[UINT64_MAX - wk->spellings[j].prio] = ng + j;
    merged_pair *out = &mg->pairs[mg->npairs];
    for (uint32_t i = 0, rank = 0; i < w; i++) {
        uint32_t x = wk->ranked[i];
        if (x == NONE) continue;
        const ent *y = &e[x < ng ? wk->groups[x].win : wk->spellings[x - ng].entry];
        out[x] = (merged_pair){ y->key, y->value, rank++ };
    }
    mg->maps[mg->nmaps++] = (merged_map){ mg->npairs, ng, spelled, repeat };
    mg->npairs += total;
    return true;
}

/* Work out mapping m's pairs if it merges, and first those of each
 * mapping it merges that merges nothing (those that merge, ending first,
 * are worked out already) */
static bool work_out(worker *wk, uint32_t m) {
    const oyl_tree *t = wk->t;
    const oyl_node *n = t->nodes;
    collector *c = &wk->c;
    merged_map one = { 0, 0, 0, NONE };   /* the pairs merged, if only one mapping's */
    bool merges = false, several = false;
    uint32_t k = m + 1, own = 0;
    for (uint32_t i = 0; i < n[m].len; i++) {
        uint32_t v = k + n[k].span;
        bool merge = is_merge_key(t, k);
        if (merge) {   /* its items, gone through here and in gather, may bring no pairs */
            uint32_t item, count = merge_items(t, v, &item);
            if (count > wk->left) {
                c->status = OYL_ERR_LIMIT;
                return false;
            }
            wk->left -= count;
        }
        if (merge && mergeable(t, m, v)) {
            merges = true;
            uint32_t item, count = merge_items(t, v, &item);
            for (uint32_t j = 0; j < count; j++, item += n[item].span) {
                uint32_t x = oyl_node_target(t, item);
                const merged_map *r = merged_in(wk->mg, x);
                if (!r) {   /* its own pairs, the last written winning */
                    uint32_t w = n[x].len, kx = x + 1;
                    if (w > wk->left) {
                        c->status = OYL_ERR_LIMIT;
                        return false;
                    }
                    wk->left -= w;
                    c->len = c->nforms = 0;
                    for (uint32_t p = 0; p < w; p++) {
                        uint32_t vx = kx + n[kx].span;
                        if (!push_ent(c, (ent){ .key = kx, .value = vx, .prio = UINT64_MAX - (w - 1 - p),
                                                .pos = p, .written = true }))
                            return false;
                        kx = vx + n[vx].span;
                    }
                    if (!keep(wk, x)) goto oom;
                    r = &wk->mg->maps[wk->mg->nmaps - 1];
                }
                if (!r->n) continue;
                if (!one.n) one = (merged_map){ r->first, r->n, r->spelled, NONE };
                else if (r->first != one.first) several = true;
            }
        } else {
            own++;
        }
        k = v + n[v].span;
    }
    if (!merges) return true;
    if (!own && !several) {   /* the pairs of the one mapping merged */
        if (!grow((void **)&wk->mg->maps, &wk->mg->maps_cap, wk->mg->nmaps, 1, sizeof *wk->mg->maps) ||
            !oyl_nmap_put(&wk->mg->index, m, wk->mg->nmaps))
            goto oom;
        wk->mg->maps[wk->mg->nmaps++] = one;
        return true;
    }
    c->len = c->nforms = 0;
    oyl_nmap_init(&wk->index, &t->hash_key);
    bool ok = gather(wk, m);
    oyl_nmap_free(&wk->index);
    if (!ok) return false;
    if (!grow((void **)&wk->stack, &wk->stack_cap, 0, wk->nsrc, sizeof *wk->stack)) goto oom;
    walk_written(wk);
    walk_winning(wk, t->merge == OYL_MERGE_ORDERED);
    if (!keep(wk, m)) goto oom;
    return true;
oom:
    c->status = OYL_ERR_MEMORY;
    return false;
}

/* Work out the pairs of tree t's mappings that merge, in the order the
 * mappings end. The entries gathered are bounded as the parser's merges
 * for events are; past that, nothing is kept. False if out of memory. */
static bool work_out_merges(oyl_tree *t) {
    const oyl_node *n = t->nodes;
    worker wk = { .t = t, .mg = calloc(1, sizeof *wk.mg),
                  .left = ((uint64_t)t->len * 16 + 100000) * 4 };   /* as the parser's for events */
    if (!wk.mg) return false;
    /* under 2^30, so that a mapping's entries (at most this many) fit a
     * settling table of twice as many slots, a power of two, in 32 bits */
    if (wk.left >= 1u << 30) wk.left = (1u << 30) - 1;
    oyl_key_hash_tag(&wk.tag_hash, &t->hash_key, wk.tag);
    oyl_nmap_init(&wk.mg->index, &t->hash_key);
    /* the pairs exist even if none are kept, so that offsets into them are
     * never taken from NULL */
    if (!grow((void **)&wk.mg->pairs, &wk.mg->pairs_cap, 0, 1, sizeof *wk.mg->pairs)) {
        free_merges(wk.mg);
        return false;
    }
    ent e_buf[32];
    oyl_key forms_buf[32];
    wk.c = (collector){ .t = t, .e = e_buf, .e_buf = e_buf, .cap = 32,
                        .forms = forms_buf, .forms_buf = forms_buf, .forms_cap = 32 };
    uint32_t *open = NULL, nopen = 0, open_cap = 0;   /* mappings not ended yet */
    bool ok = true;
    for (uint32_t i = 0; ok && i <= t->len; i++) {
        while (ok && nopen && (i == t->len || open[nopen - 1] + n[open[nopen - 1]].span <= i))
            ok = work_out(&wk, open[--nopen]);
        if (ok && i < t->len && n[i].kind == OYL_NODE_MAPPING) {
            if ((ok = grow((void **)&open, &open_cap, nopen, 1, sizeof *open))) open[nopen++] = i;
            else wk.c.status = OYL_ERR_MEMORY;
        }
    }
    free(open);
    free(wk.src);
    free(wk.mrg);
    free(wk.items);
    free(wk.stack);
    free(wk.order);
    free(wk.group);
    free(wk.ranked);
    free(wk.table);
    free(wk.groups);
    free(wk.spellings);
    if (wk.c.e != e_buf) free(wk.c.e);
    if (wk.c.forms != forms_buf) free(wk.c.forms);
    if (ok) {
        t->merges = wk.mg;
        return true;
    }
    free_merges(wk.mg);
    if (wk.c.status != OYL_ERR_LIMIT) return false;
    t->merges_costly = true;
    return true;
}

/* ── Writing ─────────────────────────────────────────────── */

/* Emit node i, as the event that read it would be */
static oyl_status emit_node(oyl_emitter *e, const oyl_tree *t, uint32_t i) {
    const oyl_node *n = &t->nodes[i];
    if (n->kind == OYL_NODE_ALIAS) return oyl_emit_alias(e, oyl_tree_text(t, n));
    oyl_str tag = OYL_STR_NULL, anchor = OYL_STR_NULL;
    if (n->props) {
        const oyl_node_props *pr = &t->props[n->props];
        tag = pooled(t, pr->tag, pr->tag_len);
        anchor = pooled(t, pr->anchor, pr->anchor_len);
    }
    bool flow = (n->flags & OYL_NODE_FLOW) != 0;
    switch (n->kind) {
    case OYL_NODE_SCALAR:
        return oyl_emit_scalar(e, oyl_tree_text(t, n), (oyl_scalar_style)n->style, anchor, tag);
    case OYL_NODE_SEQUENCE: return oyl_emit_sequence_start(e, anchor, tag, flow);
    default:                return oyl_emit_mapping_start(e, anchor, tag, flow);
    }
}

/* End the collections from node j up through its ancestors, stopping at
 * `stop` (still open; NONE ends them all) */
static oyl_status end_up_to(oyl_emitter *e, const oyl_tree *t, uint32_t j, uint32_t stop) {
    const oyl_node *n = t->nodes;
    oyl_status st = OYL_OK;
    while (j != stop && st == OYL_OK) {
        if (n[j].kind == OYL_NODE_SEQUENCE) st = oyl_emit_sequence_end(e);
        else if (n[j].kind == OYL_NODE_MAPPING) st = oyl_emit_mapping_end(e);
        if (j == 0) break;
        j -= n[j].parent;
    }
    return st;
}

oyl_status oyl_emit_tree(oyl_emitter *e, const oyl_tree *t) {
    if (!e) return OYL_ERR_INPUT;
    if (!t) return oyl_emitter_fail(e, OYL_ERR_INPUT);
    /* the document's directives, but for those given for it already */
    oyl_status st = OYL_OK;
    if (t->has_version && !oyl_emitter_has_directive(e, OYL_STR_NULL))
        st = oyl_emit_yaml_directive(e, t->version_major, t->version_minor);
    for (int i = 0; i < t->tag_dir_count && st == OYL_OK; i++) {
        oyl_str h = pooled(t, t->tag_dirs[i].handle, t->tag_dirs[i].handle_len);
        if (!oyl_emitter_has_directive(e, h))
            st = oyl_emit_tag_directive(e, h, pooled(t, t->tag_dirs[i].prefix, t->tag_dirs[i].prefix_len));
    }
    if (st == OYL_OK) st = oyl_emit_document_start(e, t->implicit_start);
    for (uint32_t i = 0; i < t->len && st == OYL_OK; i++) {
        /* node i's parent is node i-1 or one of its ancestors: the
         * collections between them have ended */
        if (i) st = end_up_to(e, t, i - 1, i - t->nodes[i].parent);
        if (st == OYL_OK) st = emit_node(e, t, i);
    }
    if (st == OYL_OK) st = end_up_to(e, t, t->len - 1, NONE);
    if (st == OYL_OK) st = oyl_emit_document_end(e, t->implicit_end);
    return st;
}
