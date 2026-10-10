/*
 * oyl_parser.c — YAML 1.2 event parser
 *
 * Thin layer over the scanner: handles block structure via indent tracking,
 * simple key detection via deferred scalar approach, and flow context.
 *
 * Architecture:
 *   The parser consumes tokens from the scanner and produces events.
 *   Block structure (mappings, sequences) is tracked via an indent-based
 *   context stack. Simple keys are detected by peeking ahead for ':'
 *   after a scalar. Flow collections are handled with dedicated states.
 */

#include "oyl_internal.h"
#include "oyl_simd.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Context types ───────────────────────────────────────── */

typedef enum {
    CTX_BLOCK_MAP,
    CTX_BLOCK_SEQ,
    CTX_FLOW_MAP,
    CTX_FLOW_SEQ,
} ctx_type;

typedef struct {
    ctx_type type;
    int      indent;
} ctx_entry;

/* ── Incremental parser states ──────────────────────────── */

typedef enum {
    ST_STREAM_START, ST_STREAM_DOC_LOOP, ST_STREAM_END,
    ST_DOC_DIRECTIVES, ST_DOC_START_EXPLICIT, ST_DOC_START_IMPLICIT,
    ST_DOC_END_EXPLICIT, ST_DOC_CONTENT,
    ST_BLOCK_NODE, ST_BLOCK_NODE_SCALAR, ST_BLOCK_NODE_SCALAR_KEY,
    ST_BLOCK_NODE_ALIAS, ST_BLOCK_NODE_ALIAS_KEY,
    ST_BLOCK_NODE_SEQ, ST_BLOCK_NODE_EXPLICIT_KEY, ST_BLOCK_NODE_BARE_VALUE,
    ST_BLOCK_NODE_EMPTY,
    ST_BLOCK_MAP_LOOP, ST_BLOCK_MAP_EXPLICIT_KEY, ST_BLOCK_MAP_SIMPLE_KEY,
    ST_BLOCK_MAP_POST_KEY, ST_BLOCK_MAP_VALUE, ST_BLOCK_MAP_VALUE_NODE,
    ST_BLOCK_MAP_END,
    ST_BLOCK_SEQ_LOOP, ST_BLOCK_SEQ_ENTRY, ST_BLOCK_SEQ_ENTRY_NODE,
    ST_BLOCK_SEQ_END,
    ST_UNROLL,
    /* Flow sequence (incremental) */
    ST_FLOW_SEQ_LOOP, ST_FLOW_SEQ_ENTRY, ST_FLOW_SEQ_ENTRY_KEY,
    ST_FLOW_SEQ_IMPLICIT_END,
    ST_FLOW_SEQ_EXPLICIT_KEY, ST_FLOW_SEQ_EXPLICIT_VALUE,
    ST_FLOW_SEQ_EXPLICIT_END,
    ST_FLOW_SEQ_CHECK_COLON, ST_FLOW_SEQ_SEP, ST_FLOW_SEQ_END,
    /* Flow mapping (incremental) */
    ST_FLOW_MAP_LOOP, ST_FLOW_MAP_VALUE, ST_FLOW_MAP_SEP, ST_FLOW_MAP_END,
    /* Flow node dispatch (incremental) */
    ST_FLOW_NODE,
    ST_EAGER_DRAIN,
    ST_DONE,
} parser_state;

typedef struct {
    ctx_type     type;
    int          indent;
    parser_state return_state;
} state_frame;

/* ── Parser state ────────────────────────────────────────── */

struct oyl_parser {
    oyl_scanner *scanner;
    oyl_arena   *arena;
    const char  *input;
    size_t       input_len;

    /* token lookahead */
    oyl_token current;
    bool      have_token;

    /* event list (dynamic array, drained via cursor) */
    oyl_event *events;
    int        evt_len;
    int        evt_cap;
    int        evt_cursor;

    /* context stack */
    ctx_entry *contexts;
    int        ctx_len;
    int        ctx_cap;

    /* pending anchor/tag for next node */
    oyl_str pending_anchor;
    oyl_str pending_tag;
    bool    has_anchor;
    bool    has_tag;
    size_t  props_line;  /* line where first prop was consumed */
    int     props_col;   /* column (0-based) where first prop was consumed */
    oyl_mark props_start; /* full mark of first property token */

    /* tag directives: handle → prefix mapping */
    struct {
        char handle[16];  /* e.g., "!", "!!", "!e!" */
        char prefix[256]; /* e.g., "tag:example.com,2000:app/" */
    } tag_directives[8];
    int tag_dir_count;

    /* lifecycle */
    bool stream_started;
    bool stream_ended;
    bool doc_open;

    /* schema (optional, for tag resolution) */
    const oyl_schema *schema;

    /* merge key resolution (opt-in) */
    bool merge_enabled;

    /* alias resolution (opt-in) */
    bool resolve_enabled;

    /* safety limits */
    int max_events;
    int max_depth;

    /* line of the current document's '---' marker (0 if none): a block
     * collection may not start on that line */
    size_t doc_start_line;

    /* error context */
    char     error_msg[256];
    oyl_mark error_mark;
    bool     oom;         /* stop parsing (see stop_status) */
    oyl_status stop_status; /* why oom was set: 0 = allocation failure */

    /* incremental state machine */
    bool         incremental;   /* true = state machine, false = eager */
    parser_state state;
    state_frame *frames;
    int          frame_len;
    int          frame_cap;
    oyl_event    out_buf[8];    /* small output buffer for incremental */
    int          out_len;
    int          out_cursor;
    oyl_event    saved_node;    /* saved scalar/alias for : lookahead */
    int          saved_node_col;
    bool         have_saved_node;
    parser_state unroll_return;
    parser_state node_return;  /* where to go after a block node completes */
    int          events_delivered; /* events returned to caller (for eager fallback skip) */
    uint64_t     delivered_sig;    /* running signature of their types */
    oyl_status   scan_error;      /* last scanner error (for incremental path) */
    int          scan_error_out;  /* out_len when scan_error was hit */
    size_t       value_colon_line; /* line of the last block map value ':' */
    bool         value_colon_starts_line; /* and whether it starts that line */

    /* event handed out by the public oyl_parse_next() */
    oyl_event out_evt;

    /* original anchor names by binding serial (see bind_anchors) */
    oyl_str *bound_names;

    /* flow-as-key lookahead cache (see flow_is_block_key) */
    bool    fk_valid;
    size_t  fk_lo, fk_hi;   /* byte range of the last scanned collection */
    size_t *fk_keys;        /* sorted open offsets of collections that are keys */
    int     fk_nkeys, fk_keys_cap;  /* fk_nkeys -1: an ambiguous quote, so
                                     * every collection "is a key" */
    size_t *fk_stack;       /* scan scratch: open-bracket offsets */
    int     fk_stack_cap;

    /* The eager parser works one document at a time, so memory follows the
     * largest document, not the stream. A fallback to it rewinds to a
     * checkpoint taken at the start of a recent document (see
     * inc_checkpoint); the events delivered since are parsed again and
     * skipped, a document at a time. */
    oyl_scanner *ckpt;             /* scanner state there, or NULL */
    oyl_token    ckpt_tok;
    bool         ckpt_have_tok;
    int          ckpt_delivered;   /* events delivered before it */
    uint64_t     ckpt_sig;         /* delivered_sig then */
    int          evt_base;         /* events before the eager chunk */
    int          skip_n;           /* events of it left to skip */
    uint64_t     skip_sig;         /* their expected signature */
    uint64_t     skip_got;         /* and what was skipped so far */
};

/* ── Error reporting ─────────────────────────────────────── */

#define PARSE_ERROR(p, msg) do { \
    snprintf((p)->error_msg, sizeof((p)->error_msg), "%s", (msg)); \
    (p)->error_mark = (p)->current.start; \
    return OYL_ERR_PARSE; \
} while(0)

/* ── Helpers ─────────────────────────────────────────────── */

/* Zeroed event of the given type. A compound literal rather than a
 * function: events are ~112 bytes, and GCC does not reliably inline a
 * by-value helper, which costs a call plus store-forwarding stalls when the
 * caller's field writes are then copied out with wide loads. */
#define evt_simple(t) ((oyl_event){ .type = (t) })

/* Stop parsing from a helper that cannot return a status: sets oom, which
 * every loop checks, and records the status OOM_STATUS will report. */
static void stop_with(oyl_parser *p, oyl_status status, const char *msg) {
    if (!p->stop_status) {
        snprintf(p->error_msg, sizeof(p->error_msg), "%s", msg);
        p->error_mark = p->current.start;
        p->stop_status = status;
    }
    p->oom = true;
}

/* A safety limit (events, depth, alias expansion) was exceeded. */
static void hit_limit(oyl_parser *p, const char *msg) {
    stop_with(p, OYL_ERR_LIMIT, msg);
}

#define OOM_STATUS(p) ((p)->stop_status ? (p)->stop_status : OYL_ERR_MEMORY)

static inline bool over_limit(oyl_parser *p) {
    return p->max_events > 0 && p->evt_base + p->evt_len >= p->max_events;
}

static inline bool enqueue(oyl_parser *p, const oyl_event *evt) {
    if (over_limit(p)) { hit_limit(p, "event limit exceeded"); return false; }
    if (p->evt_len >= p->evt_cap) {
        int new_cap = p->evt_cap * 2;
        if (new_cap < 64) new_cap = 64;
        oyl_event *new_evts = realloc(p->events, new_cap * sizeof(oyl_event));
        if (!new_evts) { p->oom = true; return false; }
        p->events = new_evts;
        p->evt_cap = new_cap;
    }
    oyl_event *dst = &p->events[p->evt_len++];
    *dst = *evt;
    /* resolve tag via schema if set and no explicit tag */
    if (p->schema && dst->tag.data == NULL) {
        if (dst->type == OYL_EVT_SCALAR)
            dst->tag = oyl_schema_resolve(p->schema, dst->value, dst->scalar_style);
        else if (dst->type == OYL_EVT_MAPPING_START)
            dst->tag = p->schema->default_map_tag;
        else if (dst->type == OYL_EVT_SEQUENCE_START)
            dst->tag = p->schema->default_seq_tag;
    }
    return true;
}

/* Insert an event at index `idx` of the eager event list (e.g. a
 * MAPPING_START before a key that turned out to start an implicit
 * mapping). Like enqueue, applies the schema's default tags. */
static bool insert_event(oyl_parser *p, int idx, const oyl_event *evt) {
    if (!enqueue(p, evt)) return false;
    oyl_event tagged = p->events[p->evt_len - 1];
    memmove(&p->events[idx + 1], &p->events[idx],
            (size_t)(p->evt_len - 1 - idx) * sizeof(oyl_event));
    p->events[idx] = tagged;
    return true;
}

static inline bool dequeue(oyl_parser *p, oyl_event *evt) {
    if (p->evt_cursor >= p->evt_len) return false;
    *evt = p->events[p->evt_cursor++];
    return true;
}

static inline bool push_ctx(oyl_parser *p, ctx_type type, int indent) {
    if ((type == CTX_BLOCK_MAP || type == CTX_BLOCK_SEQ) && p->doc_start_line &&
        p->current.start.line == p->doc_start_line) {
        stop_with(p, OYL_ERR_PARSE, "block collection cannot start on the '---' line");
        return false;
    }
    if (p->max_depth > 0 && p->ctx_len >= p->max_depth) {
        hit_limit(p, "nesting depth limit exceeded");
        return false;
    }
    if (p->ctx_len >= p->ctx_cap) {
        int new_cap = p->ctx_cap * 2;
        ctx_entry *new_data = realloc(p->contexts, new_cap * sizeof(ctx_entry));
        if (!new_data) { p->oom = true; return false; }
        p->contexts = new_data;
        p->ctx_cap = new_cap;
    }
    p->contexts[p->ctx_len++] = (ctx_entry){type, indent};
    return true;
}

static inline void pop_ctx(oyl_parser *p) {
    if (p->ctx_len > 0) p->ctx_len--;
}

static inline ctx_entry *top_ctx(oyl_parser *p) {
    return p->ctx_len > 0 ? &p->contexts[p->ctx_len - 1] : NULL;
}

/* ── Token access ────────────────────────────────────────── */

static inline oyl_status peek_token(oyl_parser *p) {
    if (p->have_token) return OYL_OK;
    oyl_status st = oyl_scan_token(p->scanner, &p->current);
    if (st != OYL_OK) {
        const char *smsg = oyl_scanner_error(p->scanner);
        if (smsg) {
            snprintf(p->error_msg, sizeof(p->error_msg), "%s", smsg);
            p->error_mark = oyl_scanner_error_mark(p->scanner);
        }
        p->scan_error = st; /* save for incremental path error detection */
        p->scan_error_out = p->out_len;
        return st;
    }
    p->have_token = true;
    return OYL_OK;
}

static inline void consume_token(oyl_parser *p) {
    p->have_token = false;
}

static inline oyl_token_type tok_type(oyl_parser *p) {
    return p->current.type;
}

/* Token column (0-based) */
static inline int tok_col(oyl_parser *p) {
    return (int)p->current.start.col - 1;
}

/* ── Context helpers ─────────────────────────────────────── */

/* Inside a flow collection? The incremental parser tracks nesting in its
 * frame stack, the eager parser in its context stack. */
static inline bool in_flow(oyl_parser *p) {
    if (p->incremental) {
        if (p->frame_len == 0) return false;
        ctx_type t = p->frames[p->frame_len - 1].type;
        return t == CTX_FLOW_MAP || t == CTX_FLOW_SEQ;
    }
    ctx_entry *top = top_ctx(p);
    return top && (top->type == CTX_FLOW_MAP || top->type == CTX_FLOW_SEQ);
}

/* Close all block contexts */
static void unroll_all(oyl_parser *p, oyl_mark mark) {
    while (p->ctx_len > 0) {
        ctx_entry *top = top_ctx(p);
        if (top->type == CTX_FLOW_MAP || top->type == CTX_FLOW_SEQ) break;
        oyl_event_type et = (top->type == CTX_BLOCK_MAP)
            ? OYL_EVT_MAPPING_END : OYL_EVT_SEQUENCE_END;
        oyl_event evt = evt_simple(et);
        evt.start = mark;
        evt.end = mark;
        enqueue(p, &evt);
        pop_ctx(p);
    }
}

/* ── Attach pending anchor/tag ───────────────────────────── */

static inline void attach_props(oyl_parser *p, oyl_event *evt) {
    bool had_props = p->has_anchor || p->has_tag;
    if (p->has_anchor) {
        evt->anchor = p->pending_anchor;
        p->has_anchor = false;
        p->pending_anchor = OYL_STR_NULL;
    }
    if (p->has_tag) {
        evt->tag = p->pending_tag;
        p->has_tag = false;
        p->pending_tag = OYL_STR_NULL;
    }
    if (had_props) {
        evt->start = p->props_start;
    }
}

/* A flow collection at events[at] turned out to be an implicit key, and a
 * mapping start is being inserted before it. Properties that were on an
 * earlier line than the collection belong to that mapping, as they would
 * for a scalar key ("&m\n[a]: b" anchors the mapping). `anchor` and `tag`
 * are those properties as written: the event's tag may instead be the
 * schema's default, which stays with the collection. */
static void move_props_to_key_map(oyl_parser *p, int at, oyl_event *map_evt,
                                  oyl_mark coll_start, oyl_str anchor, oyl_str tag) {
    oyl_event *coll = &p->events[at];
    map_evt->anchor = anchor;
    map_evt->tag = tag;         /* insert_event applies the schema default */
    coll->anchor = OYL_STR_NULL;
    coll->tag = OYL_STR_NULL;
    if (p->schema)
        coll->tag = coll->type == OYL_EVT_MAPPING_START ? p->schema->default_map_tag
                                                        : p->schema->default_seq_tag;
    coll->start = coll_start;
}

/* ── Emit empty scalar ───────────────────────────────────── */

static void emit_empty(oyl_parser *p) {
    oyl_event evt = evt_simple(OYL_EVT_SCALAR);
    evt.scalar_style = OYL_SCALAR_PLAIN;
    evt.start = p->current.start;
    evt.end = p->current.start;
    attach_props(p, &evt);
    enqueue(p, &evt);
}

/* ── Ensure doc is open ──────────────────────────────────── */

static void ensure_doc(oyl_parser *p, bool explicit, oyl_mark mark) {
    if (!p->doc_open) {
        oyl_event evt = evt_simple(OYL_EVT_DOC_START);
        evt.implicit = !explicit;
        evt.start = mark;
        evt.end = mark;
        enqueue(p, &evt);
        p->doc_open = true;
    }
}

/* ── Consume anchor/tag properties ───────────────────────── */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* copy src to dst, decoding %XX percent-encoded bytes; returns decoded length */
static size_t pct_decode(char *dst, const char *src, size_t len) {
    size_t di = 0;
    for (size_t si = 0; si < len; ) {
        if (src[si] == '%' && si + 2 < len) {
            int hi = hex_val(src[si + 1]);
            int lo = hex_val(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 3;
                continue;
            }
        }
        dst[di++] = src[si++];
    }
    return di;
}

static oyl_str expand_tag(oyl_parser *p, oyl_str raw) {
    if (raw.len < 1 || raw.data[0] != '!') return raw;

    /* verbatim tag: !<...> → strip !< and > */
    if (raw.len >= 3 && raw.data[1] == '<' && raw.data[raw.len - 1] == '>') {
        size_t inner_len = raw.len - 3; /* strip !< and > */
        char *buf = oyl_arena_alloc(p->arena, inner_len + 1, 1);
        if (!buf) return raw;
        size_t dlen = pct_decode(buf, raw.data + 2, inner_len);
        buf[dlen] = '\0';
        return (oyl_str){buf, dlen};
    }

    /* find the tag handle: !!, !x!, or ! */
    size_t handle_len = 1; /* at least "!" */
    if (raw.len >= 2 && raw.data[1] == '!') {
        /* !! secondary handle */
        handle_len = 2;
    } else if (raw.len >= 3) {
        /* !x...! named handle: find second '!' */
        for (size_t i = 1; i < raw.len; i++) {
            if (raw.data[i] == '!') {
                handle_len = i + 1;
                break;
            }
        }
    }

    /* look up the handle in tag directives */
    for (int i = 0; i < p->tag_dir_count; i++) {
        size_t hlen = strlen(p->tag_directives[i].handle);
        if (hlen == handle_len && memcmp(raw.data, p->tag_directives[i].handle, hlen) == 0) {
            const char *prefix = p->tag_directives[i].prefix;
            size_t plen = strlen(prefix);
            size_t suffix_len = raw.len - handle_len;
            size_t total = plen + suffix_len; /* max size before decoding */
            char *buf = oyl_arena_alloc(p->arena, total + 1, 1);
            if (!buf) return raw;
            memcpy(buf, prefix, plen);
            size_t dlen = pct_decode(buf + plen, raw.data + handle_len, suffix_len);
            buf[plen + dlen] = '\0';
            return (oyl_str){buf, plen + dlen};
        }
    }

    /* default: !! → tag:yaml.org,2002: */
    if (raw.len >= 2 && raw.data[1] == '!') {
        const char *prefix = "tag:yaml.org,2002:";
        size_t plen = 18;
        size_t suffix_len = raw.len - 2;
        size_t total = plen + suffix_len;
        char *buf = oyl_arena_alloc(p->arena, total + 1, 1);
        if (!buf) return raw;
        memcpy(buf, prefix, plen);
        size_t dlen = pct_decode(buf + plen, raw.data + 2, suffix_len);
        buf[plen + dlen] = '\0';
        return (oyl_str){buf, plen + dlen};
    }

    return raw;
}

/* A named handle (!name!suffix) must be declared by a %TAG directive of
 * the current document; ! and !! are always available. */
static bool tag_handle_declared(const oyl_parser *p, oyl_str raw) {
    if (raw.len < 3 || raw.data[0] != '!' || raw.data[1] == '!' || raw.data[1] == '<')
        return true;
    size_t hlen = 0;
    for (size_t i = 1; i < raw.len; i++) {
        if (raw.data[i] == '!') { hlen = i + 1; break; }
    }
    if (hlen == 0) return true; /* local tag: !suffix */
    for (int i = 0; i < p->tag_dir_count; i++) {
        if (strlen(p->tag_directives[i].handle) == hlen &&
            memcmp(p->tag_directives[i].handle, raw.data, hlen) == 0)
            return true;
    }
    return false;
}

/* A node's second property on a new line must be indented more than the
 * enclosing block collection ("key: &x\n!!map" is invalid). */
static bool props_continuation_ok(oyl_parser *p) {
    if (!(p->has_anchor || p->has_tag)) return true;
    if (p->current.start.line == p->props_line || in_flow(p)) return true;
    ctx_entry *top = top_ctx(p);
    int parent = top ? top->indent : -1;
    return tok_col(p) > parent;
}

/* Does the current property token have content after it on its line?
 * Then it starts the props of that content (a key), not of the node
 * whose props began on an earlier line. */
static bool prop_starts_node_line(oyl_parser *p) {
    size_t i = p->current.end.offset;
    while (i < p->input_len && (p->input[i] == ' ' || p->input[i] == '\t')) i++;
    return i < p->input_len && p->input[i] != '\n' && p->input[i] != '\r' &&
           p->input[i] != '#';
}

static oyl_status consume_props(oyl_parser *p) {
    /* Consume at most one anchor and one tag per node */
    for (;;) {
        oyl_status st = peek_token(p);
        if (st != OYL_OK) return st;
        if ((tok_type(p) == OYL_TOK_ANCHOR && !p->has_anchor) ||
            (tok_type(p) == OYL_TOK_TAG && !p->has_tag)) {
            /* a property on a new line not indented past the enclosing
             * collection starts its next entry, not a continuation */
            if (!props_continuation_ok(p)) break;
        }
        if (tok_type(p) == OYL_TOK_ANCHOR && !p->has_anchor) {
            /* In block context, a tag on a previous line and an anchor on
             * a new line followed by content: stop — the tag is for the
             * collection and the anchor starts props for the first key.
             * An anchor alone on its line still belongs to the collection. */
            if (p->has_tag && p->current.start.line != p->props_line &&
                !in_flow(p) && prop_starts_node_line(p)) break;
            if (!p->has_tag) {
                p->props_line = p->current.start.line;
                p->props_col = tok_col(p);
                p->props_start = p->current.start;
            }
            p->pending_anchor = p->current.value;
            p->has_anchor = true;
            consume_token(p);
        } else if (tok_type(p) == OYL_TOK_TAG && !p->has_tag) {
            /* likewise an anchor on a previous line and a tag on a new one */
            if (p->has_anchor && p->current.start.line != p->props_line &&
                !in_flow(p) && prop_starts_node_line(p)) break;
            if (!p->has_anchor) {
                p->props_line = p->current.start.line;
                p->props_col = tok_col(p);
                p->props_start = p->current.start;
            }
            if (!tag_handle_declared(p, p->current.value))
                PARSE_ERROR(p, "undefined tag handle");
            p->pending_tag = expand_tag(p, p->current.value);
            p->has_tag = true;
            consume_token(p);
        } else {
            break;
        }
    }
    /* properties directly before an alias would apply to it (on a later
     * line they belong to a block collection whose first key is the alias) */
    if ((p->has_anchor || p->has_tag) && tok_type(p) == OYL_TOK_ALIAS &&
        (p->current.start.line == p->props_line || in_flow(p)))
        PARSE_ERROR(p, "an alias cannot have an anchor or tag");
    return OYL_OK;
}

/* ── Forward declarations ────────────────────────────────── */

static oyl_status parse_block_node(oyl_parser *p);
static oyl_status parse_flow_node(oyl_parser *p);
static oyl_status parse_flow_sequence(oyl_parser *p);
static oyl_status parse_flow_mapping(oyl_parser *p);
static oyl_status parse_block_sequence(oyl_parser *p, int seq_indent);
static oyl_status parse_block_mapping(oyl_parser *p, int map_indent);
static oyl_status parse_block_map_value(oyl_parser *p, int map_indent);

/* ── Parse flow sequence contents ────────────────────────── */

static oyl_status parse_flow_sequence(oyl_parser *p) {
    oyl_status st;

    for (;;) {
        if (p->oom) return OOM_STATUS(p);
        if (over_limit(p)) { hit_limit(p, "event limit exceeded"); return OYL_ERR_LIMIT; }
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_FLOW_SEQ_END) {
            oyl_mark close = p->current.start;
            oyl_mark close_end = p->current.end;
            consume_token(p);
            oyl_event end_evt = evt_simple(OYL_EVT_SEQUENCE_END);
            end_evt.start = close;
            end_evt.end = close_end;
            enqueue(p, &end_evt);
            pop_ctx(p);
            return OYL_OK;
        }

        /* comma with no entry before it: [ , a ] or [ a, , b ] */
        if (tok_type(p) == OYL_TOK_FLOW_ENTRY)
            PARSE_ERROR(p, "unexpected ',' in flow sequence");

        /* check for explicit key ? — starts a flow pair */
        if (tok_type(p) == OYL_TOK_BLOCK_MAP_KEY) {
            /* explicit key in flow sequence → flow pair (implicit mapping) */
            oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
            map_evt.flow = true;
            map_evt.start = p->current.start;
            map_evt.end = p->current.end;
            enqueue(p, &map_evt);

            consume_token(p);  /* consume ? */
            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE ||
                tok_type(p) == OYL_TOK_FLOW_ENTRY ||
                tok_type(p) == OYL_TOK_FLOW_SEQ_END) {
                emit_empty(p);  /* empty key */
            } else {
                st = parse_flow_node(p);
                if (st != OYL_OK) return st;
            }

            /* parse value */
            st = peek_token(p);
            if (st != OYL_OK) return st;
            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
                consume_token(p);
                st = peek_token(p);
                if (st != OYL_OK) return st;
                if (tok_type(p) == OYL_TOK_FLOW_ENTRY ||
                    tok_type(p) == OYL_TOK_FLOW_SEQ_END) {
                    emit_empty(p);
                } else {
                    st = parse_flow_node(p);
                    if (st != OYL_OK) return st;
                }
            } else {
                emit_empty(p);
            }

            {
                /* zero width where the next token starts (after a flow
                 * value, the current token is still its closing bracket) */
                st = peek_token(p);
                if (st != OYL_OK) return st;
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            goto check_flow_sep;
        }

        /* Save position before parsing entry (for implicit key wrapping) */
        int pre_key_len = p->evt_len;

        /* parse entry */
        st = parse_flow_node(p);
        if (st != OYL_OK) return st;

        /* check for implicit flow pair: node followed by : */
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
            /* Insert MAPPING_START before all key events */
            int key_idx = pre_key_len;
            int num_key_events = p->evt_len - key_idx;
            if (num_key_events > 0) {
                /* grow array by 1 */
                oyl_event placeholder = evt_simple(OYL_EVT_MAPPING_START);
                placeholder.flow = true;
                placeholder.start = p->events[key_idx].start;
                placeholder.end = p->events[key_idx].start;
                if (!insert_event(p, key_idx, &placeholder)) return OOM_STATUS(p);
            }

            consume_token(p); /* consume : */
            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (tok_type(p) == OYL_TOK_FLOW_ENTRY ||
                tok_type(p) == OYL_TOK_FLOW_SEQ_END) {
                emit_empty(p);
            } else {
                st = parse_flow_node(p);
                if (st != OYL_OK) return st;
            }

            {
                /* zero width where the next token starts (after a flow
                 * value, the current token is still its closing bracket) */
                st = peek_token(p);
                if (st != OYL_OK) return st;
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
        }

check_flow_sep:
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_FLOW_ENTRY) {
            consume_token(p);
            continue;
        }

        if (tok_type(p) == OYL_TOK_FLOW_SEQ_END) {
            continue; /* will be handled at top of loop */
        }

        PARSE_ERROR(p, "expected ',' or ']' in flow sequence");
    }
}

/* ── Parse flow mapping contents ─────────────────────────── */

static oyl_status parse_flow_mapping(oyl_parser *p) {
    oyl_status st;

    for (;;) {
        if (p->oom) return OOM_STATUS(p);
        if (over_limit(p)) { hit_limit(p, "event limit exceeded"); return OYL_ERR_LIMIT; }
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_FLOW_MAP_END) {
            oyl_mark close = p->current.start;
            oyl_mark close_end = p->current.end;
            consume_token(p);
            oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
            end_evt.start = close;
            end_evt.end = close_end;
            enqueue(p, &end_evt);
            pop_ctx(p);
            return OYL_OK;
        }

        /* comma with no entry before it: { , a: b } */
        if (tok_type(p) == OYL_TOK_FLOW_ENTRY)
            PARSE_ERROR(p, "unexpected ',' in flow mapping");

        /* parse key */
        if (tok_type(p) == OYL_TOK_BLOCK_MAP_KEY) {
            consume_token(p);
            st = peek_token(p);
            if (st != OYL_OK) return st;
            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE ||
                tok_type(p) == OYL_TOK_FLOW_ENTRY ||
                tok_type(p) == OYL_TOK_FLOW_MAP_END) {
                emit_empty(p);
            } else {
                st = parse_flow_node(p);
                if (st != OYL_OK) return st;
            }
        } else {
            st = parse_flow_node(p);
            if (st != OYL_OK) return st;
        }

        /* parse value */
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
            consume_token(p);
            st = peek_token(p);
            if (st != OYL_OK) return st;
            if (tok_type(p) == OYL_TOK_FLOW_ENTRY ||
                tok_type(p) == OYL_TOK_FLOW_MAP_END) {
                emit_empty(p);
            } else {
                st = parse_flow_node(p);
                if (st != OYL_OK) return st;
            }
        } else {
            emit_empty(p);
        }

        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_FLOW_ENTRY) {
            consume_token(p);
            continue;
        }

        if (tok_type(p) == OYL_TOK_FLOW_MAP_END) {
            continue;
        }

        PARSE_ERROR(p, "expected ',' or '}' in flow mapping");
    }
}

/* ── Parse a node in flow context ────────────────────────── */

static oyl_status parse_flow_node(oyl_parser *p) {
    if (p->oom) return OOM_STATUS(p);
    oyl_status st = consume_props(p);
    if (st != OYL_OK) return st;

    st = peek_token(p);
    if (st != OYL_OK) return st;

    oyl_token_type tt = tok_type(p);

    if (tt == OYL_TOK_ALIAS) {
        oyl_event evt = evt_simple(OYL_EVT_ALIAS);
        evt.value = p->current.value;
        evt.start = p->current.start;
        evt.end = p->current.end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        consume_token(p);
        return OYL_OK;
    }

    if (tt == OYL_TOK_FLOW_SEQ_START) {
        int col = tok_col(p);
        oyl_mark open_start = p->current.start;
        oyl_mark open_end = p->current.end;
        consume_token(p);
        oyl_event evt = evt_simple(OYL_EVT_SEQUENCE_START);
        evt.flow = true;
        evt.start = open_start;
        evt.end = open_end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_SEQ, col);
        return parse_flow_sequence(p);
    }

    if (tt == OYL_TOK_FLOW_MAP_START) {
        int col = tok_col(p);
        oyl_mark open_start = p->current.start;
        oyl_mark open_end = p->current.end;
        consume_token(p);
        oyl_event evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = open_start;
        evt.end = open_end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_MAP, col);
        return parse_flow_mapping(p);
    }

    if (tt == OYL_TOK_SCALAR) {
        oyl_event evt = evt_simple(OYL_EVT_SCALAR);
        evt.value = p->current.value;
        evt.scalar_style = p->current.scalar_style;
        evt.start = p->current.start;
        evt.end = p->current.end;
        attach_props(p, &evt);

        consume_token(p);

        /* check for implicit key in flow context */
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
            /* this scalar is a key in an implicit flow mapping */
            /* For now, just emit the scalar; the caller handles key-value */
        }

        enqueue(p, &evt);
        return OYL_OK;
    }

    /* empty scalar */
    emit_empty(p);
    return OYL_OK;
}

/* ── Parse block map value ───────────────────────────────── */

static oyl_status parse_block_map_value(oyl_parser *p, int map_indent) {
    if (p->oom) return OOM_STATUS(p);
    oyl_status st = peek_token(p);
    if (st != OYL_OK) return st;

    if (tok_type(p) != OYL_TOK_BLOCK_MAP_VALUE) {
        emit_empty(p);
        return OYL_OK;
    }

    consume_token(p); /* consume : */

    st = peek_token(p);
    if (st != OYL_OK) return st;

    oyl_token_type tt = tok_type(p);
    int col = tok_col(p);

    /* empty value cases */
    if (tt == OYL_TOK_STREAM_END || tt == OYL_TOK_DOC_START ||
        tt == OYL_TOK_DOC_END) {
        emit_empty(p);
        return OYL_OK;
    }

    /* Anything left of the mapping's indentation ends it, and anything at
     * its indentation is the next entry: the value is empty. The one
     * exception is a block sequence, which may sit at the key's
     * indentation ("k:\n- a"). */
    if (col < map_indent ||
        (col == map_indent && tt != OYL_TOK_BLOCK_SEQ_ENTRY)) {
        emit_empty(p);
        return OYL_OK;
    }

    /* parse value as block node */
    return parse_block_node(p);
}

/* ── Parse block mapping ─────────────────────────────────── */

static oyl_status parse_block_mapping(oyl_parser *p, int map_indent) {
    oyl_status st;

    for (;;) {
        if (p->oom) return OOM_STATUS(p);
        if (over_limit(p)) { hit_limit(p, "event limit exceeded"); return OYL_ERR_LIMIT; }
        st = peek_token(p);
        if (st != OYL_OK) return st;

        int col = tok_col(p);
        oyl_token_type tt = tok_type(p);

        /* explicit key ? */
        if (tt == OYL_TOK_BLOCK_MAP_KEY && col == map_indent) {
            consume_token(p);

            st = peek_token(p);
            if (st != OYL_OK) return st;

            /* nothing indented past the '?' (e.g. ':' or another '?' at
             * the mapping's indentation, or the end of the document):
             * the key is empty. A block sequence may sit at the mapping's
             * indentation ("?\n- a"). */
            if (tok_col(p) < map_indent ||
                (tok_col(p) == map_indent && tok_type(p) != OYL_TOK_BLOCK_SEQ_ENTRY) ||
                tok_type(p) == OYL_TOK_DOC_START || tok_type(p) == OYL_TOK_DOC_END ||
                tok_type(p) == OYL_TOK_STREAM_END) {
                emit_empty(p);
            } else if (tok_type(p) == OYL_TOK_ANCHOR ||
                       tok_type(p) == OYL_TOK_TAG) {
                /* Consume props, then check if ':' follows at map indent.
                 * If so, the props belong to an empty key scalar, not a nested node. */
                st = consume_props(p);
                if (st != OYL_OK) return st;
                st = peek_token(p);
                if (st != OYL_OK) return st;
                if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE &&
                    tok_col(p) == map_indent) {
                    emit_empty(p);
                } else {
                    st = parse_block_node(p);
                    if (st != OYL_OK) return st;
                }
            } else {
                st = parse_block_node(p);
                if (st != OYL_OK) return st;
            }

            /* parse value: an explicit value's ':' is at the mapping's
             * indentation. One further left belongs to an enclosing
             * mapping ("?\n  ? x\n: y"): this key's value is empty. */
            st = peek_token(p);
            if (st != OYL_OK) return st;
            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE && tok_col(p) > map_indent)
                PARSE_ERROR(p, "explicit mapping value must be at the mapping's indentation");
            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE && tok_col(p) < map_indent) {
                emit_empty(p);
                continue;
            }
            st = parse_block_map_value(p, map_indent);
            if (st != OYL_OK) return st;
            continue;
        }

        /* empty key + value (: at map indent) */
        if (tt == OYL_TOK_BLOCK_MAP_VALUE && col == map_indent) {
            emit_empty(p); /* empty key */
            st = parse_block_map_value(p, map_indent);
            if (st != OYL_OK) return st;
            continue;
        }

        /* simple key (scalar at map indent) */
        if (col != map_indent) break;

        /* tokens that can't be keys */
        if (tt == OYL_TOK_STREAM_END || tt == OYL_TOK_DOC_START ||
            tt == OYL_TOK_DOC_END || tt == OYL_TOK_BLOCK_SEQ_ENTRY) {
            break;
        }

        /* consume properties that might be on the key */
        int key_start_col = col; /* remember where the key (or its props) started */
        st = consume_props(p);
        if (st != OYL_OK) return st;

        st = peek_token(p);
        if (st != OYL_OK) return st;
        tt = tok_type(p);
        col = tok_col(p);

        /* an implicit key, props included, is on one line */
        if ((p->has_anchor || p->has_tag) && p->current.start.line != p->props_line)
            PARSE_ERROR(p, "mapping key properties must be on the key's line");

        /* key matches map indent if either the props or scalar is at map_indent */
        bool at_map_indent = (col == map_indent) || (key_start_col == map_indent);

        /* props on an empty key ("!!null : v") */
        if (tt == OYL_TOK_BLOCK_MAP_VALUE && key_start_col == map_indent &&
            (p->has_anchor || p->has_tag)) {
            emit_empty(p);
            st = parse_block_map_value(p, map_indent);
            if (st != OYL_OK) return st;
            continue;
        }
        if (tt == OYL_TOK_SCALAR && at_map_indent) {
            oyl_event evt = evt_simple(OYL_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);

            /* peek for : */
            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
                /* confirmed key */
                enqueue(p, &evt);
                st = parse_block_map_value(p, map_indent);
                if (st != OYL_OK) return st;
                continue;
            }

            /* a scalar at the mapping's indentation must be a key */
            PARSE_ERROR(p, "could not find expected ':' after mapping key");
        }

        /* alias as key */
        if (tt == OYL_TOK_ALIAS && col == map_indent) {
            oyl_event evt = evt_simple(OYL_EVT_ALIAS);
            evt.value = p->current.value;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);

            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
                enqueue(p, &evt);
                st = parse_block_map_value(p, map_indent);
                if (st != OYL_OK) return st;
                continue;
            }
            PARSE_ERROR(p, "could not find expected ':' after mapping key");
        }

        /* flow collection as key: parse just the collection (a block node
         * would treat "[...]:" as the start of a new, nested mapping) */
        if ((tt == OYL_TOK_FLOW_SEQ_START || tt == OYL_TOK_FLOW_MAP_START) &&
            at_map_indent) {
            st = parse_flow_node(p);
            if (st != OYL_OK) return st;

            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
                st = parse_block_map_value(p, map_indent);
                if (st != OYL_OK) return st;
                continue;
            }
            PARSE_ERROR(p, "could not find expected ':' after mapping key");
        }

        break;
    }

    return OYL_OK;
}

/* ── Parse block sequence ────────────────────────────────── */

static oyl_status parse_block_sequence(oyl_parser *p, int seq_indent) {
    oyl_status st;

    for (;;) {
        if (p->oom) return OOM_STATUS(p);
        if (over_limit(p)) { hit_limit(p, "event limit exceeded"); return OYL_ERR_LIMIT; }
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) != OYL_TOK_BLOCK_SEQ_ENTRY || tok_col(p) != seq_indent)
            break;

        consume_token(p); /* consume - */

        st = peek_token(p);
        if (st != OYL_OK) return st;

        oyl_token_type tt = tok_type(p);

        /* empty entry: an entry's content is indented past its '-', so
         * anything at or left of the '-' (the next '-', a ':' of an
         * enclosing explicit entry, ...) comes after it */
        if (tok_col(p) <= seq_indent) {
            emit_empty(p);
            continue;
        }
        if (tt == OYL_TOK_STREAM_END || tt == OYL_TOK_DOC_START ||
            tt == OYL_TOK_DOC_END) {
            emit_empty(p);
            break;
        }

        /* parse entry content */
        st = parse_block_node(p);
        if (st != OYL_OK) return st;
    }

    return OYL_OK;
}

/* Is the current token an implicit key's ':'? It must be on the line where
 * the key ends: a ':' starting a later line belongs to an explicit "?"
 * entry or an enclosing mapping. */
static inline bool key_colon_follows(oyl_parser *p, size_t key_end_line) {
    return tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE && !in_flow(p) &&
           p->current.start.line == key_end_line;
}

/* ── Parse block node (may be scalar, collection, alias, flow) ── */

static oyl_status parse_block_node(oyl_parser *p) {
    if (p->oom) return OOM_STATUS(p);
    oyl_status st = consume_props(p);
    if (st != OYL_OK) return st;

    st = peek_token(p);
    if (st != OYL_OK) return st;

    oyl_token_type tt = tok_type(p);
    int col = tok_col(p);

    /* a block sequence must start on a new line after its properties */
    if (tt == OYL_TOK_BLOCK_SEQ_ENTRY && (p->has_anchor || p->has_tag) &&
        p->current.start.line == p->props_line)
        PARSE_ERROR(p, "block sequence cannot start on the same line as its properties");

    /* A new line that is not indented past the enclosing block collection
     * starts its next entry ("k: !!null\n!!str x: y", "? a: &x\n: b"):
     * our props belong to an empty node. A block sequence may still sit
     * at a mapping's indentation ("k: &a\n- x" anchors the sequence). */
    if ((p->has_anchor || p->has_tag) && !in_flow(p) &&
        p->current.start.line != p->props_line) {
        ctx_entry *parent = top_ctx(p);
        if (parent && (col < parent->indent ||
                       (col == parent->indent &&
                        !(tt == OYL_TOK_BLOCK_SEQ_ENTRY && parent->type == CTX_BLOCK_MAP)))) {
            emit_empty(p);
            return OYL_OK;
        }
    }

    /* If next token is ANCHOR/TAG, these are props for an inner node.
     * Save our props (for the outer collection) and let the inner
     * node's props be consumed when we recurse into parsing. */
    if ((tt == OYL_TOK_ANCHOR || tt == OYL_TOK_TAG) &&
        (p->has_anchor || p->has_tag)) {
        /* Save outer node's props */
        oyl_str saved_anchor = p->pending_anchor;
        oyl_str saved_tag = p->pending_tag;
        bool had_anchor = p->has_anchor;
        bool had_tag = p->has_tag;
        oyl_mark saved_props_start = p->props_start;
        p->has_anchor = false;
        p->has_tag = false;
        p->pending_anchor = OYL_STR_NULL;
        p->pending_tag = OYL_STR_NULL;

        /* Consume inner node's props to peek at the real structure */
        int inner_col = tok_col(p);   /* where the inner node (a key?) starts */
        size_t inner_line = p->current.start.line;
        st = consume_props(p);
        if (st != OYL_OK) return st;

        /* Save inner props */
        oyl_str inner_anchor = p->pending_anchor;
        oyl_str inner_tag = p->pending_tag;
        bool inner_has_anchor = p->has_anchor;
        bool inner_has_tag = p->has_tag;

        /* Restore outer props for attach_props calls below */
        p->pending_anchor = saved_anchor;
        p->pending_tag = saved_tag;
        p->has_anchor = had_anchor;
        p->has_tag = had_tag;
        p->props_start = saved_props_start;

        st = peek_token(p);
        if (st != OYL_OK) return st;
        tt = tok_type(p);
        col = tok_col(p);

        /* Now handle the node type with inner props deferred */
        if (tt == OYL_TOK_SCALAR) {
            oyl_event evt = evt_simple(OYL_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
            /* Attach inner props to the scalar */
            if (inner_has_anchor) evt.anchor = inner_anchor;
            if (inner_has_tag) evt.tag = inner_tag;

            /* the key starts at its props when they share its line
             * ("&k a: b" is a key at the column of "&k") */
            int scalar_col = evt.start.line == inner_line ? inner_col : col;
            consume_token(p);

            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (key_colon_follows(p, evt.start.line)) {
                /* mapping — outer props go on +MAP */
                oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
                map_evt.start = evt.start;
                map_evt.end = evt.start;
                attach_props(p, &map_evt); /* outer anchor/tag */
                enqueue(p, &map_evt);
                push_ctx(p, CTX_BLOCK_MAP, scalar_col);
                enqueue(p, &evt); /* key scalar with inner anchor/tag */

                st = parse_block_map_value(p, scalar_col);
                if (st != OYL_OK) return st;
                st = parse_block_mapping(p, scalar_col);
                if (st != OYL_OK) return st;
                {
                    oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                    end_evt.start = p->current.start;
                    end_evt.end = p->current.start;
                    enqueue(p, &end_evt);
                }
                pop_ctx(p);
                return OYL_OK;
            }

            /* not a mapping: both sets of properties apply to the scalar,
             * which is fine unless they conflict (two anchors or two tags) */
            if ((inner_has_anchor && p->has_anchor) || (inner_has_tag && p->has_tag))
                PARSE_ERROR(p, "a node cannot have two anchors or two tags");
            attach_props(p, &evt);
            enqueue(p, &evt);
            return OYL_OK;
        }

        if (tt == OYL_TOK_BLOCK_SEQ_ENTRY) {
            oyl_event evt = evt_simple(OYL_EVT_SEQUENCE_START);
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            enqueue(p, &evt);
            push_ctx(p, CTX_BLOCK_SEQ, col);
            /* Restore inner props for the first entry's node */
            p->pending_anchor = inner_anchor;
            p->pending_tag = inner_tag;
            p->has_anchor = inner_has_anchor;
            p->has_tag = inner_has_tag;
            st = parse_block_sequence(p, col);
            if (st != OYL_OK) return st;
            {
                oyl_event end_evt = evt_simple(OYL_EVT_SEQUENCE_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return OYL_OK;
        }

        /* Empty key with its own props ("!!map\n!!null : v"): outer props
         * go on the mapping, inner props on the empty key */
        if (tt == OYL_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
            oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
            map_evt.start = p->current.start;
            map_evt.end = p->current.start;
            attach_props(p, &map_evt); /* outer anchor/tag */
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, inner_col);

            oyl_event key = evt_simple(OYL_EVT_SCALAR);
            key.start = p->current.start;
            key.end = p->current.start;
            if (inner_has_anchor) key.anchor = inner_anchor;
            if (inner_has_tag) key.tag = inner_tag;
            enqueue(p, &key);

            st = parse_block_map_value(p, inner_col);
            if (st != OYL_OK) return st;
            st = parse_block_mapping(p, inner_col);
            if (st != OYL_OK) return st;
            {
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return OYL_OK;
        }

        /* Flow collection with double props. If ':' follows, the collection
         * is the first key of an implicit block mapping that takes the
         * outer props; otherwise both sets of props are the collection's. */
        if (tt == OYL_TOK_FLOW_SEQ_START || tt == OYL_TOK_FLOW_MAP_START) {
            int key_col = p->current.start.line == inner_line ? inner_col : col;
            oyl_mark flow_open = p->current.start;
            oyl_mark flow_open_end = p->current.end;
            int coll_idx = p->evt_len;   /* where the collection starts */

            /* Parse the flow collection directly (not via parse_block_node,
             * to avoid double complex-key detection).
             * Clear pending props first so flow content can use consume_props. */
            p->has_anchor = false;
            p->has_tag = false;
            p->pending_anchor = OYL_STR_NULL;
            p->pending_tag = OYL_STR_NULL;

            consume_token(p);
            oyl_event cevt = evt_simple(tt == OYL_TOK_FLOW_SEQ_START ? OYL_EVT_SEQUENCE_START
                                                                    : OYL_EVT_MAPPING_START);
            cevt.flow = true;
            cevt.start = flow_open;
            cevt.end = flow_open_end;
            if (inner_has_anchor) cevt.anchor = inner_anchor;
            if (inner_has_tag) cevt.tag = inner_tag;
            enqueue(p, &cevt);
            if (tt == OYL_TOK_FLOW_SEQ_START) {
                push_ctx(p, CTX_FLOW_SEQ, col);
                st = parse_flow_sequence(p);
            } else {
                push_ctx(p, CTX_FLOW_MAP, col);
                st = parse_flow_mapping(p);
            }
            if (st != OYL_OK) return st;

            st = peek_token(p);
            if (st != OYL_OK) return st;

            if (tok_type(p) != OYL_TOK_BLOCK_MAP_VALUE) {
                /* not a key: both sets of props are the collection's */
                if ((inner_has_anchor && had_anchor) || (inner_has_tag && had_tag))
                    PARSE_ERROR(p, "a node cannot have two anchors or two tags");
                if (had_anchor) p->events[coll_idx].anchor = saved_anchor;
                if (had_tag) p->events[coll_idx].tag = saved_tag;
                p->has_anchor = false;
                p->has_tag = false;
                p->pending_anchor = OYL_STR_NULL;
                p->pending_tag = OYL_STR_NULL;
                return OYL_OK;
            }

            /* a key: the mapping (with the outer props) starts before it */
            oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
            map_evt.anchor = saved_anchor;
            map_evt.tag = saved_tag;
            map_evt.start = saved_props_start;
            map_evt.end = saved_props_start;
            if (!insert_event(p, coll_idx, &map_evt)) return OOM_STATUS(p);
            push_ctx(p, CTX_BLOCK_MAP, key_col);
            st = parse_block_map_value(p, key_col);
            if (st != OYL_OK) return st;
            st = parse_block_mapping(p, key_col);
            if (st != OYL_OK) return st;
            {
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return OYL_OK;
        }

        /* Restore inner props for other cases */
        p->pending_anchor = inner_anchor;
        p->pending_tag = inner_tag;
        p->has_anchor = inner_has_anchor;
        p->has_tag = inner_has_tag;
        /* fall through to normal handling (outer props already in pending) */
        /* Actually attach outer props first, then inner will be re-consumed */
        /* This is a complex edge case; emit empty with outer props */
        oyl_event empty = evt_simple(OYL_EVT_SCALAR);
        empty.value = OYL_STR_NULL;
        empty.start = p->current.start;
        empty.end = p->current.start;
        attach_props(p, &empty);
        enqueue(p, &empty);
        return OYL_OK;
    }

    /* alias */
    if (tt == OYL_TOK_ALIAS) {
        oyl_event evt = evt_simple(OYL_EVT_ALIAS);
        evt.value = p->current.value;
        evt.start = p->current.start;
        evt.end = p->current.end;
        int alias_col = col;
        consume_token(p);

        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (key_colon_follows(p, evt.start.line)) {
            /* alias is a key in a new block mapping — props go on mapping */
            oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
            map_evt.start = evt.start;
            map_evt.end = evt.start;
            attach_props(p, &map_evt);
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, alias_col);
            enqueue(p, &evt); /* alias as key (no props) */
            st = parse_block_map_value(p, alias_col);
            if (st != OYL_OK) return st;
            st = parse_block_mapping(p, alias_col);
            if (st != OYL_OK) return st;
            {
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
            return OYL_OK;
        }

        /* an alias node itself can't have an anchor or tag */
        if (p->has_anchor || p->has_tag)
            PARSE_ERROR(p, "an alias cannot have an anchor or tag");
        enqueue(p, &evt);
        return OYL_OK;
    }

    /* flow sequence */
    if (tt == OYL_TOK_FLOW_SEQ_START) {
        /* a key with props on its line starts at them ("&x [a]: b") */
        int flow_col = ((p->has_anchor || p->has_tag) &&
                        p->props_line == p->current.start.line) ? p->props_col : col;
        bool props_before_line = (p->has_anchor || p->has_tag) &&
                                 p->props_line != p->current.start.line;
        oyl_str own_anchor = p->has_anchor ? p->pending_anchor : OYL_STR_NULL;
        oyl_str own_tag = p->has_tag ? p->pending_tag : OYL_STR_NULL;
        /* Save queue position in case this is a complex key */
        int saved_evt_len = p->evt_len;

        oyl_mark open_start = p->current.start;
        oyl_mark open_end = p->current.end;
        consume_token(p);
        oyl_event evt = evt_simple(OYL_EVT_SEQUENCE_START);
        evt.flow = true;
        evt.start = open_start;
        evt.end = open_end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_SEQ, col);
        st = parse_flow_sequence(p);
        if (st != OYL_OK) return st;

        /* Check if this flow seq was a complex key (followed by ':').
         * Don't wrap if the ':' belongs to an existing parent block mapping. */
        st = peek_token(p);
        if (st != OYL_OK) return st;
        if (key_colon_follows(p, p->events[p->evt_len - 1].end.line)) {
            ctx_entry *fctx = top_ctx(p);
            bool colon_in_parent = fctx && fctx->type == CTX_BLOCK_MAP &&
                                   tok_col(p) == fctx->indent;
            if (colon_in_parent) goto flow_seq_done;
            /* Insert mapping start before the flow events */
            oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
            map_evt.start = p->events[saved_evt_len].start;
            map_evt.end = p->events[saved_evt_len].start;
            if (props_before_line)
                move_props_to_key_map(p, saved_evt_len, &map_evt, open_start,
                                      own_anchor, own_tag);
            if (!insert_event(p, saved_evt_len, &map_evt)) return OOM_STATUS(p);
            push_ctx(p, CTX_BLOCK_MAP, flow_col);
            st = parse_block_map_value(p, flow_col);
            if (st != OYL_OK) return st;
            st = parse_block_mapping(p, flow_col);
            if (st != OYL_OK) return st;
            {
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
        }
        flow_seq_done:
        return OYL_OK;
    }

    /* flow mapping */
    if (tt == OYL_TOK_FLOW_MAP_START) {
        /* a key with props on its line starts at them ("&x [a]: b") */
        int flow_col = ((p->has_anchor || p->has_tag) &&
                        p->props_line == p->current.start.line) ? p->props_col : col;
        bool props_before_line = (p->has_anchor || p->has_tag) &&
                                 p->props_line != p->current.start.line;
        oyl_str own_anchor = p->has_anchor ? p->pending_anchor : OYL_STR_NULL;
        oyl_str own_tag = p->has_tag ? p->pending_tag : OYL_STR_NULL;
        int saved_evt_len = p->evt_len;

        oyl_mark open_start2 = p->current.start;
        oyl_mark open_end2 = p->current.end;
        consume_token(p);
        oyl_event evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = open_start2;
        evt.end = open_end2;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_FLOW_MAP, col);
        st = parse_flow_mapping(p);
        if (st != OYL_OK) return st;

        /* Check if this flow map was a complex key (followed by ':') */
        st = peek_token(p);
        if (st != OYL_OK) return st;
        if (key_colon_follows(p, p->events[p->evt_len - 1].end.line)) {
            ctx_entry *fmctx = top_ctx(p);
            bool colon_in_parent2 = fmctx && fmctx->type == CTX_BLOCK_MAP &&
                                    tok_col(p) == fmctx->indent;
            if (colon_in_parent2) goto flow_map_done;
            oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
            map_evt.start = p->events[saved_evt_len].start;
            map_evt.end = p->events[saved_evt_len].start;
            if (props_before_line)
                move_props_to_key_map(p, saved_evt_len, &map_evt, open_start2,
                                      own_anchor, own_tag);
            if (!insert_event(p, saved_evt_len, &map_evt)) return OOM_STATUS(p);
            push_ctx(p, CTX_BLOCK_MAP, flow_col);
            st = parse_block_map_value(p, flow_col);
            if (st != OYL_OK) return st;
            st = parse_block_mapping(p, flow_col);
            if (st != OYL_OK) return st;
            {
                oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                end_evt.start = p->current.start;
                end_evt.end = p->current.start;
                enqueue(p, &end_evt);
            }
            pop_ctx(p);
        }
        flow_map_done:
        return OYL_OK;
    }

    /* If we have pending props (anchor/tag) and the next token indicates
     * there's no content for them (doc/stream end, or sibling in parent
     * collection), emit them on an empty scalar. */
    if ((p->has_anchor || p->has_tag) &&
        (tt == OYL_TOK_DOC_START || tt == OYL_TOK_DOC_END ||
         tt == OYL_TOK_STREAM_END)) {
        oyl_event empty_evt = evt_simple(OYL_EVT_SCALAR);
        empty_evt.value = OYL_STR_NULL;
        empty_evt.start = p->current.start;
        empty_evt.end = p->current.start;
        attach_props(p, &empty_evt);
        enqueue(p, &empty_evt);
        return OYL_OK;
    }
    if ((p->has_anchor || p->has_tag) && tt == OYL_TOK_BLOCK_SEQ_ENTRY) {
        ctx_entry *ctx = top_ctx(p);
        /* If '-' is below (strictly less than) the current context's indent,
         * the tag/anchor belongs to an empty scalar (the '-' is a parent).
         * At equal indent in a sequence, it's a sibling → empty scalar too. */
        bool is_empty = false;
        if (ctx && col < ctx->indent)
            is_empty = true;
        else if (ctx && ctx->type == CTX_BLOCK_SEQ && col == ctx->indent)
            is_empty = true;
        if (is_empty) {
            oyl_event empty_evt = evt_simple(OYL_EVT_SCALAR);
            empty_evt.value = OYL_STR_NULL;
            empty_evt.start = p->current.start;
            empty_evt.end = p->current.start;
            attach_props(p, &empty_evt);
            enqueue(p, &empty_evt);
            return OYL_OK;
        }
    }

    /* block sequence */
    if (tt == OYL_TOK_BLOCK_SEQ_ENTRY) {
        oyl_event evt = evt_simple(OYL_EVT_SEQUENCE_START);
        evt.start = p->current.start;
        evt.end = p->current.end;
        attach_props(p, &evt);
        enqueue(p, &evt);
        push_ctx(p, CTX_BLOCK_SEQ, col);
        st = parse_block_sequence(p, col);
        if (st != OYL_OK) return st;
        {
            oyl_event end_evt = evt_simple(OYL_EVT_SEQUENCE_END);
            end_evt.start = p->current.start;
            end_evt.end = p->current.start;
            enqueue(p, &end_evt);
        }
        pop_ctx(p);
        return OYL_OK;
    }

    /* scalar — check if it's at the same indent as parent mapping (sibling key) */
    if (tt == OYL_TOK_SCALAR) {
        ctx_entry *ctx = top_ctx(p);
        if (ctx && ctx->type == CTX_BLOCK_MAP && col <= ctx->indent) {
            /* This scalar is at the same or lower indent as the current mapping.
             * It belongs to the parent — emit empty value with any pending props. */
            oyl_event empty_evt = evt_simple(OYL_EVT_SCALAR);
            empty_evt.value = OYL_STR_NULL;
            empty_evt.start = p->current.start;
            empty_evt.end = p->current.start;
            attach_props(p, &empty_evt);
            enqueue(p, &empty_evt);
            return OYL_OK;
        }

        oyl_event evt = evt_simple(OYL_EVT_SCALAR);
        evt.value = p->current.value;
        evt.scalar_style = p->current.scalar_style;
        evt.start = p->current.start;
        evt.end = p->current.end;
        size_t scalar_line = p->current.start.line;

        int scalar_col = col;
        consume_token(p);

        /* peek for : → simple key (starts a nested mapping) */
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (key_colon_follows(p, scalar_line)) {
            int colon_col = tok_col(p);
            /* The effective key column considers props on the same line:
             * e.g., "!!str a:" has effective col = 0 (the tag col) */
            int eff_key_col = scalar_col;
            if ((p->has_anchor || p->has_tag) &&
                p->props_line == scalar_line &&
                p->props_col < scalar_col) {
                eff_key_col = p->props_col;
            }
            /* Check: the key must be at a deeper indent than the enclosing
             * block, AND the : must be deeper too. */
            ctx_entry *ctx = top_ctx(p);
            bool nested = !ctx ||
                          (eff_key_col > ctx->indent && colon_col > ctx->indent);

            if (nested) {
                /* this scalar is a key for a NEW mapping.
                 * If pending props are from a different line than the scalar,
                 * they belong on the mapping (e.g., !!map\n  key: val).
                 * If from the same line, they belong on the key scalar
                 * (e.g., &anchor key: val or !!str key: val). */
                oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
                bool props_on_map = (p->has_anchor || p->has_tag) &&
                                    p->props_line != scalar_line;

                /* mapping indent uses effective key col (considers props) */
                int map_col = eff_key_col;

                if (props_on_map) {
                    map_evt.start = evt.start;
                    map_evt.end = evt.start;
                    attach_props(p, &map_evt);
                    enqueue(p, &map_evt);
                    push_ctx(p, CTX_BLOCK_MAP, map_col);
                    enqueue(p, &evt); /* key scalar without props */
                } else {
                    map_evt.start = evt.start;
                    map_evt.end = evt.start;
                    enqueue(p, &map_evt);
                    push_ctx(p, CTX_BLOCK_MAP, map_col);
                    attach_props(p, &evt); /* props go on the key scalar */
                    enqueue(p, &evt);
                }

                st = parse_block_map_value(p, map_col);
                if (st != OYL_OK) return st;

                /* continue mapping for more keys at same indent */
                st = parse_block_mapping(p, map_col);
                if (st != OYL_OK) return st;

                {
                    oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
                    end_evt.start = p->current.start;
                    end_evt.end = p->current.start;
                    enqueue(p, &end_evt);
                }
                pop_ctx(p);
                return OYL_OK;
            }
        }

        /* just a scalar value — attach props now */
        attach_props(p, &evt);
        enqueue(p, &evt);
        return OYL_OK;
    }

    /* explicit key ? */
    if (tt == OYL_TOK_BLOCK_MAP_KEY) {
        oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
        map_evt.start = p->current.start;
        map_evt.end = p->current.end;
        attach_props(p, &map_evt);
        enqueue(p, &map_evt);
        push_ctx(p, CTX_BLOCK_MAP, col);

        st = parse_block_mapping(p, col);
        if (st != OYL_OK) return st;

        {
            oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
            end_evt.start = p->current.start;
            end_evt.end = p->current.start;
            enqueue(p, &end_evt);
        }
        pop_ctx(p);
        return OYL_OK;
    }

    /* bare : (empty key mapping) — tag/anchor belongs on the empty key */
    if (tt == OYL_TOK_BLOCK_MAP_VALUE && !in_flow(p)) {
        oyl_event map_evt = evt_simple(OYL_EVT_MAPPING_START);
        map_evt.start = p->current.start;
        map_evt.end = p->current.end;
        /* props on the colon's line belong on the empty key ("&k : v"),
         * and the mapping starts at them; props on an earlier line belong
         * on the mapping (handled by attach below) */
        if ((p->has_anchor || p->has_tag) && p->props_line == p->current.start.line) {
            int map_col = p->props_col;
            map_evt.start = p->props_start;
            map_evt.end = p->props_start;
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, map_col);
            /* emit empty key with the pending props */
            oyl_event key_evt = evt_simple(OYL_EVT_SCALAR);
            key_evt.value = OYL_STR_NULL;
            key_evt.start = p->current.start;
            key_evt.end = p->current.start;
            attach_props(p, &key_evt);
            enqueue(p, &key_evt);
            st = parse_block_map_value(p, map_col);
            if (st != OYL_OK) return st;
            st = parse_block_mapping(p, map_col);
        } else {
            attach_props(p, &map_evt);
            enqueue(p, &map_evt);
            push_ctx(p, CTX_BLOCK_MAP, col);
            st = parse_block_mapping(p, col);
        }
        if (st != OYL_OK) return st;

        {
            oyl_event end_evt = evt_simple(OYL_EVT_MAPPING_END);
            end_evt.start = p->current.start;
            end_evt.end = p->current.start;
            enqueue(p, &end_evt);
        }
        pop_ctx(p);
        return OYL_OK;
    }

    /* empty node / doc boundary */
    if (tt == OYL_TOK_STREAM_END || tt == OYL_TOK_DOC_START ||
        tt == OYL_TOK_DOC_END || tt == OYL_TOK_NONE) {
        emit_empty(p);
        return OYL_OK;
    }

    /* fallback: empty scalar */
    emit_empty(p);
    return OYL_OK;
}

/* ── Parse document ──────────────────────────────────────── */

/* After a document's root node only a document boundary may follow. */
static oyl_status expect_document_end(oyl_parser *p) {
    oyl_status st = peek_token(p);
    if (st != OYL_OK) return st;
    oyl_token_type tt = tok_type(p);
    if (tt != OYL_TOK_DOC_END && tt != OYL_TOK_DOC_START &&
        tt != OYL_TOK_STREAM_END && tt != OYL_TOK_NONE)
        PARSE_ERROR(p, "unexpected content after document root node");
    return OYL_OK;
}

/* Split the next whitespace-separated word of a directive line. Stops at
 * a comment ('#' after whitespace). Returns false when no word is left. */
static bool directive_word(const char **cur, const char *end,
                           const char **word, size_t *len) {
    const char *s = *cur;
    bool blank = false;
    while (s < end && (*s == ' ' || *s == '\t')) { s++; blank = true; }
    if (s >= end || (blank && *s == '#')) { *cur = end; return false; }
    *word = s;
    while (s < end && *s != ' ' && *s != '\t') s++;
    *len = (size_t)(s - *word);
    *cur = s;
    return true;
}

/* Validate a directive line and apply it. */
static oyl_status parse_directive(oyl_parser *p, bool *had_yaml) {
    oyl_str line = p->current.value;
    const char *cur = line.data + 1, *end = line.data + line.len;
    const char *name, *w1, *w2, *extra;
    size_t name_len, l1, l2, lx;

    if (!directive_word(&cur, end, &name, &name_len) || name != line.data + 1)
        PARSE_ERROR(p, "invalid directive");
    for (size_t i = 0; i < line.len; i++) {
        uint8_t c = (uint8_t)line.data[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            PARSE_ERROR(p, "control character in directive");
    }

    if (name_len == 4 && memcmp(name, "YAML", 4) == 0) {
        if (*had_yaml) PARSE_ERROR(p, "duplicate %YAML directive");
        *had_yaml = true;
        if (!directive_word(&cur, end, &w1, &l1))
            PARSE_ERROR(p, "%YAML directive requires a version");
        size_t i = 0, dot = 0;
        while (i < l1 && w1[i] >= '0' && w1[i] <= '9') i++;
        dot = i;
        if (dot == 0 || dot >= l1 || w1[dot] != '.')
            PARSE_ERROR(p, "invalid %YAML version");
        i = dot + 1;
        while (i < l1 && w1[i] >= '0' && w1[i] <= '9') i++;
        if (i != l1 || i == dot + 1)
            PARSE_ERROR(p, "invalid %YAML version");
        if (directive_word(&cur, end, &extra, &lx))
            PARSE_ERROR(p, "unexpected text after %YAML version");
        return OYL_OK;
    }

    if (name_len == 3 && memcmp(name, "TAG", 3) == 0) {
        if (!directive_word(&cur, end, &w1, &l1) ||
            !directive_word(&cur, end, &w2, &l2))
            PARSE_ERROR(p, "%TAG directive requires a handle and a prefix");
        if (directive_word(&cur, end, &extra, &lx))
            PARSE_ERROR(p, "unexpected text after %TAG prefix");
        /* handle: !, !!, or !word! */
        bool ok = w1[0] == '!' && w1[l1 - 1] == '!';
        for (size_t i = 1; ok && i + 1 < l1; i++) {
            char c = w1[i];
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                 (c >= 'A' && c <= 'Z') || c == '-';
        }
        if (!ok) PARSE_ERROR(p, "invalid %TAG handle");
        if (l1 >= 16 || l2 >= 256) PARSE_ERROR(p, "%TAG handle or prefix too long");

        int idx = -1;
        for (int i = 0; i < p->tag_dir_count; i++) {
            if (strlen(p->tag_directives[i].handle) == l1 &&
                memcmp(p->tag_directives[i].handle, w1, l1) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            if (p->tag_dir_count >= 8) PARSE_ERROR(p, "too many %TAG directives");
            idx = p->tag_dir_count++;
        }
        memcpy(p->tag_directives[idx].handle, w1, l1);
        p->tag_directives[idx].handle[l1] = '\0';
        memcpy(p->tag_directives[idx].prefix, w2, l2);
        p->tag_directives[idx].prefix[l2] = '\0';
        return OYL_OK;
    }

    /* other directives are reserved; the spec says to ignore them */
    return OYL_OK;
}

static oyl_status parse_document(oyl_parser *p) {
    if (p->oom) return OOM_STATUS(p);
    oyl_status st = peek_token(p);
    if (st != OYL_OK) return st;

    /* a new document starts here (unless this is only a '...' or the end
     * of the stream); %TAG handles apply to the next document only */
    if (tok_type(p) != OYL_TOK_DOC_END && tok_type(p) != OYL_TOK_STREAM_END)
        p->tag_dir_count = 0;

    /* directives (%YAML, %TAG) */
    bool had_directive = false, had_yaml = false;
    while (tok_type(p) == OYL_TOK_DIRECTIVE) {
        if (p->doc_open)
            PARSE_ERROR(p, "directive requires a preceding document end marker '...'");
        st = parse_directive(p, &had_yaml);
        if (st != OYL_OK) return st;
        had_directive = true;
        consume_token(p);
        st = peek_token(p);
        if (st != OYL_OK) return st;
    }
    if (had_directive && tok_type(p) != OYL_TOK_DOC_START)
        PARSE_ERROR(p, "directives must be followed by a document start marker '---'");

    if (tok_type(p) == OYL_TOK_DOC_START) {
        /* explicit doc start */
        if (p->doc_open) {
            unroll_all(p, p->current.start);
            oyl_event evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            enqueue(p, &evt);
            p->doc_open = false;
        }
        oyl_mark doc_start = p->current.start;
        oyl_mark doc_end = p->current.end;
        p->doc_start_line = doc_start.line;
        consume_token(p);
        oyl_event evt = evt_simple(OYL_EVT_DOC_START);
        evt.implicit = false;
        evt.start = doc_start;
        evt.end = doc_end;
        enqueue(p, &evt);
        p->doc_open = true;

        /* peek for content */
        st = peek_token(p);
        if (st != OYL_OK) return st;

        if (tok_type(p) == OYL_TOK_DIRECTIVE)
            PARSE_ERROR(p, "directive requires a preceding document end marker '...'");
        if (tok_type(p) == OYL_TOK_STREAM_END ||
            tok_type(p) == OYL_TOK_DOC_END ||
            tok_type(p) == OYL_TOK_DOC_START) {
            /* empty document */
            emit_empty(p);
        } else {
            st = parse_block_node(p);
            if (st != OYL_OK) return st;
            st = expect_document_end(p);
            if (st != OYL_OK) return st;
        }

        return OYL_OK;
    }

    if (tok_type(p) == OYL_TOK_DOC_END) {
        if (p->doc_open) {
            unroll_all(p, p->current.start);
            oyl_event evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = false;
            evt.start = p->current.start;
            evt.end = p->current.end;
            enqueue(p, &evt);
            p->doc_open = false;
        }
        consume_token(p);
        return OYL_OK;
    }

    if (tok_type(p) == OYL_TOK_STREAM_END) {
        return OYL_OK; /* handled by caller */
    }

    if (tok_type(p) == OYL_TOK_FLOW_SEQ_END || tok_type(p) == OYL_TOK_FLOW_MAP_END ||
        tok_type(p) == OYL_TOK_FLOW_ENTRY)
        PARSE_ERROR(p, "unexpected flow indicator outside a flow collection");

    /* implicit document */
    if (!p->doc_open) {
        p->doc_start_line = 0;
        ensure_doc(p, false, p->current.start);
    }

    st = parse_block_node(p);
    if (st != OYL_OK) return st;
    return expect_document_end(p);
}

/* ── Parse stream ────────────────────────────────────────── */

/* Parse the stream's next document into the event list: STREAM_START
 * comes before the first, and STREAM_END after the last. One document at
 * a time keeps the list as small as the largest document. */
static oyl_status parse_stream(oyl_parser *p) {
    oyl_status st;

    if (!p->stream_started) {
        st = peek_token(p);
        if (st != OYL_OK) return st;
        oyl_mark stream_start = p->current.start;
        oyl_mark stream_start_end = p->current.end;
        consume_token(p);

        oyl_event sevt = evt_simple(OYL_EVT_STREAM_START);
        sevt.start = stream_start;
        sevt.end = stream_start_end;
        enqueue(p, &sevt);
        p->stream_started = true;
    }

    st = peek_token(p);
    if (st != OYL_OK) return st;

    if (tok_type(p) == OYL_TOK_STREAM_END || tok_type(p) == OYL_TOK_NONE) {
        if (p->doc_open) {
            unroll_all(p, p->current.start);
            oyl_event evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            enqueue(p, &evt);
            p->doc_open = false;
        }
        oyl_mark end_mark = p->current.start;
        oyl_mark end_mark_end = p->current.end;
        consume_token(p);
        {
            oyl_event se = evt_simple(OYL_EVT_STREAM_END);
            se.start = end_mark;
            se.end = end_mark_end;
            enqueue(p, &se);
        }
        p->stream_ended = true;
        /* the event limit may be hit on the closing events above */
        return p->oom ? OOM_STATUS(p) : OYL_OK;
    }

    /* safety: a document that consumed no input can never make
     * progress (it would emit empty nodes forever) */
    size_t prev_off = p->current.start.offset;
    oyl_token_type prev_type = tok_type(p);

    st = parse_document(p);
    if (st != OYL_OK) return st;

    if (p->have_token && p->current.start.offset == prev_off &&
        tok_type(p) == prev_type)
        PARSE_ERROR(p, "unexpected token");
    return p->oom ? OOM_STATUS(p) : OYL_OK;
}

/* ── Merge key resolution ────────────────────────────────── */

/* Compute the exclusive end index for the node starting at idx */
static int node_end(const oyl_event *events, int len, int idx) {
    oyl_event_type t = events[idx].type;
    if (t == OYL_EVT_SCALAR || t == OYL_EVT_ALIAS)
        return idx + 1;
    if (t == OYL_EVT_MAPPING_START || t == OYL_EVT_SEQUENCE_START) {
        oyl_event_type end_t = (t == OYL_EVT_MAPPING_START)
            ? OYL_EVT_MAPPING_END : OYL_EVT_SEQUENCE_END;
        int depth = 1;
        for (int j = idx + 1; j < len; j++) {
            if (events[j].type == t) depth++;
            else if (events[j].type == end_t && --depth == 0) return j + 1;
        }
    }
    return idx + 1;
}

static uint32_t name_hash(oyl_str s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.len; i++) h = (h ^ (uint8_t)s.data[i]) * 16777619u;
    return h;
}

/* Anchor table. Merge and resolve run after bind_anchors, so each anchor
 * name is unique in the stream; the hash index maps a name to its entry. */
typedef struct {
    oyl_str name;
    int     start;  /* index of anchored node */
    int     end;    /* exclusive end */
} merge_anchor;

typedef struct {
    merge_anchor *entries;
    int           len;
    int           cap;
    int          *index;     /* entry index + 1 per slot, 0 if empty */
    size_t        index_cap; /* a power of two, or 0 without an index */
} merge_anchor_table;

static void atbl_init(merge_anchor_table *t) {
    t->entries = NULL; t->len = 0; t->cap = 0;
    t->index = NULL; t->index_cap = 0;
}

static void atbl_free(merge_anchor_table *t) {
    free(t->entries);
    free(t->index);
    atbl_init(t);
}

static void atbl_add(merge_anchor_table *t, oyl_str name, int start, int end) {
    if (t->len >= t->cap) {
        int nc = t->cap ? t->cap * 2 : 16;
        merge_anchor *ne = realloc(t->entries, nc * sizeof(merge_anchor));
        if (!ne) return;
        t->entries = ne; t->cap = nc;
    }
    t->entries[t->len++] = (merge_anchor){name, start, end};
}

static bool atbl_name_is(const merge_anchor *a, oyl_str name) {
    return a->name.len == name.len && memcmp(a->name.data, name.data, name.len) == 0;
}

/* The latest entry with this name, or NULL. */
static merge_anchor *atbl_lookup(merge_anchor_table *t, oyl_str name) {
    if (!t->index) {   /* no memory for an index: scan */
        for (int i = t->len - 1; i >= 0; i--)
            if (atbl_name_is(&t->entries[i], name)) return &t->entries[i];
        return NULL;
    }
    for (size_t h = name_hash(name) & (t->index_cap - 1); t->index[h];
         h = (h + 1) & (t->index_cap - 1)) {
        merge_anchor *a = &t->entries[t->index[h] - 1];
        if (atbl_name_is(a, name)) return a;
    }
    return NULL;
}

static void atbl_build(merge_anchor_table *t, const oyl_event *events, int len) {
    atbl_init(t);
    for (int i = 0; i < len; i++) {
        if (events[i].anchor.data && events[i].anchor.len > 0) {
            int end = node_end(events, len, i);
            atbl_add(t, events[i].anchor, i, end);
        }
    }
    if (t->len == 0) return;
    size_t cap = 16;
    while (cap < (size_t)t->len * 2) cap *= 2;
    t->index = calloc(cap, sizeof *t->index);
    if (!t->index) return;
    t->index_cap = cap;
    for (int i = 0; i < t->len; i++) {
        size_t h = name_hash(t->entries[i].name) & (cap - 1);
        while (t->index[h] && !atbl_name_is(&t->entries[t->index[h] - 1], t->entries[i].name))
            h = (h + 1) & (cap - 1);
        t->index[h] = i + 1;   /* a later entry with the same name wins */
    }
}

/* Dynamic event array for building output */
typedef struct {
    oyl_event *data;
    int        len;
    int        cap;
    int        limit;    /* max events, or 0 for none */
    bool       oom;      /* stop: allocation failure or limit reached */
    bool       limited;  /* the limit (not allocation) stopped it */
} evt_buf;

static void ebuf_init(evt_buf *b, int cap, int limit) {
    if (cap < 64) cap = 64;
    b->data = malloc((size_t)cap * sizeof(oyl_event));
    b->len = 0;
    b->cap = cap;
    b->limit = limit;
    b->oom = !b->data;
    b->limited = false;
}

static void ebuf_free(evt_buf *b) {
    free(b->data);
    b->data = NULL; b->len = 0; b->cap = 0;
}

static void ebuf_push(evt_buf *b, oyl_event e) {
    if (b->oom) return;
    if (b->limit > 0 && b->len >= b->limit) {
        b->oom = true;
        b->limited = true;
        return;
    }
    if (b->len >= b->cap) {
        if (b->cap > INT_MAX / 2) { b->oom = true; return; }
        int nc = b->cap * 2;
        oyl_event *nd = realloc(b->data, (size_t)nc * sizeof(oyl_event));
        if (!nd) { b->oom = true; return; }
        b->data = nd; b->cap = nc;
    }
    b->data[b->len++] = e;
}

static void ebuf_push_range(evt_buf *b, const oyl_event *events, int start, int end) {
    for (int i = start; i < end; i++)
        ebuf_push(b, events[i]);
}

/* Key set for tracking seen keys (override semantics) */
typedef struct {
    oyl_str *keys;
    int      len;
    int      cap;
} key_set;

static void kset_init(key_set *ks) {
    ks->keys = NULL; ks->len = 0; ks->cap = 0;
}

static void kset_free(key_set *ks) {
    free(ks->keys);
    ks->keys = NULL; ks->len = 0; ks->cap = 0;
}

static bool kset_contains(const key_set *ks, oyl_str key) {
    for (int i = 0; i < ks->len; i++) {
        /* empty keys may have NULL data: don't pass it to memcmp */
        if (ks->keys[i].len == key.len &&
            (key.len == 0 || memcmp(ks->keys[i].data, key.data, key.len) == 0))
            return true;
    }
    return false;
}

static void kset_add(key_set *ks, oyl_str key) {
    if (kset_contains(ks, key)) return;
    if (ks->len >= ks->cap) {
        int nc = ks->cap ? ks->cap * 2 : 16;
        oyl_str *nk = realloc(ks->keys, nc * sizeof(oyl_str));
        if (!nk) return;
        ks->keys = nk; ks->cap = nc;
    }
    ks->keys[ks->len++] = key;
}

static bool is_merge_key(const oyl_event *evt) {
    return evt->type == OYL_EVT_SCALAR
        && evt->scalar_style == OYL_SCALAR_PLAIN
        && evt->value.len == 2
        && evt->value.data[0] == '<' && evt->value.data[1] == '<';
}

/* Merge key-value pairs from a source mapping into buf, skipping already-seen keys */
static void merge_from_mapping(const oyl_event *events, int evt_len,
                               int map_start, int map_end,
                               evt_buf *out, key_set *seen) {
    /* iterate key-value pairs inside the mapping (skip MAPPING_START/END) */
    int pos = map_start + 1;
    while (pos < map_end - 1) {
        int key_start = pos;
        int key_end = node_end(events, evt_len, pos);
        int val_start = key_end;
        int val_end = node_end(events, evt_len, val_start);

        /* only scalar keys participate in override checking */
        bool is_scalar_key = (key_end - key_start == 1 &&
                              events[key_start].type == OYL_EVT_SCALAR);
        bool skip = false;
        if (is_scalar_key) {
            oyl_str kv = events[key_start].value;
            if (kset_contains(seen, kv)) {
                skip = true;
            } else {
                kset_add(seen, kv);
            }
        }

        if (!skip) {
            /* copy key events; copies carry no anchors (not new definitions) */
            for (int j = key_start; j < key_end && !out->oom; j++) {
                oyl_event copy = events[j];
                copy.anchor = OYL_STR_NULL;
                ebuf_push(out, copy);
            }
            /* copy value events */
            for (int j = val_start; j < val_end && !out->oom; j++) {
                oyl_event copy = events[j];
                copy.anchor = OYL_STR_NULL;
                ebuf_push(out, copy);
            }
        }

        pos = val_end;
    }
}

/* Merge one merge source: an inline mapping or an alias to a mapping.
 * Returns an error message, or NULL. */
static const char *merge_source(const oyl_event *events, int evt_len, int pos,
                                merge_anchor_table *anchors,
                                evt_buf *out, key_set *seen) {
    if (events[pos].type == OYL_EVT_MAPPING_START) {
        merge_from_mapping(events, evt_len, pos, node_end(events, evt_len, pos),
                           out, seen);
        return NULL;
    }
    if (events[pos].type == OYL_EVT_ALIAS) {
        merge_anchor *a = atbl_lookup(anchors, events[pos].value);
        if (!a) return "merge key refers to an undefined anchor";
        if (events[a->start].type != OYL_EVT_MAPPING_START)
            return "merge key must refer to a mapping";
        merge_from_mapping(events, evt_len, a->start, a->end, out, seen);
        return NULL;
    }
    return "merge value must be a mapping or a sequence of mappings";
}

/* Process a merge key's value: a mapping, an alias to one, or a sequence
 * of those (earlier sources win on conflicts). Returns an error message,
 * or NULL. */
static const char *process_merge_value(const oyl_event *events, int evt_len,
                                       int val_start, int val_end,
                                       merge_anchor_table *anchors,
                                       evt_buf *out, key_set *seen) {
    if (events[val_start].type != OYL_EVT_SEQUENCE_START)
        return merge_source(events, evt_len, val_start, anchors, out, seen);

    /* <<: [*a, {k: v}, ...] */
    int pos = val_start + 1;
    int seq_end = val_end - 1; /* SEQUENCE_END */
    while (pos < seq_end) {
        const char *err = merge_source(events, evt_len, pos, anchors, out, seen);
        if (err) return err;
        pos = node_end(events, evt_len, pos);
    }
    return NULL;
}

/* Event budget for merge/alias expansion. Expansion can grow the stream
 * exponentially ("billion laughs"), so it is bounded by max_events, or, if
 * that is disabled, by a multiple of the unexpanded stream. */
static int expansion_limit(const oyl_parser *p, int unexpanded) {
    if (p->max_events > 0)   /* what the stream has left */
        return p->max_events - p->evt_base > 1 ? p->max_events - p->evt_base : 1;
    long long lim = (long long)unexpanded * 16 + 100000;
    return lim > INT_MAX / 2 ? INT_MAX / 2 : (int)lim;
}

/* Status for an expansion buffer that stopped early. */
static oyl_status expansion_failed(oyl_parser *p, const evt_buf *b) {
    if (!b->limited) return OYL_ERR_MEMORY;
    hit_limit(p, "alias/merge expansion exceeds the event limit");
    return OYL_ERR_LIMIT;
}

static oyl_status resolve_merges(oyl_parser *p) {
    #define MAX_MERGE_PASSES 32
    int limit = expansion_limit(p, p->evt_len);

    for (int pass = 0; pass < MAX_MERGE_PASSES; pass++) {
        merge_anchor_table anchors;
        atbl_build(&anchors, p->events, p->evt_len);

        evt_buf out;
        ebuf_init(&out, p->evt_len * 2, limit);
        bool changed = false;

        int i = 0;
        while (i < p->evt_len && !out.oom) {
            if (p->events[i].type != OYL_EVT_MAPPING_START) {
                ebuf_push(&out, p->events[i]);
                i++;
                continue;
            }

            /* found a mapping — find its end */
            int map_start = i;
            int map_end = node_end(p->events, p->evt_len, i);

            /* check if this mapping has any merge keys */
            bool has_merge = false;
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int ke = node_end(p->events, p->evt_len, pos);
                    if (is_merge_key(&p->events[pos])) { has_merge = true; break; }
                    int ve = node_end(p->events, p->evt_len, ke);
                    pos = ve;
                }
            }

            if (!has_merge) {
                /* no merge keys at this level — just push MAPPING_START
                 * and let the loop continue to process inner events
                 * (which may contain nested mappings with merge keys) */
                ebuf_push(&out, p->events[i]);
                i++;
                continue;
            }

            /* this mapping has merges — rebuild it */
            changed = true;

            /* first collect explicit (non-merge) keys */
            key_set seen;
            kset_init(&seen);

            /* pass 1: record all explicit keys */
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int key_start = pos;
                    int key_end = node_end(p->events, p->evt_len, pos);
                    int val_end = node_end(p->events, p->evt_len, key_end);

                    if (!is_merge_key(&p->events[key_start])) {
                        if (key_end - key_start == 1 &&
                            p->events[key_start].type == OYL_EVT_SCALAR) {
                            kset_add(&seen, p->events[key_start].value);
                        }
                    }
                    pos = val_end;
                }
            }

            /* emit MAPPING_START */
            ebuf_push(&out, p->events[map_start]);

            /* pass 2: emit explicit pairs first */
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int key_start = pos;
                    int key_end = node_end(p->events, p->evt_len, pos);
                    int val_end = node_end(p->events, p->evt_len, key_end);

                    if (!is_merge_key(&p->events[key_start])) {
                        ebuf_push_range(&out, p->events, key_start, val_end);
                    }
                    pos = val_end;
                }
            }

            /* pass 3: process merge keys in order, injecting non-duplicate pairs */
            {
                int pos = map_start + 1;
                while (pos < map_end - 1) {
                    int key_start = pos;
                    int key_end = node_end(p->events, p->evt_len, pos);
                    int val_start = key_end;
                    int val_end = node_end(p->events, p->evt_len, key_end);

                    if (is_merge_key(&p->events[key_start])) {
                        const char *err = process_merge_value(
                            p->events, p->evt_len, val_start, val_end,
                            &anchors, &out, &seen);
                        if (err) {
                            kset_free(&seen);
                            atbl_free(&anchors);
                            ebuf_free(&out);
                            snprintf(p->error_msg, sizeof(p->error_msg), "%s", err);
                            p->error_mark = p->events[key_start].start;
                            return OYL_ERR_PARSE;
                        }
                    }
                    pos = val_end;
                }
            }

            /* emit MAPPING_END */
            ebuf_push(&out, p->events[map_end - 1]);

            kset_free(&seen);
            i = map_end;
        }

        atbl_free(&anchors);

        if (out.oom) {
            oyl_status st = expansion_failed(p, &out);
            ebuf_free(&out);
            return st;
        }

        if (!changed) {
            ebuf_free(&out);
            break;
        }

        /* replace event array */
        free(p->events);
        p->events = out.data;
        p->evt_len = out.len;
        p->evt_cap = out.cap;
        p->evt_cursor = 0;
        /* don't free out — ownership transferred */
    }

    return OYL_OK;
    #undef MAX_MERGE_PASSES
}

/* ── Anchor binding ──────────────────────────────────────── */

/* An alias refers to the most recent node with that anchor *before* it in
 * the same document (YAML 1.2 §3.2.2.2). Anchor names may be reused, so
 * before merge/alias expansion every anchor definition gets a unique
 * internal name ("name\0<n>") and each alias is pointed at the definition
 * it refers to. The name-based expansion code below then resolves exactly
 * the right node. An alias with no preceding definition (undefined, a
 * forward reference, or one from an earlier document) keeps its original
 * name, which matches no internal name, and stays an unresolved alias.
 * unbind_anchors() restores the original names afterwards, pointing back
 * into the input as before; original names never contain '\0' (the
 * scanner stops names at NUL). */

typedef struct { oyl_str name; oyl_str bound; } anchor_slot;

static oyl_status bind_anchors(oyl_parser *p) {
    int nanchors = 0;
    for (int i = 0; i < p->evt_len; i++)
        if (p->events[i].anchor.data && p->events[i].anchor.len) nanchors++;
    if (nanchors == 0) return OYL_OK;

    size_t cap = 16;
    while (cap < (size_t)nanchors * 2) cap *= 2;
    anchor_slot *tab = calloc(cap, sizeof *tab);
    /* the slots filled in the current document: anchors are per document,
     * and clearing the whole table at each one would cost the whole
     * stream's anchor count per document */
    size_t *used = malloc((size_t)nanchors * sizeof *used);
    int nused = 0;
    free(p->bound_names);
    p->bound_names = malloc(((size_t)nanchors + 1) * sizeof *p->bound_names);
    if (!tab || !used || !p->bound_names) {
        free(tab);
        free(used);
        return OYL_ERR_MEMORY;
    }

    unsigned serial = 0;
    for (int i = 0; i < p->evt_len; i++) {
        oyl_event *e = &p->events[i];
        if (e->type == OYL_EVT_DOC_START) {
            while (nused > 0) tab[used[--nused]] = (anchor_slot){0};
            continue;
        }
        /* an anchored node is defined at its start, so aliases inside it
         * (recursive references) bind to it too */
        if (e->anchor.data && e->anchor.len) {
            char *u = oyl_arena_alloc(p->arena, e->anchor.len + 12, 1);
            if (!u) { free(tab); free(used); return OYL_ERR_MEMORY; }
            memcpy(u, e->anchor.data, e->anchor.len);
            p->bound_names[++serial] = e->anchor;
            int n = snprintf(u + e->anchor.len, 12, "%c%u", '\0', serial);
            oyl_str bound = { u, e->anchor.len + (size_t)n };
            size_t h = name_hash(e->anchor) & (cap - 1);
            while (tab[h].name.data &&
                   !(tab[h].name.len == e->anchor.len &&
                     memcmp(tab[h].name.data, e->anchor.data, e->anchor.len) == 0))
                h = (h + 1) & (cap - 1);
            if (!tab[h].name.data) used[nused++] = h;
            tab[h].name = e->anchor;
            tab[h].bound = bound;
            e->anchor = bound;
        }
        if (e->type == OYL_EVT_ALIAS) {
            size_t h = name_hash(e->value) & (cap - 1);
            while (tab[h].name.data) {
                if (tab[h].name.len == e->value.len &&
                    memcmp(tab[h].name.data, e->value.data, e->value.len) == 0) {
                    e->value = tab[h].bound;
                    break;
                }
                h = (h + 1) & (cap - 1);
            }
        }
    }
    free(tab);
    free(used);
    return OYL_OK;
}

static void unbind_name(const oyl_parser *p, oyl_str *s) {
    if (!s->data) return;
    const char *z = memchr(s->data, '\0', s->len);
    if (z) *s = p->bound_names[strtoul(z + 1, NULL, 10)];
}

static void unbind_anchors(oyl_parser *p) {
    if (!p->bound_names) return;
    for (int i = 0; i < p->evt_len; i++) {
        unbind_name(p, &p->events[i].anchor);
        if (p->events[i].type == OYL_EVT_ALIAS) unbind_name(p, &p->events[i].value);
    }
    free(p->bound_names);
    p->bound_names = NULL;
}

/* ── Alias resolution ────────────────────────────────────── */

enum { ACYCLE_WHITE = 0, ACYCLE_GRAY = 1, ACYCLE_BLACK = 2 };

static bool alias_has_cycle(int idx, merge_anchor_table *t, int *color,
                            const oyl_event *events) {
    color[idx] = ACYCLE_GRAY;
    for (int j = t->entries[idx].start; j < t->entries[idx].end; j++) {
        if (events[j].type != OYL_EVT_ALIAS) continue;
        merge_anchor *a = atbl_lookup(t, events[j].value);
        if (!a) continue;
        int k = (int)(a - t->entries);
        if (color[k] == ACYCLE_GRAY) return true;
        if (color[k] == ACYCLE_WHITE && alias_has_cycle(k, t, color, events))
            return true;
    }
    color[idx] = ACYCLE_BLACK;
    return false;
}

static bool is_cyclic_name(merge_anchor_table *t, bool *cyclic, oyl_str name) {
    merge_anchor *a = atbl_lookup(t, name);
    return a && cyclic[a - t->entries];
}

/* The depth limit protects consumers that recurse, so it applies to the
 * finished eager event list, whatever built it: a flow collection used as a
 * key gains a mapping around it, and alias/merge expansion can nest far
 * deeper than the input (in a chain of anchors that each contain an alias
 * of the previous one, every expansion adds levels). */
static oyl_status check_eager_depth(oyl_parser *p) {
    if (p->max_depth <= 0) return OYL_OK;
    int depth = 0;
    for (int i = 0; i < p->evt_len; i++) {
        switch (p->events[i].type) {
        case OYL_EVT_MAPPING_START: case OYL_EVT_SEQUENCE_START:
            if (++depth > p->max_depth) {
                hit_limit(p, p->merge_enabled || p->resolve_enabled
                                 ? "nesting depth limit exceeded after alias/merge expansion"
                                 : "nesting depth limit exceeded");
                return OYL_ERR_LIMIT;
            }
            break;
        case OYL_EVT_MAPPING_END: case OYL_EVT_SEQUENCE_END:
            depth--;
            break;
        default:
            break;
        }
    }
    return OYL_OK;
}

static oyl_status resolve_aliases(oyl_parser *p) {
    /* build initial anchor table for cycle detection */
    merge_anchor_table anchors;
    atbl_build(&anchors, p->events, p->evt_len);

    if (anchors.len == 0) {
        atbl_free(&anchors);
        return OYL_OK;
    }

    /* detect cycles via DFS */
    int *color = calloc(anchors.len, sizeof(int));
    bool *cyclic = calloc(anchors.len, sizeof(bool));
    if (!color || !cyclic) {
        free(color); free(cyclic);
        atbl_free(&anchors);
        return OYL_ERR_MEMORY;
    }

    for (int i = 0; i < anchors.len; i++) {
        if (color[i] == ACYCLE_WHITE) {
            if (alias_has_cycle(i, &anchors, color, p->events)) {
                for (int k = 0; k < anchors.len; k++)
                    if (color[k] == ACYCLE_GRAY) cyclic[k] = true;
            }
        }
    }
    atbl_free(&anchors);

    /* expand non-cyclic aliases in passes */
    int limit = expansion_limit(p, p->evt_len);
    for (int pass = 0; pass < 32; pass++) {
        atbl_build(&anchors, p->events, p->evt_len);

        evt_buf out;
        ebuf_init(&out, p->evt_len * 2, limit);
        bool changed = false;

        for (int i = 0; i < p->evt_len && !out.oom; i++) {
            if (p->events[i].type != OYL_EVT_ALIAS) {
                ebuf_push(&out, p->events[i]);
                continue;
            }

            merge_anchor *a = atbl_lookup(&anchors, p->events[i].value);
            if (!a || is_cyclic_name(&anchors, cyclic, p->events[i].value)) {
                ebuf_push(&out, p->events[i]);
                continue;
            }

            changed = true;
            /* copies carry no anchors: they are not new definitions, and
             * keeping them would grow the anchor table every pass (and
             * misalign it with the cyclic[] flags, which are indexed by
             * the original table) */
            for (int j = a->start; j < a->end && !out.oom; j++) {
                oyl_event copy = p->events[j];
                copy.anchor = OYL_STR_NULL;
                ebuf_push(&out, copy);
            }
        }

        atbl_free(&anchors);

        if (out.oom) {
            oyl_status st = expansion_failed(p, &out);
            ebuf_free(&out);
            free(color);
            free(cyclic);
            return st;
        }

        if (!changed) {
            ebuf_free(&out);
            break;
        }

        free(p->events);
        p->events = out.data;
        p->evt_len = out.len;
        p->evt_cap = out.cap;
        p->evt_cursor = 0;
    }

    free(color);
    free(cyclic);
    return OYL_OK;
}

/* ── Flow-as-key lookahead ──────────────────────────────── */

/* Before parsing a flow collection incrementally we must know whether it is
 * an implicit key ([a]: b), because MAPPING_START has to be emitted first.
 * We answer by scanning raw bytes to the matching ] or } and checking for a
 * following ':' (skipping whitespace and comments).
 *
 * One scan answers the question for every collection nested inside the
 * scanned one, so results are cached: later queries for nested collections
 * are a binary search instead of a rescan. This keeps the lookahead linear
 * overall (rescanning per collection is quadratic in nesting depth), and for
 * a typical document the whole top-level collection is scanned exactly once.
 *
 * The scan has to skip quoted scalars, but a quote character can also be
 * plain text ("don't"), part of a tag or anchor name ("!''"), or start a
 * quoted scalar after properties ("&a 'x'"). Telling these apart needs the
 * scanner's full tokenization, so the scan only decides the cases that are
 * certain (fk_quote_kind) and otherwise answers "key" for everything it
 * covers. That errs on the safe side: a "key" answer sends the parser to
 * the eager path, which decides keys after parsing the collection and so
 * is always right, only slower. A wrong "not a key" would not be. '#'
 * starts a comment only after whitespace. */

/* What a quote character at input[i] is, given that the scan's quote
 * tracking is exact up to i (`qend` is just past the last quoted scalar it
 * skipped): 1 if it certainly opens a quoted scalar, 0 if it is certainly
 * plain text, -1 if telling needs full tokenization. */
ALWAYS_INLINE int fk_quote_kind(const char *input, size_t i, size_t qend) {
    char c = input[i - 1];          /* i > 0: the scan starts at a bracket */
    if (c != ' ' && c != '\t') {
        /* directly after an entry start, or after a value ':' that
         * follows a closed scalar or collection ({"a":"b"}, [a]:'b') */
        if (c == '[' || c == '{' || c == ',') return 1;
        if (c == ':' && i >= 2 &&
            (((input[i - 2] == '"' || input[i - 2] == '\'') && i - 1 == qend) ||
             input[i - 2] == ']' || input[i - 2] == '}'))
            return 1;
        /* inside a word: plain text, or a tag or anchor name, since a
         * quoted scalar only starts a token */
        return 0;
    }
    size_t j = i;
    while (j > 0 && (input[j - 1] == ' ' || input[j - 1] == '\t')) j--;
    if (j == 0) return 1;
    c = input[j - 1];
    /* an entry start, or ": " (in flow context ':' before a blank is always
     * the value indicator) */
    if (c == '[' || c == '{' || c == ',' || c == ':') return 1;
    return -1;      /* after properties, text, '?', ... */
}

/* Is the byte after a closing bracket at `i` (skipping blanks, breaks and
 * comments) a ':'? */
static bool fk_colon_follows(const char *input, size_t len, size_t i) {
    for (size_t j = i + 1; j < len; j++) {
        char nc = input[j];
        if (nc == ' ' || nc == '\t' || nc == '\n' || nc == '\r') continue;
        if (nc == '#') {
            const char *nl = memchr(input + j, '\n', len - j);
            if (!nl) return false;
            j = (size_t)(nl - input);
            continue;
        }
        return nc == ':';
    }
    return false;
}

/* The scan only stops at the bytes ' " # [ ] { } < \n \r. A cursor walks
 * them using a bitmask built 64 bytes at a time (oyl_flow_mask64, SIMD on
 * x86-64): bit k of `bits` marks a stop at base + k. Skipping a quoted
 * scalar or comment walks the same bits, so no byte is examined twice. */
typedef struct {
    const char *in;
    size_t      len;
    size_t      base;
    uint64_t    bits;
} fk_cursor;

static inline uint64_t fk_mask_at(const char *in, size_t len, size_t base) {
    if (len - base >= 64) return oyl_flow_mask64(in + base);
    uint64_t m = 0;                      /* the last partial block */
    for (size_t k = 0; base + k < len; k++) {
        char c = in[base + k];
        if (c == '\'' || c == '"' || c == '#' || c == '[' || c == ']' ||
            c == '{' || c == '}' || c == '<' || c == '\n' || c == '\r')
            m |= (uint64_t)1 << k;
    }
    return m;
}

/* Position the cursor so the next stop is the first at or after `pos`. */
static inline void fk_seek(fk_cursor *c, size_t pos) {
    c->base = pos;
    c->bits = pos < c->len ? fk_mask_at(c->in, c->len, pos) : 0;
}

/* Offset of the next stop (consuming it), or `len` if there is none. */
static inline size_t fk_next(fk_cursor *c) {
    while (c->bits == 0) {
        if (c->len - c->base <= 64) { c->base = c->len; return c->len; }
        c->base += 64;
        c->bits = fk_mask_at(c->in, c->len, c->base);
    }
    size_t k = (size_t)__builtin_ctzll(c->bits);
    c->bits &= c->bits - 1;
    return c->base + k;
}

/* Consume a quoted scalar whose opening quote is at `i`. Returns the
 * offset just past its closing quote, or `len` if it is unterminated. */
static size_t fk_skip_quoted(fk_cursor *c, size_t i) {
    const char *in = c->in;
    char q = in[i];
    for (;;) {
        size_t k = fk_next(c);
        if (k == c->len) return c->len;
        if (in[k] != q) continue;
        if (q == '\'') {
            if (k + 1 < c->len && in[k + 1] == '\'') { fk_next(c); continue; } /* '' */
            return k + 1;
        }
        /* double-quoted: escaped if preceded by an odd run of backslashes */
        size_t bs = 0;
        while (k - bs > i + 1 && in[k - bs - 1] == '\\') bs++;
        if (!(bs & 1)) return k + 1;
    }
}

static int fk_cmp(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return (x > y) - (x < y);
}

/* Scan the collection opening at `offset`, filling the cache with the open
 * offsets of every collection within it (itself included) that is a key.
 * Returns false on allocation failure. */
static bool fk_scan(oyl_parser *p, size_t offset) {
    const char *input = p->input;
    size_t len = p->input_len;
    int depth = 0;
    size_t qend = SIZE_MAX;     /* just past the last skipped quoted scalar */
    fk_cursor cur = { input, len, 0, 0 };

    p->fk_valid = false;
    p->fk_nkeys = 0;
    fk_seek(&cur, offset);

    for (;;) {
        size_t i = fk_next(&cur);
        if (i == len) goto unterminated;
        switch (input[i]) {
        case '\'': case '"':
            switch (fk_quote_kind(input, i, qend)) {
            case 1:
                qend = fk_skip_quoted(&cur, i);
                if (qend == len) goto unterminated;
                break;
            case -1:
                goto ambiguous;
            default:
                break;
            }
            break;
        case '<':
            /* A verbatim tag ("!<tag:a,b>") may contain [ ] { } , # and
             * quotes, so it is skipped whole, to its '>'. It starts a token
             * only after a blank or an entry start; elsewhere "!<" is text. */
            if (i >= 1 && input[i - 1] == '!' &&
                (i == 1 || input[i - 2] == ' ' || input[i - 2] == '\t' ||
                 input[i - 2] == '[' || input[i - 2] == '{' || input[i - 2] == ',')) {
                size_t k = i + 1;
                while (k < len && input[k] != '>' && input[k] != ' ' &&
                       input[k] != '\t' && input[k] != '\n' && input[k] != '\r')
                    k++;
                if (k == len || input[k] != '>') goto ambiguous;
                fk_seek(&cur, k + 1);
            }
            break;
        case '#':
            /* a comment runs to the next \n, which it consumes */
            if (i > 0 && (input[i - 1] == ' ' || input[i - 1] == '\t' ||
                          input[i - 1] == '\n' || input[i - 1] == '\r')) {
                size_t k;
                while ((k = fk_next(&cur)) != len && input[k] != '\n') {}
                if (k == len) goto unterminated;
            }
            break;
        case '[': case '{':
            if (depth >= p->fk_stack_cap) {
                int nc = p->fk_stack_cap ? p->fk_stack_cap * 2 : 64;
                size_t *ns = realloc(p->fk_stack, (size_t)nc * sizeof(size_t));
                if (!ns) return false;
                p->fk_stack = ns;
                p->fk_stack_cap = nc;
            }
            p->fk_stack[depth++] = i;
            break;
        case '\n': case '\r':
            /* An implicit key is a single line: collections still open at
             * a line break can't be keys, so the scan can stop here. For a
             * multi-line document (JSON) this keeps the lookahead to one
             * short scan per line instead of a pass over everything. */
            if (depth > 0) {
                p->fk_lo = offset;
                p->fk_hi = i;
                goto done;
            }
            break;
        case ']': case '}':
            if (depth == 0) break;
            depth--;
            if (fk_colon_follows(input, len, i)) {
                if (p->fk_nkeys >= p->fk_keys_cap) {
                    int nc = p->fk_keys_cap ? p->fk_keys_cap * 2 : 16;
                    size_t *nk = realloc(p->fk_keys, (size_t)nc * sizeof(size_t));
                    if (!nk) return false;
                    p->fk_keys = nk;
                    p->fk_keys_cap = nc;
                }
                p->fk_keys[p->fk_nkeys++] = p->fk_stack[depth];
            }
            if (depth == 0) {
                p->fk_lo = offset;
                p->fk_hi = i;
                goto done;
            }
            break;
        default:
            break;
        }
    }
ambiguous:
    /* telling needs full tokenization: answer "key" for everything here,
     * which sends the parser to the eager path (see above) */
    p->fk_nkeys = -1;
    p->fk_lo = offset;
    p->fk_hi = len;
    p->fk_valid = true;
    return true;
unterminated:
    /* unclosed collections are never keys */
    p->fk_lo = offset;
    p->fk_hi = len;
done:
    if (p->fk_nkeys > 1)
        qsort(p->fk_keys, (size_t)p->fk_nkeys, sizeof(size_t), fk_cmp);
    p->fk_valid = true;
    return true;
}

/* Returns true if the flow collection opening at `offset` is an implicit
 * key, i.e. ':' follows its matching close bracket. */
static bool flow_is_block_key(oyl_parser *p, size_t offset) {
    if (!p->fk_valid || offset < p->fk_lo || offset > p->fk_hi) {
        /* on allocation failure, claim "key": the eager fallback is always
         * correct, just slower */
        if (!fk_scan(p, offset)) return true;
    }
    if (p->fk_nkeys < 0) return true;
    size_t lo = 0, hi = (size_t)p->fk_nkeys;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->fk_keys[mid] < offset) lo = mid + 1;
        else hi = mid;
    }
    return lo < (size_t)p->fk_nkeys && p->fk_keys[lo] == offset;
}

/* ── Incremental state machine ───────────────────────────── */

static inline void inc_emit(oyl_parser *p, const oyl_event *evt) {
    if (p->max_events > 0 && p->events_delivered + p->out_len >= p->max_events) {
        hit_limit(p, "event limit exceeded");
        return;
    }
    if (p->out_len < 8)
        p->out_buf[p->out_len++] = *evt;
}

static inline void inc_push_frame(oyl_parser *p, ctx_type type, int indent,
                                   parser_state return_state) {
    if ((type == CTX_BLOCK_MAP || type == CTX_BLOCK_SEQ) && p->doc_start_line &&
        p->current.start.line == p->doc_start_line) {
        stop_with(p, OYL_ERR_PARSE, "block collection cannot start on the '---' line");
        return;
    }
    if (p->max_depth > 0 && p->frame_len >= p->max_depth) {
        /* the collection's start event (and, for a block mapping, its
         * first key) was just queued: withhold them, so no delivered event
         * nests deeper than the limit */
        for (int k = p->out_len - 1; k >= p->out_cursor; k--) {
            if (p->out_buf[k].type == OYL_EVT_MAPPING_START ||
                p->out_buf[k].type == OYL_EVT_SEQUENCE_START) {
                p->out_len = k;
                break;
            }
        }
        hit_limit(p, "nesting depth limit exceeded");
        return;
    }
    if (p->frame_len >= p->frame_cap) {
        int nc = p->frame_cap * 2;
        state_frame *nf = realloc(p->frames, nc * sizeof(state_frame));
        if (!nf) { p->oom = true; return; }
        p->frames = nf;
        p->frame_cap = nc;
    }
    p->frames[p->frame_len++] = (state_frame){type, indent, return_state};
}

static inline state_frame inc_pop_frame(oyl_parser *p) {
    if (p->frame_len > 0) return p->frames[--p->frame_len];
    return (state_frame){0, -1, ST_DONE};
}

static inline state_frame *inc_top_frame(oyl_parser *p) {
    if (p->frame_len > 0) return &p->frames[p->frame_len - 1];
    return NULL;
}

/* Emit a scalar event for the current (SCALAR) token, with pending props,
 * and consume the token. The event is built in place in out_buf: building
 * it in a local and copying it over stalls on store forwarding. */
static inline void inc_emit_scalar_token(oyl_parser *p) {
    if (p->max_events > 0 && p->events_delivered + p->out_len >= p->max_events) {
        hit_limit(p, "event limit exceeded");
        return;
    }
    if (p->out_len < 8) {
        oyl_event *e = &p->out_buf[p->out_len++];
        *e = (oyl_event){
            .type = OYL_EVT_SCALAR,
            .value = p->current.value,
            .scalar_style = p->current.scalar_style,
            .start = p->current.start,
            .end = p->current.end,
        };
        attach_props(p, e);
    }
    consume_token(p);
}

/* After a flow collection entry: consume ',' or go straight to the close
 * state, skipping a round trip through the SEP/LOOP states. Anything else
 * is left to the SEP state, which reports the error. */
static inline oyl_status inc_flow_sep(oyl_parser *p, bool in_seq) {
    oyl_status st = peek_token(p);
    if (st != OYL_OK) return st;
    oyl_token_type tt = tok_type(p);
    if (tt == OYL_TOK_FLOW_ENTRY) {
        consume_token(p);
        p->state = in_seq ? ST_FLOW_SEQ_LOOP : ST_FLOW_MAP_LOOP;
    } else if (tt == (in_seq ? OYL_TOK_FLOW_SEQ_END : OYL_TOK_FLOW_MAP_END)) {
        p->state = in_seq ? ST_FLOW_SEQ_END : ST_FLOW_MAP_END;
    } else {
        p->state = in_seq ? ST_FLOW_SEQ_SEP : ST_FLOW_MAP_SEP;
    }
    return OYL_OK;
}

/* Flow mapping value, with the ':' already consumed. */
static inline oyl_status inc_flow_map_value(oyl_parser *p) {
    oyl_status st = peek_token(p);
    if (st != OYL_OK) return st;
    oyl_token_type tt = tok_type(p);
    if (tt == OYL_TOK_SCALAR) {
        /* common case: scalar value, emitted in the same step */
        inc_emit_scalar_token(p);
        return inc_flow_sep(p, false);
    }
    if (tt == OYL_TOK_FLOW_ENTRY || tt == OYL_TOK_FLOW_MAP_END) {
        /* empty value */
        oyl_event evt = evt_simple(OYL_EVT_SCALAR);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);
        p->state = ST_FLOW_MAP_SEP;
    } else {
        p->node_return = ST_FLOW_MAP_SEP;
        p->state = ST_FLOW_NODE;
    }
    return OYL_OK;
}

/* Only blanks before `offset` on its line (the input's start, after a byte
 * order mark, counts as a line start): a ':' there is an explicit value's
 * or an empty key's, never an implicit key's (the scanner's classify_colon
 * decides the same way). */
static bool starts_line(const oyl_parser *p, size_t offset) {
    size_t k = offset;
    while (k > 0 && (p->input[k - 1] == ' ' || p->input[k - 1] == '\t')) k--;
    if (k == 0 || p->input[k - 1] == '\n' || p->input[k - 1] == '\r') return true;
    return k == 3 && memcmp(p->input, "\xEF\xBB\xBF", 3) == 0;
}

/* Bytes of input between checkpoints. The fuzz builds set it small, so
 * that their short inputs take several. */
#ifndef OYL_CKPT_SPACING
#define OYL_CKPT_SPACING 65536
#endif

/* At a document's start, save the scanner's state, so that a fallback to
 * the eager parser re-parses from here, not from the start of the stream.
 * Only in a clean state (nothing buffered or open); otherwise an earlier
 * checkpoint stays, and the fallback re-parses from there. */
static void inc_checkpoint(oyl_parser *p) {
    if (p->out_len || p->scan_error || p->doc_open || p->frame_len ||
        p->has_anchor || p->has_tag || !p->have_token)
        return;
    /* Copying the scanner's state costs about as much as parsing a tiny
     * document, so a checkpoint follows the last one by OYL_CKPT_SPACING
     * bytes. A fallback then re-parses at most that much more, still a
     * document at a time. */
    if (p->ckpt && p->current.start.offset - p->ckpt_tok.start.offset < OYL_CKPT_SPACING)
        return;
    if (!p->ckpt) {
        p->ckpt = oyl_scanner_new(p->input, p->input_len, p->arena);
        if (!p->ckpt) return;
    }
    if (!oyl_scanner_copy(p->ckpt, p->scanner)) return;
    p->ckpt_tok = p->current;
    p->ckpt_have_tok = p->have_token;
    p->ckpt_delivered = p->events_delivered;
    p->ckpt_sig = p->delivered_sig;
}

/* Flow-context states. Kept out of parser_step so the block-context
 * dispatch stays compact; parser_step forwards every ST_FLOW_* state here. */
/* After a document's root node: only '...', '---' or the end of the stream
 * may follow. Emits DOC_END and moves on to the next document. */
static oyl_status inc_end_document(oyl_parser *p) {
    oyl_status st = peek_token(p);
    if (st != OYL_OK) return st;
    oyl_token_type tt = tok_type(p);
    if (tt != OYL_TOK_DOC_END && tt != OYL_TOK_DOC_START &&
        tt != OYL_TOK_STREAM_END)
        PARSE_ERROR(p, "unexpected content after document root node");
    oyl_event evt = evt_simple(OYL_EVT_DOC_END);
    if (tt == OYL_TOK_DOC_END) {
        consume_token(p);
        evt.implicit = false;
    } else {
        evt.implicit = true;
    }
    evt.start = p->current.start;
    /* "..." is the event's extent; an implicit end is zero width, where
     * what follows starts */
    evt.end = evt.implicit ? p->current.start : p->current.end;
    inc_emit(p, &evt);
    p->doc_open = false;
    p->state = ST_STREAM_DOC_LOOP;
    return OYL_OK;
}

static oyl_status parser_step_flow(oyl_parser *p) {
    oyl_token_type tt;
    int col;
    oyl_event evt;

    switch (p->state) {

    /* ── Flow node dispatch (incremental) ──────────────────── */

    case ST_FLOW_NODE: {
        peek_token(p);
        tt = tok_type(p);

        /* consume properties if present */
        if (tt == OYL_TOK_TAG || tt == OYL_TOK_ANCHOR) {
            oyl_status st = consume_props(p);
            if (st != OYL_OK) return st;
            peek_token(p);
            tt = tok_type(p);
        }

        if (tt == OYL_TOK_SCALAR) {
            inc_emit_scalar_token(p);
            p->state = p->node_return;
            return OYL_OK;
        }

        if (tt == OYL_TOK_ALIAS) {
            evt = evt_simple(OYL_EVT_ALIAS);
            evt.value = p->current.value;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, &evt);
            p->state = p->node_return;
            return OYL_OK;
        }

        if (tt == OYL_TOK_FLOW_SEQ_START) {
            col = tok_col(p);
            evt = evt_simple(OYL_EVT_SEQUENCE_START);
            evt.flow = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, &evt);
            inc_push_frame(p, CTX_FLOW_SEQ, col, p->node_return);
            p->state = ST_FLOW_SEQ_LOOP;
            return OYL_OK;
        }

        if (tt == OYL_TOK_FLOW_MAP_START) {
            col = tok_col(p);
            evt = evt_simple(OYL_EVT_MAPPING_START);
            evt.flow = true;
            evt.start = p->current.start;
            evt.end = p->current.end;
            attach_props(p, &evt);
            consume_token(p);
            inc_emit(p, &evt);
            inc_push_frame(p, CTX_FLOW_MAP, col, p->node_return);
            p->state = ST_FLOW_MAP_LOOP;
            return OYL_OK;
        }

        /* empty node */
        evt = evt_simple(OYL_EVT_SCALAR);
        evt.start = p->current.start;
        evt.end = p->current.start;
        attach_props(p, &evt);
        inc_emit(p, &evt);
        p->state = p->node_return;
        return OYL_OK;
    }

    /* ── Flow mapping states (incremental) ─────────────────── */

    case ST_FLOW_MAP_LOOP: {
        if (p->oom) return OOM_STATUS(p);
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_FLOW_MAP_END) {
            p->state = ST_FLOW_MAP_END;
            return OYL_OK;
        }

        /* comma with no entry before it: { , a: b } */
        if (tt == OYL_TOK_FLOW_ENTRY)
            PARSE_ERROR(p, "unexpected ',' in flow mapping");

        /* explicit key: ? */
        if (tt == OYL_TOK_BLOCK_MAP_KEY) {
            consume_token(p);
            peek_token(p);
            tt = tok_type(p);
            if (tt == OYL_TOK_BLOCK_MAP_VALUE ||
                tt == OYL_TOK_FLOW_ENTRY ||
                tt == OYL_TOK_FLOW_MAP_END) {
                /* empty key */
                evt = evt_simple(OYL_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, &evt);
                p->state = ST_FLOW_MAP_VALUE;
            } else {
                p->node_return = ST_FLOW_MAP_VALUE;
                p->state = ST_FLOW_NODE;
            }
            return OYL_OK;
        }

        /* common case: scalar key — emit it, and the value too when it is
         * a scalar, in a single step */
        if (tt == OYL_TOK_SCALAR) {
            inc_emit_scalar_token(p);
            p->state = ST_FLOW_MAP_VALUE;
            oyl_status st = peek_token(p);
            if (st != OYL_OK) return st;
            if (tok_type(p) != OYL_TOK_BLOCK_MAP_VALUE) return OYL_OK;
            consume_token(p);
            return inc_flow_map_value(p);
        }

        /* implicit key: dispatch to flow node, then check for : */
        p->node_return = ST_FLOW_MAP_VALUE;
        p->state = ST_FLOW_NODE;
        return OYL_OK;
    }

    case ST_FLOW_MAP_VALUE: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_BLOCK_MAP_VALUE) {
            consume_token(p);
            return inc_flow_map_value(p);
        } else {
            /* no ':', emit empty value */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_FLOW_MAP_SEP;
        }
        return OYL_OK;
    }

    case ST_FLOW_MAP_SEP: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_FLOW_ENTRY) {
            consume_token(p);
            p->state = ST_FLOW_MAP_LOOP;
            return OYL_OK;
        }
        if (tt == OYL_TOK_FLOW_MAP_END) {
            p->state = ST_FLOW_MAP_LOOP; /* will close next iteration */
            return OYL_OK;
        }
        PARSE_ERROR(p, "expected ',' or '}' in flow mapping");
    }

    case ST_FLOW_MAP_END: {
        oyl_mark close = p->current.start;
        oyl_mark close_end = p->current.end;
        consume_token(p); /* consume } */
        evt = evt_simple(OYL_EVT_MAPPING_END);
        evt.start = close;
        evt.end = close_end;
        inc_emit(p, &evt);
        state_frame frame = inc_pop_frame(p);
        p->state = frame.return_state;
        if (p->state == ST_FLOW_SEQ_SEP) return inc_flow_sep(p, true);
        if (p->state == ST_FLOW_MAP_SEP) return inc_flow_sep(p, false);
        return OYL_OK;
    }

    /* ── Flow sequence states (incremental) ────────────────── */

    case ST_FLOW_SEQ_LOOP: {
        if (p->oom) return OOM_STATUS(p);
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_FLOW_SEQ_END) {
            p->state = ST_FLOW_SEQ_END;
            return OYL_OK;
        }

        /* explicit pair: ? key : value. Properties can't precede the
         * '?' (it starts a pair, not a node), as the eager parser has it */
        if (tt == OYL_TOK_BLOCK_MAP_KEY) {
            if (p->has_anchor || p->has_tag)
                PARSE_ERROR(p, "expected ',' or ']' in flow sequence");
            p->state = ST_FLOW_SEQ_EXPLICIT_KEY;
            return OYL_OK;
        }

        /* scalar or alias: potential implicit pair key */
        if (tt == OYL_TOK_SCALAR || tt == OYL_TOK_ALIAS) {
            p->state = ST_FLOW_SEQ_ENTRY;
            goto flow_seq_entry;
        }

        /* nested flow collection as entry — could be implicit pair key
         * ([{a}: val]) which we can't handle incrementally.  Scan ahead
         * to the matching close bracket: if ':' follows it's a pair key
         * and we fall back to eager; otherwise parse incrementally. */
        if (tt == OYL_TOK_FLOW_SEQ_START || tt == OYL_TOK_FLOW_MAP_START) {
            if (flow_is_block_key(p, p->current.start.offset)) {
                p->state = ST_EAGER_DRAIN;
            } else {
                p->node_return = ST_FLOW_SEQ_SEP;
                p->state = ST_FLOW_NODE;
            }
            return OYL_OK;
        }

        /* implicit pair with empty key: [ : value ] */
        if (tt == OYL_TOK_BLOCK_MAP_VALUE) {
            /* empty key, with any props before the ':' ("[&a : b]"); the
             * pair starts where the key does */
            oyl_event key = evt_simple(OYL_EVT_SCALAR);
            key.start = p->current.start;
            key.end = p->current.start;
            attach_props(p, &key);

            evt = evt_simple(OYL_EVT_MAPPING_START);
            evt.flow = true;
            evt.start = key.start;
            evt.end = key.start;
            inc_emit(p, &evt);
            inc_emit(p, &key);

            consume_token(p); /* consume : */

            /* parse value */
            peek_token(p);
            tt = tok_type(p);
            if (tt == OYL_TOK_FLOW_ENTRY ||
                tt == OYL_TOK_FLOW_SEQ_END) {
                evt = evt_simple(OYL_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, &evt);
                p->state = ST_FLOW_SEQ_IMPLICIT_END;
            } else {
                p->node_return = ST_FLOW_SEQ_IMPLICIT_END;
                p->state = ST_FLOW_NODE;
            }
            return OYL_OK;
        }

        /* TAG/ANCHOR: consume props, re-enter loop */
        if (tt == OYL_TOK_TAG || tt == OYL_TOK_ANCHOR) {
            oyl_status st = consume_props(p);
            if (st != OYL_OK) return st;
            st = peek_token(p);
            if (st != OYL_OK) return st;
            tt = tok_type(p);
            if (tt == OYL_TOK_FLOW_ENTRY || tt == OYL_TOK_FLOW_SEQ_END) {
                /* properties on an empty node: [&a, b] */
                evt = evt_simple(OYL_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                attach_props(p, &evt);
                inc_emit(p, &evt);
                return inc_flow_sep(p, true);
            }
            /* a second anchor or tag that consume_props refused would
             * otherwise be re-dispatched here forever */
            if (tt == OYL_TOK_TAG || tt == OYL_TOK_ANCHOR)
                PARSE_ERROR(p, "expected ',' or ']' in flow sequence");
            /* props consumed, stay in ST_FLOW_SEQ_LOOP to re-dispatch */
            return OYL_OK;
        }

        /* comma with no entry before it: [ , a ] or [ a, , b ] */
        if (tt == OYL_TOK_FLOW_ENTRY)
            PARSE_ERROR(p, "unexpected ',' in flow sequence");

        PARSE_ERROR(p, "unexpected token in flow sequence");
    }

    case ST_FLOW_SEQ_ENTRY:
    flow_seq_entry: {
        /* Parse scalar/alias, peek for ':' to detect implicit pair */
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_SCALAR) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.value = p->current.value;
            evt.scalar_style = p->current.scalar_style;
            evt.start = p->current.start;
            evt.end = p->current.end;
        } else { /* ALIAS */
            evt = evt_simple(OYL_EVT_ALIAS);
            evt.value = p->current.value;
            evt.start = p->current.start;
            evt.end = p->current.end;
        }
        attach_props(p, &evt);
        consume_token(p);

        /* peek for ':' — implicit pair detection */
        peek_token(p);
        if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
            p->saved_node = evt;
            p->have_saved_node = true;
            p->state = ST_FLOW_SEQ_ENTRY_KEY;
            return OYL_OK;
        }

        /* plain entry, not a pair */
        inc_emit(p, &evt);
        return inc_flow_sep(p, true);
    }

    case ST_FLOW_SEQ_ENTRY_KEY: {
        /* Implicit pair detected: emit MAPPING_START + saved key */
        evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = p->saved_node.start;
        evt.end = p->saved_node.start;
        inc_emit(p, &evt);
        inc_emit(p, &p->saved_node);
        p->have_saved_node = false;

        /* consume ':' */
        consume_token(p);

        /* check for empty value */
        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_FLOW_ENTRY || tt == OYL_TOK_FLOW_SEQ_END) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_FLOW_SEQ_IMPLICIT_END;
        } else {
            p->node_return = ST_FLOW_SEQ_IMPLICIT_END;
            p->state = ST_FLOW_NODE;
        }
        return OYL_OK;
    }

    case ST_FLOW_SEQ_IMPLICIT_END: {
        evt = evt_simple(OYL_EVT_MAPPING_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);
        p->state = ST_FLOW_SEQ_SEP;
        return OYL_OK;
    }

    case ST_FLOW_SEQ_EXPLICIT_KEY: {
        /* ? key : value pair in flow sequence */
        evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.flow = true;
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, &evt);

        consume_token(p); /* consume ? */

        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_BLOCK_MAP_VALUE ||
            tt == OYL_TOK_FLOW_ENTRY ||
            tt == OYL_TOK_FLOW_SEQ_END) {
            /* empty key */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_FLOW_SEQ_EXPLICIT_VALUE;
        } else {
            p->node_return = ST_FLOW_SEQ_EXPLICIT_VALUE;
            p->state = ST_FLOW_NODE;
        }
        return OYL_OK;
    }

    case ST_FLOW_SEQ_EXPLICIT_VALUE: {
        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_BLOCK_MAP_VALUE) {
            consume_token(p);
            peek_token(p);
            tt = tok_type(p);
            if (tt == OYL_TOK_FLOW_ENTRY ||
                tt == OYL_TOK_FLOW_SEQ_END) {
                evt = evt_simple(OYL_EVT_SCALAR);
                evt.start = p->current.start;
                evt.end = p->current.start;
                inc_emit(p, &evt);
            } else {
                p->node_return = ST_FLOW_SEQ_EXPLICIT_END;
                p->state = ST_FLOW_NODE;
                return OYL_OK;
            }
        } else {
            /* no value indicator, emit empty */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
        }
        p->state = ST_FLOW_SEQ_EXPLICIT_END;
        return OYL_OK;
    }

    case ST_FLOW_SEQ_EXPLICIT_END: {
        evt = evt_simple(OYL_EVT_MAPPING_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);
        p->state = ST_FLOW_SEQ_SEP;
        return OYL_OK;
    }

    case ST_FLOW_SEQ_CHECK_COLON: {
        /* After nested flow collection entry, check for ':' */
        peek_token(p);
        if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
            /* nested collection as implicit pair key — rare, fall back */
            p->state = ST_EAGER_DRAIN;
            return OYL_OK;
        }
        p->state = ST_FLOW_SEQ_SEP;
        return OYL_OK;
    }

    case ST_FLOW_SEQ_SEP: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_FLOW_ENTRY) {
            consume_token(p);
            p->state = ST_FLOW_SEQ_LOOP;
            return OYL_OK;
        }
        if (tt == OYL_TOK_FLOW_SEQ_END) {
            p->state = ST_FLOW_SEQ_LOOP; /* will close next iteration */
            return OYL_OK;
        }
        PARSE_ERROR(p, "expected ',' or ']' in flow sequence");
    }

    case ST_FLOW_SEQ_END: {
        oyl_mark close = p->current.start;
        oyl_mark close_end = p->current.end;
        consume_token(p); /* consume ] */
        evt = evt_simple(OYL_EVT_SEQUENCE_END);
        evt.start = close;
        evt.end = close_end;
        inc_emit(p, &evt);
        state_frame frame = inc_pop_frame(p);
        p->state = frame.return_state;
        if (p->state == ST_FLOW_SEQ_SEP) return inc_flow_sep(p, true);
        if (p->state == ST_FLOW_MAP_SEP) return inc_flow_sep(p, false);
        return OYL_OK;
    }

    default:
        break;
    }

    return OYL_ERR_PARSE;
}

static oyl_status parser_step(oyl_parser *p) {
    oyl_token_type tt;
    int col;
    oyl_mark mark;
    oyl_event evt;
    state_frame *top;

    if (p->oom) return OOM_STATUS(p);

    switch (p->state) {

    case ST_STREAM_START:
        evt = evt_simple(OYL_EVT_STREAM_START);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.end;
        consume_token(p);
        inc_emit(p, &evt);
        p->state = ST_STREAM_DOC_LOOP;
        return OYL_OK;

    case ST_STREAM_DOC_LOOP:
        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_STREAM_END) {
            p->state = ST_STREAM_END;
            return OYL_OK;
        }
        p->state = ST_DOC_DIRECTIVES;
        return OYL_OK;

    case ST_STREAM_END:
        evt = evt_simple(OYL_EVT_STREAM_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.end;
        consume_token(p);
        inc_emit(p, &evt);
        p->stream_ended = true;
        p->state = ST_DONE;
        return OYL_OK;

    case ST_DOC_DIRECTIVES:
        /* check for %TAG / %YAML directives — fall back to eager */
        peek_token(p);
        tt = tok_type(p);
        inc_checkpoint(p);
        if (tt == OYL_TOK_DIRECTIVE) {
            p->state = ST_EAGER_DRAIN;
            return OYL_OK;
        }
        if (tt == OYL_TOK_DOC_START) {
            p->state = ST_DOC_START_EXPLICIT;
        } else if (tt == OYL_TOK_DOC_END) {
            /* doc end without content, emit implicit doc */
            consume_token(p);
            p->state = ST_STREAM_DOC_LOOP;
        } else if (tt == OYL_TOK_STREAM_END) {
            p->state = ST_STREAM_END;
        } else {
            p->state = ST_DOC_START_IMPLICIT;
        }
        return OYL_OK;

    case ST_DOC_START_EXPLICIT: {
        /* close previous doc if open */
        if (p->doc_open) {
            /* unroll any open collections */
            if (p->frame_len > 0) {
                p->unroll_return = ST_DOC_START_EXPLICIT;
                p->state = ST_UNROLL;
                return OYL_OK;
            }
            evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = true;
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->doc_open = false;
        }
        peek_token(p);
        mark = p->current.start;
        oyl_mark marker_end = p->current.end;            /* the "---" */
        p->doc_start_line = mark.line;
        consume_token(p);
        evt = evt_simple(OYL_EVT_DOC_START);
        evt.implicit = false;
        evt.start = mark;
        evt.end = marker_end;
        inc_emit(p, &evt);
        p->doc_open = true;
        p->state = ST_DOC_CONTENT;
        return OYL_OK;
    }

    case ST_DOC_START_IMPLICIT: {
        if (p->doc_open) {
            if (p->frame_len > 0) {
                p->unroll_return = ST_DOC_START_IMPLICIT;
                p->state = ST_UNROLL;
                return OYL_OK;
            }
            evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = true;
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->doc_open = false;
        }
        peek_token(p);
        p->doc_start_line = 0;
        evt = evt_simple(OYL_EVT_DOC_START);
        evt.implicit = true;
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);
        p->doc_open = true;
        p->state = ST_DOC_CONTENT;
        return OYL_OK;
    }

    case ST_DOC_CONTENT:
        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_DOC_END) {
            /* empty document body */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_DOC_END_EXPLICIT;
            return OYL_OK;
        }
        if (tt == OYL_TOK_DOC_START) {
            /* empty document body, new doc follows */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            /* close this doc implicitly */
            evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->doc_open = false;
            p->state = ST_STREAM_DOC_LOOP;
            return OYL_OK;
        }
        if (tt == OYL_TOK_STREAM_END) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            evt = evt_simple(OYL_EVT_DOC_END);
            evt.implicit = true;
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->doc_open = false;
            p->state = ST_STREAM_END;
            return OYL_OK;
        }
        /* non-trivial content — parse as block node */
        p->node_return = ST_DOC_END_EXPLICIT;
        p->state = ST_BLOCK_NODE;
        return OYL_OK;

    case ST_BLOCK_NODE:
        /* Consume properties (anchor, tag) */
        peek_token(p);
        tt = tok_type(p);

        /* Consume anchor/tag properties */
        if (tt == OYL_TOK_TAG || tt == OYL_TOK_ANCHOR) {
            /* Fall back to eager for nodes with properties — the double-props
             * logic is complex and interacts with collection start detection.
             * We'll handle the simple (no props) cases incrementally first. */
            p->state = ST_EAGER_DRAIN;
            return OYL_OK;
        }

        if (tt == OYL_TOK_ALIAS) {
            p->state = ST_BLOCK_NODE_ALIAS;
            return OYL_OK;
        }

        if (tt == OYL_TOK_BLOCK_SEQ_ENTRY) {
            p->state = ST_BLOCK_NODE_SEQ;
            return OYL_OK;
        }

        if (tt == OYL_TOK_BLOCK_MAP_KEY) {
            p->state = ST_BLOCK_NODE_EXPLICIT_KEY;
            return OYL_OK;
        }

        if (tt == OYL_TOK_BLOCK_MAP_VALUE) {
            p->state = ST_BLOCK_NODE_BARE_VALUE;
            return OYL_OK;
        }

        if (tt == OYL_TOK_FLOW_SEQ_START || tt == OYL_TOK_FLOW_MAP_START) {
            /* Check if this flow collection is used as a complex block key
             * ([flow]: value). A map value on an implicit key's ':' line
             * can't be one ("k: [a]: b" is invalid); after a ':' that
             * starts its line (an explicit value's, or an empty key's), it
             * can (": [a]: b"), as the scanner has it. Elsewhere, scan
             * ahead to check if ':' follows the matching close bracket. */
            if ((p->node_return == ST_BLOCK_MAP_LOOP &&
                 p->current.start.line == p->value_colon_line &&
                 !p->value_colon_starts_line) ||
                !flow_is_block_key(p, p->current.start.offset)) {
                p->state = ST_FLOW_NODE;
            } else {
                p->state = ST_EAGER_DRAIN;
            }
            return OYL_OK;
        }

        if (tt == OYL_TOK_SCALAR) {
            p->state = ST_BLOCK_NODE_SCALAR;
            return OYL_OK;
        }

        if (tt == OYL_TOK_FLOW_SEQ_END || tt == OYL_TOK_FLOW_MAP_END ||
            tt == OYL_TOK_FLOW_ENTRY)
            PARSE_ERROR(p, "unexpected flow indicator outside a flow collection");
        if (tt == OYL_TOK_DIRECTIVE)
            PARSE_ERROR(p, "directive requires a preceding document end marker '...'");

        /* DOC_START, DOC_END, STREAM_END → empty node */
        p->state = ST_BLOCK_NODE_EMPTY;
        return OYL_OK;

    case ST_BLOCK_NODE_SCALAR: {
        peek_token(p);
        mark = p->current.start;
        col = tok_col(p);
        evt = (oyl_event){
            .type = OYL_EVT_SCALAR,
            .value = p->current.value,
            .scalar_style = p->current.scalar_style,
            .start = p->current.start,
            .end = p->current.end,
        };
        consume_token(p);

        /* peek for ':' → this scalar is a mapping key. The ':' must be on
         * the scalar's line (an implicit key is one line; a block scalar,
         * whose token ends on the next line, never is one) */
        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_BLOCK_MAP_VALUE && p->current.start.line == evt.start.line) {
            p->saved_node = evt;
            p->saved_node_col = col;
            p->have_saved_node = true;
            p->state = ST_BLOCK_NODE_SCALAR_KEY;
            return OYL_OK;
        }

        /* plain scalar value */
        inc_emit(p, &evt);

        /* return to parent context */
        p->state = p->node_return;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_SCALAR_KEY: {
        /* We have a saved scalar that's a key → emit MAPPING_START + key scalar */
        evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.start = p->saved_node.start;
        evt.end = p->saved_node.start;
        inc_emit(p, &evt);

        /* emit the key scalar */
        inc_emit(p, &p->saved_node);
        p->have_saved_node = false;

        /* push block map context — return to caller's node_return after map ends */
        inc_push_frame(p, CTX_BLOCK_MAP, p->saved_node_col, p->node_return);

        /* consume ':' and parse value */
        p->state = ST_BLOCK_MAP_VALUE;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_ALIAS: {
        peek_token(p);
        mark = p->current.start;
        col = tok_col(p);
        evt = (oyl_event){
            .type = OYL_EVT_ALIAS,
            .value = p->current.value,
            .start = p->current.start,
            .end = p->current.end,
        };
        consume_token(p);

        /* peek for ':' → alias as mapping key
         * Only if ':' col >= alias col (otherwise ':' belongs to parent map) */
        peek_token(p);
        tt = tok_type(p);
        if (tt == OYL_TOK_BLOCK_MAP_VALUE && p->current.start.line == evt.start.line) {
            p->saved_node = evt;
            p->saved_node_col = col;
            p->have_saved_node = true;
            p->state = ST_BLOCK_NODE_ALIAS_KEY;
            return OYL_OK;
        }

        inc_emit(p, &evt);

        /* return to parent context */
        p->state = p->node_return;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_ALIAS_KEY: {
        evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.start = p->saved_node.start;
        evt.end = p->saved_node.start;
        inc_emit(p, &evt);
        inc_emit(p, &p->saved_node);
        p->have_saved_node = false;
        inc_push_frame(p, CTX_BLOCK_MAP, p->saved_node_col, p->node_return);
        p->state = ST_BLOCK_MAP_VALUE;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_SEQ: {
        peek_token(p);
        int seq_indent = tok_col(p);
        evt = evt_simple(OYL_EVT_SEQUENCE_START);
        evt.start = p->current.start;
        evt.end = p->current.end;                         /* the "-" */

        inc_emit(p, &evt);
        inc_push_frame(p, CTX_BLOCK_SEQ, seq_indent, p->node_return);
        p->state = ST_BLOCK_SEQ_LOOP;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_EXPLICIT_KEY: {
        /* ? key: starts a mapping */
        peek_token(p);
        int map_indent = tok_col(p);
        evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.start = p->current.start;
        evt.end = p->current.end;                         /* the "?" */

        inc_emit(p, &evt);
        inc_push_frame(p, CTX_BLOCK_MAP, map_indent, p->node_return);
        p->state = ST_BLOCK_MAP_LOOP;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_BARE_VALUE: {
        /* : at start → mapping with empty key */
        peek_token(p);
        int map_indent = tok_col(p);
        evt = evt_simple(OYL_EVT_MAPPING_START);
        evt.start = p->current.start;
        evt.end = p->current.end;                         /* the ":" */

        inc_emit(p, &evt);
        inc_push_frame(p, CTX_BLOCK_MAP, map_indent, p->node_return);

        /* emit empty key */
        evt = evt_simple(OYL_EVT_SCALAR);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);

        p->state = ST_BLOCK_MAP_VALUE;
        return OYL_OK;
    }

    case ST_BLOCK_NODE_EMPTY: {
        peek_token(p);
        evt = evt_simple(OYL_EVT_SCALAR);
        evt.start = p->current.start;
        evt.end = p->current.end;
        inc_emit(p, &evt);

        /* return to parent context */
        p->state = p->node_return;
        return OYL_OK;
    }

    case ST_BLOCK_MAP_LOOP: {
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int map_indent = top ? top->indent : 0;

        /* check if we're still in this mapping's indent level */
        if (tt == OYL_TOK_BLOCK_MAP_KEY && col == map_indent) {
            p->state = ST_BLOCK_MAP_EXPLICIT_KEY;
            return OYL_OK;
        }

        if (tt == OYL_TOK_SCALAR || tt == OYL_TOK_ALIAS) {
            /* Simple key: must be at map indent and followed by ':' */
            if (col == map_indent) {
                p->state = ST_BLOCK_MAP_SIMPLE_KEY;
                return OYL_OK;
            }
        }

        if (tt == OYL_TOK_BLOCK_MAP_VALUE && col == map_indent) {
            /* empty key, value */
            evt = evt_simple(OYL_EVT_SCALAR);
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_VALUE;
            return OYL_OK;
        }

        if (tt == OYL_TOK_TAG || tt == OYL_TOK_ANCHOR) {
            /* Properties in mapping → fall back to eager */
            p->state = ST_EAGER_DRAIN;
            return OYL_OK;
        }

        if (tt == OYL_TOK_FLOW_SEQ_START || tt == OYL_TOK_FLOW_MAP_START) {
            /* Flow key in block mapping → eager fallback */
            p->state = ST_EAGER_DRAIN;
            return OYL_OK;
        }

        /* end of this mapping */
        p->state = ST_BLOCK_MAP_END;
        return OYL_OK;
    }

    case ST_BLOCK_MAP_EXPLICIT_KEY: {
        /* consume '?' */
        peek_token(p);
        consume_token(p);

        /* parse key node */
        peek_token(p);
        tt = tok_type(p);
        top = inc_top_frame(p);
        int ek_indent = top ? top->indent : 0;

        if (tok_col(p) < ek_indent ||
            (tok_col(p) == ek_indent && tt != OYL_TOK_BLOCK_SEQ_ENTRY) ||
            tt == OYL_TOK_DOC_START || tt == OYL_TOK_DOC_END || tt == OYL_TOK_STREAM_END) {
            /* nothing indented past the '?' (a block sequence may sit at
             * the mapping's indentation): empty key; the ':' is checked
             * as after any key */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_POST_KEY;
        } else {
            /* The key is a block node */
            p->node_return = ST_BLOCK_MAP_POST_KEY;
            p->state = ST_BLOCK_NODE;
        }
        return OYL_OK;
    }

    case ST_BLOCK_MAP_SIMPLE_KEY: {
        peek_token(p);
        tt = tok_type(p);

        if (tt == OYL_TOK_SCALAR) {
            mark = p->current.start;
            col = tok_col(p);
            evt = (oyl_event){
                .type = OYL_EVT_SCALAR,
                .value = p->current.value,
                .scalar_style = p->current.scalar_style,
                .start = p->current.start,
                .end = p->current.end,
            };
            consume_token(p);

            /* must be followed by ':' */
            peek_token(p);
            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
                inc_emit(p, &evt);
                p->state = ST_BLOCK_MAP_VALUE;
            } else {
                /* Not a key — fall back to eager since this shouldn't happen
                 * in well-formed YAML at the map indent level */
                p->state = ST_EAGER_DRAIN;
            }
        } else if (tt == OYL_TOK_ALIAS) {
            mark = p->current.start;
            evt = (oyl_event){
                .type = OYL_EVT_ALIAS,
                .value = p->current.value,
                .start = p->current.start,
                .end = p->current.end,
            };
            consume_token(p);

            peek_token(p);
            if (tok_type(p) == OYL_TOK_BLOCK_MAP_VALUE) {
                inc_emit(p, &evt);
                p->state = ST_BLOCK_MAP_VALUE;
            } else {
                p->state = ST_EAGER_DRAIN;
            }
        } else {
            p->state = ST_EAGER_DRAIN;
        }
        return OYL_OK;
    }

    case ST_BLOCK_MAP_POST_KEY: {
        /* After an explicit key's node, look for ':' */
        peek_token(p);
        tt = tok_type(p);
        top = inc_top_frame(p);
        int map_indent = top ? top->indent : 0;

        if (tt == OYL_TOK_BLOCK_MAP_VALUE && tok_col(p) == map_indent) {
            p->state = ST_BLOCK_MAP_VALUE;
        } else if (tt == OYL_TOK_BLOCK_MAP_VALUE && tok_col(p) > map_indent) {
            PARSE_ERROR(p, "explicit mapping value must be at the mapping's indentation");
        } else {
            /* missing value (a ':' further left belongs to an enclosing
             * mapping) → emit empty value, continue loop */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_LOOP;
        }
        return OYL_OK;
    }

    case ST_BLOCK_MAP_VALUE: {
        /* consume ':' */
        peek_token(p);
        if (tok_type(p) != OYL_TOK_BLOCK_MAP_VALUE) {
            /* missing value — emit empty */
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return OYL_OK;
        }
        bool colon_starts_line = starts_line(p, p->current.start.offset);
        size_t colon_line = p->current.start.line;
        consume_token(p);

        /* peek at what follows */
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int map_indent = top ? top->indent : 0;

        p->value_colon_line = colon_line;
        p->value_colon_starts_line = colon_starts_line;

        /* empty value: next token at same/less indent, or is doc/stream marker */
        if (tt == OYL_TOK_DOC_START || tt == OYL_TOK_DOC_END ||
            tt == OYL_TOK_STREAM_END) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return OYL_OK;
        }
        /* anything else at the map's indentation is the next entry (only a
         * block sequence may sit there as the value) → empty value */
        if (col == map_indent && tt != OYL_TOK_BLOCK_SEQ_ENTRY) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return OYL_OK;
        }
        /* any token at lesser indent than map → empty value
         * (seq entries at same indent are valid values per YAML spec) */
        if (col < map_indent && tt != OYL_TOK_BLOCK_SEQ_ENTRY) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return OYL_OK;
        }
        if (tt == OYL_TOK_BLOCK_SEQ_ENTRY && col < map_indent) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_MAP_LOOP;
            return OYL_OK;
        }

        /* the value is a block node — set return to map loop */
        p->node_return = ST_BLOCK_MAP_LOOP;
        p->state = ST_BLOCK_NODE;
        return OYL_OK;
    }

    case ST_BLOCK_MAP_VALUE_NODE:
        /* After parsing the value node, continue the map loop */
        p->state = ST_BLOCK_MAP_LOOP;
        return OYL_OK;

    case ST_BLOCK_MAP_END: {
        state_frame frame = inc_pop_frame(p);
        evt = evt_simple(OYL_EVT_MAPPING_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);

        /* handle doc close for top-level mapping */
        p->state = frame.return_state;
        if (p->state == ST_DOC_END_EXPLICIT) return inc_end_document(p);
        return OYL_OK;
    }

    case ST_BLOCK_SEQ_LOOP: {
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int seq_indent = top ? top->indent : 0;

        if (tt == OYL_TOK_BLOCK_SEQ_ENTRY && col == seq_indent) {
            p->state = ST_BLOCK_SEQ_ENTRY;
            return OYL_OK;
        }

        /* end of sequence */
        p->state = ST_BLOCK_SEQ_END;
        return OYL_OK;
    }

    case ST_BLOCK_SEQ_ENTRY: {
        /* consume '-' */
        peek_token(p);
        consume_token(p);

        /* peek at what follows */
        peek_token(p);
        tt = tok_type(p);
        col = tok_col(p);
        top = inc_top_frame(p);
        int seq_indent = top ? top->indent : 0;

        /* empty entry: an entry's content is indented past its '-', so
         * anything at or left of it (the next '-', an enclosing ':', ...)
         * comes after the entry */
        if (col <= seq_indent && tt != OYL_TOK_STREAM_END) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_SEQ_LOOP;
            return OYL_OK;
        }
        if (tt == OYL_TOK_DOC_START || tt == OYL_TOK_DOC_END ||
            tt == OYL_TOK_STREAM_END) {
            evt = evt_simple(OYL_EVT_SCALAR);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            p->state = ST_BLOCK_SEQ_LOOP;
            return OYL_OK;
        }

        /* entry has a value node */
        p->node_return = ST_BLOCK_SEQ_LOOP;
        p->state = ST_BLOCK_NODE;
        return OYL_OK;
    }

    case ST_BLOCK_SEQ_ENTRY_NODE:
        p->state = ST_BLOCK_SEQ_LOOP;
        return OYL_OK;

    case ST_BLOCK_SEQ_END: {
        state_frame frame = inc_pop_frame(p);
        evt = evt_simple(OYL_EVT_SEQUENCE_END);
        peek_token(p);
        evt.start = p->current.start;
        evt.end = p->current.start;
        inc_emit(p, &evt);

        p->state = frame.return_state;
        if (p->state == ST_DOC_END_EXPLICIT) return inc_end_document(p);
        return OYL_OK;
    }

    case ST_UNROLL: {
        /* Pop one frame per step, emit the corresponding end event */
        if (p->frame_len > 0) {
            state_frame frame = inc_pop_frame(p);
            if (frame.type == CTX_BLOCK_MAP || frame.type == CTX_FLOW_MAP) {
                evt = evt_simple(OYL_EVT_MAPPING_END);
            } else {
                evt = evt_simple(OYL_EVT_SEQUENCE_END);
            }
            peek_token(p);
            evt.start = p->current.start;
            evt.end = p->current.start;
            inc_emit(p, &evt);
            return OYL_OK;
        }
        /* all frames popped, continue to the return state */
        p->state = p->unroll_return;
        return OYL_OK;
    }

    case ST_DOC_END_EXPLICIT:
        return inc_end_document(p);

    case ST_FLOW_NODE:
    case ST_FLOW_MAP_LOOP: case ST_FLOW_MAP_VALUE: case ST_FLOW_MAP_SEP:
    case ST_FLOW_MAP_END:
    case ST_FLOW_SEQ_LOOP: case ST_FLOW_SEQ_ENTRY: case ST_FLOW_SEQ_ENTRY_KEY:
    case ST_FLOW_SEQ_IMPLICIT_END: case ST_FLOW_SEQ_EXPLICIT_KEY:
    case ST_FLOW_SEQ_EXPLICIT_VALUE: case ST_FLOW_SEQ_EXPLICIT_END:
    case ST_FLOW_SEQ_CHECK_COLON: case ST_FLOW_SEQ_SEP: case ST_FLOW_SEQ_END:
        return parser_step_flow(p);

    case ST_EAGER_DRAIN:
        /* Signal to next_event to fall back to eager mode */
        return OYL_OK;

    case ST_DONE:
        return OYL_OK;

    }

    return OYL_ERR_PARSE;
}

/* ── Public API ──────────────────────────────────────────── */

oyl_parser *oyl_parser_new(const char *input, size_t len, oyl_arena *a) {
    oyl_parser *p = calloc(1, sizeof(oyl_parser));
    if (!p) return NULL;

    p->scanner = oyl_scanner_new(input, len, a);
    if (!p->scanner) { free(p); return NULL; }

    p->arena = a;
    p->input = input;
    p->input_len = len;
    p->have_token = false;

    /* defer large allocation — incremental mode rarely needs events[];
     * eager fallback will grow via enqueue() / realloc as needed */
    p->evt_cap = 64;
    p->events = malloc(p->evt_cap * sizeof(oyl_event));
    if (!p->events) { oyl_scanner_free(p->scanner); free(p); return NULL; }
    p->evt_len = 0;
    p->evt_cursor = 0;

    p->ctx_cap = 16;
    p->contexts = malloc(p->ctx_cap * sizeof(ctx_entry));
    if (!p->contexts) { oyl_scanner_free(p->scanner); free(p->events); free(p); return NULL; }
    p->ctx_len = 0;

    p->pending_anchor = OYL_STR_NULL;
    p->pending_tag = OYL_STR_NULL;
    p->has_anchor = false;
    p->has_tag = false;
    p->stream_started = false;
    p->stream_ended = false;
    p->doc_open = false;
    p->merge_enabled = false;
    p->resolve_enabled = false;
    p->max_events = 10000; /* safety limits */
    p->max_depth = 256;
    p->oom = false;

    /* incremental state machine — use eager mode when merge/alias needed */
    p->incremental = true;
    p->state = ST_STREAM_START;
    p->frame_cap = 16;
    p->frames = malloc(p->frame_cap * sizeof(state_frame));
    if (!p->frames) {
        oyl_scanner_free(p->scanner);
        free(p->contexts); free(p->events); free(p);
        return NULL;
    }
    p->frame_len = 0;
    p->out_len = 0;
    p->out_cursor = 0;
    p->have_saved_node = false;

    return p;
}

/* After a fallback to the eager parser, the events the incremental one
 * delivered since the checkpoint are parsed again: skip them, checking
 * that they are the same. They may span several documents. */
static oyl_status skip_delivered(oyl_parser *p) {
    int n = p->skip_n < p->evt_len ? p->skip_n : p->evt_len;
    for (int i = 0; i < n; i++)
        p->skip_got = p->skip_got * 31 + (uint64_t)p->events[i].type;
    p->evt_cursor = n;
    p->skip_n -= n;
    if ((p->skip_n == 0 && p->skip_got != p->skip_sig) ||
        (p->skip_n > 0 && p->stream_ended)) {
        p->stream_ended = true;
        PARSE_ERROR(p, "input not supported by the incremental parser "
                       "(please report); parse with merge keys or alias "
                       "resolution enabled");
    }
    return OYL_OK;
}

static inline void inc_delivered(oyl_parser *p, const oyl_event *evt) {
    p->events_delivered++;
    p->delivered_sig = p->delivered_sig * 31 + (uint64_t)evt->type;
}

/* 31^n, wrapping as the signatures do: the signature of the n events
 * delivered after one with signature s0 is delivered_sig - s0 * 31^n. */
static uint64_t pow31(int n) {
    uint64_t r = 1, b = 31;
    for (; n > 0; n >>= 1, b *= b)
        if (n & 1) r *= b;
    return r;
}

/* ── Eager path (merge/alias enabled, or fell back from incremental):
 * one document's events at a time. Kept out of next_event, which runs
 * for every event. ── */
NOINLINE oyl_status eager_next_event(oyl_parser *p, oyl_event *evt) {
    for (;;) {
        if (dequeue(p, evt)) return OYL_OK;
        if (p->stream_ended) {
            *evt = evt_simple(OYL_EVT_NONE);
            return OYL_OK;
        }
        p->evt_base += p->evt_len;
        p->evt_len = 0;
        p->evt_cursor = 0;
        oyl_status st = parse_stream(p);
        if (st != OYL_OK) return st;
        if (p->merge_enabled || p->resolve_enabled) {
            st = bind_anchors(p);
            if (st != OYL_OK) return st;
        }
        if (p->merge_enabled) {
            st = resolve_merges(p);
            if (st != OYL_OK) return st;
        }
        if (p->resolve_enabled) {
            st = resolve_aliases(p);
            if (st != OYL_OK) return st;
        }
        if (p->merge_enabled || p->resolve_enabled)
            unbind_anchors(p);
        st = check_eager_depth(p);
        if (st != OYL_OK) return st;
        if (p->skip_n > 0) {
            st = skip_delivered(p);
            if (st != OYL_OK) return st;
        }
    }
}

static oyl_status next_event(oyl_parser *p, oyl_event *evt) {
    if (!p->incremental) return eager_next_event(p, evt);

    /* ── Incremental path (state machine) ── */

    /* drain buffered incremental events */
    if (p->out_cursor < p->out_len) {
        *evt = p->out_buf[p->out_cursor++];
        if (p->out_cursor >= p->out_len) {
            p->out_len = 0;
            p->out_cursor = 0;
        }
        inc_delivered(p, evt);
        return OYL_OK;
    }

    if (p->state == ST_DONE) {
        *evt = evt_simple(OYL_EVT_NONE);
        return OYL_OK;
    }

    /* a scanner error hit while reading ahead is reported once the events
     * produced before it have been delivered */
    if (p->scan_error) return p->scan_error;

    /* step the state machine until it produces output or finishes */
    p->out_len = 0;
    p->out_cursor = 0;
    while (p->out_len == 0) {
        oyl_status st = parser_step(p);
        if (p->scan_error) {
            /* keep only events produced before the error; states may emit
             * more after a failed peek, based on a stale token */
            p->out_len = p->scan_error_out;
            if (p->out_len > 0) break; /* drain valid events first */
            return p->scan_error;
        }
        if (st != OYL_OK) return st;
        if (p->oom) {
            if (p->out_len > 0) break; /* drain valid events first */
            return OOM_STATUS(p);
        }

        if (p->state == ST_EAGER_DRAIN) {
            /* fall back to eager: re-parse from a recent document's
             * checkpoint, or from the start, and skip what was delivered */
            int skip = p->events_delivered;
            uint64_t sig = p->delivered_sig;
            p->incremental = false;
            p->state = ST_DONE;
            p->stream_started = false;
            p->stream_ended = false;
            p->doc_open = false;
            p->have_token = false;
            p->evt_len = 0;
            p->evt_cursor = 0;
            p->ctx_len = 0;
            p->has_anchor = false;
            p->has_tag = false;
            p->pending_anchor = OYL_STR_NULL;
            p->pending_tag = OYL_STR_NULL;
            p->oom = false;
            p->stop_status = OYL_OK;
            p->events_delivered = 0;
            p->evt_base = 0;
            oyl_scanner_free(p->scanner);
            if (p->ckpt) {
                p->scanner = p->ckpt;
                p->ckpt = NULL;
                p->current = p->ckpt_tok;
                p->have_token = p->ckpt_have_tok;
                p->stream_started = true;   /* STREAM_START was delivered */
                p->evt_base = p->ckpt_delivered;
                skip -= p->ckpt_delivered;
                sig -= p->ckpt_sig * pow31(skip);
            } else {
                p->scanner = oyl_scanner_new(p->input, p->input_len, p->arena);
                if (!p->scanner) return OYL_ERR_MEMORY;
            }
            p->skip_n = skip;
            p->skip_sig = sig;
            p->skip_got = 0;
            return next_event(p, evt);
        }

        if (p->state == ST_DONE && p->out_len == 0) {
            *evt = evt_simple(OYL_EVT_NONE);
            return OYL_OK;
        }
    }

    *evt = p->out_buf[0];
    p->out_cursor = 1;
    if (p->out_cursor >= p->out_len) {
        p->out_len = 0;
        p->out_cursor = 0;
    }
    inc_delivered(p, evt);
    return OYL_OK;
}

oyl_status oyl_parse_next(oyl_parser *p, const oyl_event **evt) {
    oyl_status st = next_event(p, &p->out_evt);
    *evt = st == OYL_OK ? &p->out_evt : NULL;
    return st;
}

void oyl_parser_set_schema(oyl_parser *p, const oyl_schema *schema) {
    if (p) {
        p->schema = schema;
        if (schema) p->incremental = false;
    }
}

void oyl_parser_set_merge(oyl_parser *p, bool enable) {
    if (p) {
        p->merge_enabled = enable;
        if (enable) p->incremental = false;
    }
}

void oyl_parser_set_resolve(oyl_parser *p, bool enable) {
    if (p) {
        p->resolve_enabled = enable;
        if (enable) p->incremental = false;
    }
}

void oyl_parser_set_max_events(oyl_parser *p, int max) {
    if (p) p->max_events = max;
}

void oyl_parser_set_max_depth(oyl_parser *p, int max) {
    if (p) p->max_depth = max;
}

const char *oyl_parser_error(oyl_parser *p) {
    if (!p || p->error_msg[0] == '\0') return NULL;
    return p->error_msg;
}

oyl_mark oyl_parser_error_mark(oyl_parser *p) {
    if (!p) return (oyl_mark){0, 0, 0};
    return p->error_mark;
}

void oyl_parser_free(oyl_parser *p) {
    if (!p) return;
    oyl_scanner_free(p->scanner);
    oyl_scanner_free(p->ckpt);
    free(p->contexts);
    free(p->events);
    free(p->frames);
    free(p->fk_keys);
    free(p->fk_stack);
    free(p->bound_names);
    free(p);
}
