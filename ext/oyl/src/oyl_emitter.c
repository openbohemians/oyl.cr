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
    int     column;

    /* context stack */
    emit_ctx *stack;
    int       stack_len;
    int       stack_cap;

    /* state */
    bool doc_open;
    bool first_doc;
    bool wrote_block_key; /* just wrote a block map key, value next */
    bool after_seq_dash;  /* just wrote "- " for a block seq entry */
    bool key_was_alias;   /* the current mapping key is an alias */
    bool wrote_props;     /* the current node has an anchor or tag */
};

/* ── Buffer management ───────────────────────────────────── */

#define EMIT_INIT_CAP 4096

static oyl_status buf_ensure(oyl_emitter *e, size_t need) {
    if (e->len + need <= e->cap) return OYL_OK;
    size_t new_cap = e->cap * 2;
    if (new_cap < e->len + need) new_cap = e->len + need;
    char *nb = realloc(e->buf, new_cap);
    if (!nb) return OYL_ERR_MEMORY;
    e->buf = nb;
    e->cap = new_cap;
    return OYL_OK;
}

static oyl_status buf_put(oyl_emitter *e, char c) {
    oyl_status st = buf_ensure(e, 1);
    if (st != OYL_OK) return st;
    e->buf[e->len++] = c;
    if (c == '\n') e->column = 0;
    else e->column++;
    return OYL_OK;
}

static oyl_status buf_puts(oyl_emitter *e, const char *s, size_t slen) {
    oyl_status st = buf_ensure(e, slen);
    if (st != OYL_OK) return st;
    memcpy(e->buf + e->len, s, slen);
    e->len += slen;
    for (size_t i = slen; i > 0; i--) {
        if (s[i - 1] == '\n') {
            e->column = (int)(slen - i);
            return OYL_OK;
        }
    }
    e->column += (int)slen;
    return OYL_OK;
}

#define PUTS(e, lit) buf_puts(e, lit, sizeof(lit) - 1)

static oyl_status buf_indent(oyl_emitter *e, int spaces) {
    if (spaces <= 0) return OYL_OK;
    oyl_status st = buf_ensure(e, (size_t)spaces);
    if (st != OYL_OK) return st;
    memset(e->buf + e->len, ' ', (size_t)spaces);
    e->len += (size_t)spaces;
    e->column += spaces;
    return OYL_OK;
}

/* ── Context stack ───────────────────────────────────────── */

static oyl_status push_ctx(oyl_emitter *e, emit_ctx_type type, int indent) {
    if (e->stack_len >= e->stack_cap) {
        int new_cap = e->stack_cap * 2;
        if (new_cap < 8) new_cap = 8;
        emit_ctx *ns = realloc(e->stack, (size_t)new_cap * sizeof(emit_ctx));
        if (!ns) return OYL_ERR_MEMORY;
        e->stack = ns;
        e->stack_cap = new_cap;
    }
    e->stack[e->stack_len++] = (emit_ctx){
        .type = type,
        .indent = indent,
        .count = 0,
        .expect_key = (type == EMIT_CTX_BLOCK_MAP || type == EMIT_CTX_FLOW_MAP),
    };
    return OYL_OK;
}

static emit_ctx *top_ctx(oyl_emitter *e) {
    if (e->stack_len == 0) return NULL;
    return &e->stack[e->stack_len - 1];
}

static void pop_ctx(oyl_emitter *e) {
    if (e->stack_len > 0) e->stack_len--;
}

static bool in_any_flow(oyl_emitter *e) {
    for (int i = e->stack_len - 1; i >= 0; i--) {
        if (e->stack[i].type == EMIT_CTX_FLOW_MAP ||
            e->stack[i].type == EMIT_CTX_FLOW_SEQ)
            return true;
    }
    return false;
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

static bool has_break(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (oyl_is_break((uint8_t)s[i])) return true;
    }
    return false;
}

static bool needs_escape(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 && c != '\t') return true; /* control chars */
        if (c == 0x7F) return true;
    }
    return false;
}

/* Could this text be misread if emitted plain? Only syntax is considered
 * here; values that would resolve to another type (numbers, true, null)
 * are handled by choose_style. */
static bool needs_quoting(const char *s, size_t len, bool flow_ctx) {
    if (len == 0) return true;

    /* starts with an indicator. '-', '?' and ':' may start a plain scalar
     * when followed by a "safe" character: not a blank, and in flow
     * context not a flow indicator ("-123", "?x", ":x", "-foo") */
    uint8_t first = (uint8_t)s[0];
    if (oyl_is_indicator(first)) {
        if (!(first == '-' || first == '?' || first == ':')) return true;
        if (len < 2) return true;
        uint8_t next = (uint8_t)s[1];
        if (oyl_is_blank_or_break(next)) return true;
        if (flow_ctx && oyl_is_flow(next)) return true;
    }
    /* space or tab at start */
    if (first == ' ' || first == '\t') return true;

    /* U+FEFF at the start would read as a byte order mark and be dropped */
    if (len >= 3 && first == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
        return true;

    /* document markers */
    if (len >= 3 && (memcmp(s, "---", 3) == 0 || memcmp(s, "...", 3) == 0))
        return true;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        /* ": ", ":\t", " #", "\t#", and a trailing ':' (which would read
         * as a mapping value indicator) */
        if (c == ':' && (i + 1 == len || s[i + 1] == ' ' || s[i + 1] == '\t')) return true;
        if ((c == ' ' || c == '\t') && i + 1 < len && s[i + 1] == '#') return true;
        /* line breaks */
        if (oyl_is_break(c)) return true;
        /* flow indicators in flow context */
        if (flow_ctx && oyl_is_flow(c)) return true;
        /* trailing space/tab */
        if ((c == ' ' || c == '\t') && i + 1 == len) return true;
    }
    return false;
}

/* Can `s` be written as a literal block scalar and read back exactly?
 * Not with control characters (other than tab and line feed), and not
 * when the first non-empty line starts with a space: that would need an
 * indentation indicator. */
static bool literal_ok(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) return false;
    }
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
    /* Block scalars are written literal (folded values are already
     * folded, and literal keeps them exact), only in block context and
     * not as keys; otherwise double-quoted */
    bool block_ok = !flow_ctx && !is_key && literal_ok(val, len);
    if (requested == OYL_SCALAR_LITERAL || requested == OYL_SCALAR_FOLDED)
        return block_ok ? OYL_SCALAR_LITERAL : OYL_SCALAR_DOUBLE_QUOTED;
    /* single quotes fold line breaks; keep the value exact with escapes */
    if (requested == OYL_SCALAR_SINGLE_QUOTED)
        return has_break(val, len) ? OYL_SCALAR_DOUBLE_QUOTED : requested;
    if (requested == OYL_SCALAR_DOUBLE_QUOTED) return requested;

    /* PLAIN requested — auto-detect */
    if (len == 0 && !is_str_tag(tag))
        return OYL_SCALAR_PLAIN;              /* empty (null) node */
    if (needs_escape(val, len))
        return OYL_SCALAR_DOUBLE_QUOTED;      /* control characters */
    if (!needs_quoting(val, len, flow_ctx)) {
        if (is_str_tag(tag) && (is_yaml_keyword(val, len) || looks_like_number(val, len)))
            return OYL_SCALAR_DOUBLE_QUOTED;
        return OYL_SCALAR_PLAIN;
    }

    /* needs quoting — pick style */
    if (needs_escape(val, len))
        return OYL_SCALAR_DOUBLE_QUOTED;

    /* multiline in block context → literal */
    if (block_ok && has_break(val, len))
        return OYL_SCALAR_LITERAL;

    return OYL_SCALAR_DOUBLE_QUOTED;
}

/* ── Scalar emission ─────────────────────────────────────── */

static oyl_status emit_plain(oyl_emitter *e, const char *s, size_t len) {
    return buf_puts(e, s, len);
}

static oyl_status emit_single_quoted(oyl_emitter *e, const char *s, size_t len) {
    oyl_status st = buf_put(e, '\'');
    if (st != OYL_OK) return st;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            st = PUTS(e, "''");
            if (st != OYL_OK) return st;
        } else {
            st = buf_put(e, s[i]);
            if (st != OYL_OK) return st;
        }
    }
    return buf_put(e, '\'');
}

static oyl_status emit_double_quoted(oyl_emitter *e, const char *s, size_t len) {
    oyl_status st = buf_put(e, '"');
    if (st != OYL_OK) return st;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        switch (c) {
        case '\\': st = PUTS(e, "\\\\"); break;
        case '"':  st = PUTS(e, "\\\""); break;
        case '\n': st = PUTS(e, "\\n"); break;
        case '\r': st = PUTS(e, "\\r"); break;
        case '\t': st = PUTS(e, "\\t"); break;
        case '\0': st = PUTS(e, "\\0"); break;
        case '\a': st = PUTS(e, "\\a"); break;
        case '\b': st = PUTS(e, "\\b"); break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char esc[5];
                snprintf(esc, sizeof(esc), "\\x%02X", c);
                st = buf_puts(e, esc, 4);
            } else {
                st = buf_put(e, (char)c);
            }
            break;
        }
        if (st != OYL_OK) return st;
    }
    return buf_put(e, '"');
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
        size_t eol = pos;
        while (eol < body && s[eol] != '\n') eol++;
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

static oyl_status emit_scalar(oyl_emitter *e, const oyl_event *evt) {
    const char *val = evt->value.data;
    size_t vlen = evt->value.len;
    bool flow_ctx = in_any_flow(e) || e->opts.style != OYL_EMIT_BLOCK;

    emit_ctx *ctx = top_ctx(e);
    bool is_key = ctx && (ctx->type == EMIT_CTX_BLOCK_MAP || ctx->type == EMIT_CTX_FLOW_MAP) &&
                  ctx->expect_key;
    oyl_scalar_style style = choose_style(evt->scalar_style, val, vlen, flow_ctx,
                                          is_key, evt->tag);

    int indent = ctx ? ctx->indent + e->opts.indent : e->opts.indent;

    switch (style) {
    case OYL_SCALAR_PLAIN:
        if (vlen == 0) {
            /* empty node: nothing to write, except in a flow sequence
             * where "[a, ]" would drop the entry, so write ~ (null). Keep
             * the space after an anchor or tag ("&a : v", not "&a: v") */
            if (ctx && ctx->type == EMIT_CTX_FLOW_SEQ && !e->wrote_props)
                return buf_put(e, '~');
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
    return OYL_ERR_EMIT;
}

/* ── Property emission (anchor, tag) ─────────────────────── */

static oyl_status emit_anchor(oyl_emitter *e, oyl_str anchor) {
    if (!anchor.data || anchor.len == 0) return OYL_OK;
    oyl_status st = buf_put(e, '&');
    if (st != OYL_OK) return st;
    st = buf_puts(e, anchor.data, anchor.len);
    if (st != OYL_OK) return st;
    return buf_put(e, ' ');
}

/* URI characters allowed in a verbatim tag, besides alphanumerics */
static bool is_uri_char(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("-#;/?:@&=+$,_.!~*'()[]", c));
}

/* Characters allowed in a shorthand tag's suffix: URI characters other
 * than '!' and the flow indicators */
static bool is_tag_char(uint8_t c) {
    return is_uri_char(c) && c != '!' && !oyl_is_flow(c);
}

static bool all_tag_chars(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (!is_tag_char((uint8_t)s[i])) return false;
    return true;
}

/* Write a tag as "!!suffix" (standard tags), "!suffix" (local tags), or
 * verbatim "!<...>" with any other byte percent-encoded; the parser
 * decodes verbatim tags, so each form reads back as the same tag. */
static oyl_status emit_tag(oyl_emitter *e, oyl_str tag) {
    if (!tag.data || tag.len == 0) return OYL_OK;
    static const char prefix[] = "tag:yaml.org,2002:";
    static const size_t plen = sizeof(prefix) - 1;

    oyl_status st;
    if (tag.len > plen && memcmp(tag.data, prefix, plen) == 0 &&
        all_tag_chars(tag.data + plen, tag.len - plen)) {
        st = PUTS(e, "!!");
        if (st != OYL_OK) return st;
        st = buf_puts(e, tag.data + plen, tag.len - plen);
    } else if (tag.data[0] == '!' && all_tag_chars(tag.data + 1, tag.len - 1)) {
        st = buf_puts(e, tag.data, tag.len);
    } else {
        st = PUTS(e, "!<");
        for (size_t i = 0; i < tag.len && st == OYL_OK; i++) {
            uint8_t c = (uint8_t)tag.data[i];
            if (is_uri_char(c)) {
                st = buf_put(e, (char)c);
            } else {
                char esc[4];
                snprintf(esc, sizeof esc, "%%%02X", c);
                st = buf_puts(e, esc, 3);
            }
        }
        if (st != OYL_OK) return st;
        st = buf_put(e, '>');
    }
    if (st != OYL_OK) return st;
    return buf_put(e, ' ');
}

static oyl_status emit_props(oyl_emitter *e, const oyl_event *evt) {
    e->wrote_props = (evt->anchor.data && evt->anchor.len) || (evt->tag.data && evt->tag.len);
    oyl_status st = emit_anchor(e, evt->anchor);
    if (st != OYL_OK) return st;
    return emit_tag(e, evt->tag);
}

static bool has_props(const oyl_event *evt) {
    return (evt->anchor.data && evt->anchor.len) || (evt->tag.data && evt->tag.len);
}

/* Properties of a block collection go on the line that introduces it
 * ("key: &a !!map", "- &a"), and the collection itself starts on the next
 * line; a block collection can never start on its properties' line. */
static oyl_status emit_block_collection_props(oyl_emitter *e, const oyl_event *evt,
                                              bool after_key) {
    oyl_status st;
    if (!has_props(evt)) return OYL_OK;
    if (after_key) {
        st = buf_put(e, ' ');
        if (st != OYL_OK) return st;
    }
    st = emit_props(e, evt);
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
                                        const oyl_event *evt) {
    oyl_status st = buf_put(e, '?');
    if (st != OYL_OK) return st;
    st = emit_block_collection_props(e, evt, true);
    if (st != OYL_OK) return st;
    map->complex_key = true;
    e->after_seq_dash = false;
    return OYL_OK;
}

/* ── Pre-node prefix ─────────────────────────────────────── */

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
        } else {
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
    if (!ctx) return;

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

/* ── Main emit function ──────────────────────────────────── */

oyl_status oyl_emit(oyl_emitter *e, const oyl_event *evt) {
    oyl_status st;

    switch (evt->type) {
    case OYL_EVT_STREAM_START:
        e->first_doc = true;
        return OYL_OK;

    case OYL_EVT_STREAM_END:
        /* ensure trailing newline */
        if (e->len > 0 && e->buf[e->len - 1] != '\n')
            return buf_put(e, '\n');
        return OYL_OK;

    case OYL_EVT_DOC_START:
        if (!evt->implicit) {
            if (!e->first_doc && e->len > 0 && e->buf[e->len - 1] != '\n') {
                st = buf_put(e, '\n');
                if (st != OYL_OK) return st;
            }
            st = PUTS(e, "---");
            if (st != OYL_OK) return st;
        }
        e->doc_open = true;
        e->first_doc = false;
        return OYL_OK;

    case OYL_EVT_DOC_END:
        if (!evt->implicit) {
            if (e->len > 0 && e->buf[e->len - 1] != '\n') {
                st = buf_put(e, '\n');
                if (st != OYL_OK) return st;
            }
            st = PUTS(e, "...\n");
            if (st != OYL_OK) return st;
        }
        e->doc_open = false;
        return OYL_OK;

    case OYL_EVT_MAPPING_START: {
        /* inside a flow collection everything must be flow */
        bool use_flow = (e->opts.style != OYL_EMIT_BLOCK) || evt->flow || in_any_flow(e);
        emit_ctx *outer = top_ctx(e);
        bool map_value = !use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP &&
                         !outer->expect_key;

        st = emit_pre_node(e, !use_flow);
        if (st != OYL_OK) return st;

        if (!use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP && outer->expect_key) {
            st = emit_complex_key_open(e, outer, evt);
        } else if (use_flow) {
            st = emit_props(e, evt);
        } else {
            st = emit_block_collection_props(e, evt, map_value);
            /* a mapping with properties in a sequence can't share the
             * "- &a" line with its first key (the props would go to the key) */
            if (st == OYL_OK && e->after_seq_dash && has_props(evt))
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

    case OYL_EVT_MAPPING_END: {
        emit_ctx *ctx = top_ctx(e);
        if (!ctx) return OYL_ERR_EMIT;

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

    case OYL_EVT_SEQUENCE_START: {
        /* inside a flow collection everything must be flow */
        bool use_flow = (e->opts.style != OYL_EMIT_BLOCK) || evt->flow || in_any_flow(e);
        emit_ctx *outer = top_ctx(e);
        bool map_value = !use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP &&
                         !outer->expect_key;

        st = emit_pre_node(e, !use_flow);
        if (st != OYL_OK) return st;

        if (!use_flow && outer && outer->type == EMIT_CTX_BLOCK_MAP && outer->expect_key)
            st = emit_complex_key_open(e, outer, evt);
        else
            st = use_flow ? emit_props(e, evt)
                          : emit_block_collection_props(e, evt, map_value);
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
        bool saved_dash2 = e->after_seq_dash;
        e->stack_len--;
        advance_ctx(e);
        e->stack_len++;
        e->after_seq_dash = saved_dash2;
        return OYL_OK;
    }

    case OYL_EVT_SEQUENCE_END: {
        emit_ctx *ctx = top_ctx(e);
        if (!ctx) return OYL_ERR_EMIT;

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

    case OYL_EVT_SCALAR: {
        st = emit_pre_node(e, false);
        if (st != OYL_OK) return st;

        st = emit_props(e, evt);
        if (st != OYL_OK) return st;

        st = emit_scalar(e, evt);
        if (st != OYL_OK) return st;

        advance_ctx(e);
        return OYL_OK;
    }

    case OYL_EVT_ALIAS: {
        st = emit_pre_node(e, false);
        if (st != OYL_OK) return st;
        {
            emit_ctx *ctx = top_ctx(e);
            e->key_was_alias = ctx && (ctx->type == EMIT_CTX_BLOCK_MAP ||
                                       ctx->type == EMIT_CTX_FLOW_MAP) && ctx->expect_key;
        }

        st = buf_put(e, '*');
        if (st != OYL_OK) return st;
        st = buf_puts(e, evt->value.data, evt->value.len);
        if (st != OYL_OK) return st;

        advance_ctx(e);
        return OYL_OK;
    }

    default:
        return OYL_ERR_EMIT;
    }
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
    oyl_event evt = { .type = OYL_EVT_STREAM_START };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_stream_end(oyl_emitter *e) {
    oyl_event evt = { .type = OYL_EVT_STREAM_END };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_document_start(oyl_emitter *e, bool implicit) {
    oyl_event evt = { .type = OYL_EVT_DOC_START, .implicit = implicit };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_document_end(oyl_emitter *e, bool implicit) {
    oyl_event evt = { .type = OYL_EVT_DOC_END, .implicit = implicit };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_scalar(oyl_emitter *e, oyl_str value, oyl_scalar_style style,
                           oyl_str anchor, oyl_str tag) {
    oyl_event evt = { .type = OYL_EVT_SCALAR, .value = value, .scalar_style = style,
                      .anchor = anchor, .tag = tag };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_alias(oyl_emitter *e, oyl_str name) {
    oyl_event evt = { .type = OYL_EVT_ALIAS, .value = name };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_mapping_start(oyl_emitter *e, oyl_str anchor, oyl_str tag, bool flow) {
    oyl_event evt = { .type = OYL_EVT_MAPPING_START, .anchor = anchor, .tag = tag,
                      .flow = flow };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_mapping_end(oyl_emitter *e) {
    oyl_event evt = { .type = OYL_EVT_MAPPING_END };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_sequence_start(oyl_emitter *e, oyl_str anchor, oyl_str tag, bool flow) {
    oyl_event evt = { .type = OYL_EVT_SEQUENCE_START, .anchor = anchor, .tag = tag,
                      .flow = flow };
    return oyl_emit(e, &evt);
}

oyl_status oyl_emit_sequence_end(oyl_emitter *e) {
    oyl_event evt = { .type = OYL_EVT_SEQUENCE_END };
    return oyl_emit(e, &evt);
}

oyl_str oyl_emitter_output(oyl_emitter *e) {
    if (!e || !e->buf || e->len == 0) return OYL_STR_NULL;
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
