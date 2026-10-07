/*
 * oyl_scanner.c — YAML 1.2 tokenizer
 *
 * Hand-rolled, no recursion in the hot path.
 * SIMD-accelerated where it matters (plain scalar scanning, whitespace skip).
 *
 * Design:
 *   - Direct pointer into source buffer (no refill macro overhead)
 *   - Indent stack for block structure detection
 *   - Flow level counter for context switching
 */

#include "oyl_internal.h"
#include "oyl_chars.h"
#include "oyl_simd.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Indent stack ────────────────────────────────────────── */

#define INDENT_STACK_INIT 16

typedef struct {
    int  *data;
    int   len;
    int   cap;
} indent_stack;

/* ── Scanner state ───────────────────────────────────────── */

struct oyl_scanner {
    /* input */
    const char *buf;
    size_t      len;
    size_t      begin;  /* where the text starts: 3 past a UTF-8 BOM, else 0 */

    /* cursor */
    size_t      pos;
    size_t      line;
    size_t      col;

    /* state */
    int          flow_level;
    int          indent;
    indent_stack indents;
    bool         stream_started;
    bool         stream_ended;
    bool         implicit_key_allowed;
    bool         simple_key_allowed;
    bool         at_doc_start;

    /* pending tokens (block structure can require lookahead) */
    oyl_token    pending[4];
    int          pending_count;

    /* track last token's start column (0-based) for mapping indent detection */
    int          last_token_col;
    /* true if last token was a quoted scalar (for JSON-like key detection) */
    bool         last_was_quoted;

    /* arena for string duplication when needed */
    oyl_arena   *arena;

    /* implicit-key check (see check_implicit_key) */
    struct { oyl_mark open; bool is_map; int props_col; } *flows;
                                   /* open flow brackets, with the column of
                                    * props right before them (or -1) */
    int          flows_len, flows_cap;
    size_t       node_line;        /* start line of the last node that */
    size_t       node_end;         /* could be a key, and its end offset */
    bool         explicit_key;     /* a '?' awaits its ':' ... */
    int          explicit_depth;   /* ... at this flow depth */
    size_t       key_colon_line;   /* line of the last block implicit-key ':' */
    size_t       node_start;       /* start offset of the last noted node */
    int          node_col;         /* its 0-based start column */
    size_t       props_end;        /* end offset of the last anchor/tag run */
    int          props_col;        /* 0-based column where that run starts */
    size_t       indicator_end;    /* offset just past the last block - ? : */

    /* error context */
    char      error_msg[256];
    oyl_mark  error_mark;

    /* token handed out by the public oyl_scan_next() */
    oyl_token out_tok;
};


/* ── Error reporting ─────────────────────────────────────── */

#define SCAN_ERROR(s, msg) do { \
    snprintf((s)->error_msg, sizeof((s)->error_msg), "%s", (msg)); \
    (s)->error_mark = mark(s); \
    return OYL_ERR_SCAN; \
} while(0)

/* ── Helpers ─────────────────────────────────────────────── */

#define PEEK(s)       ((s)->pos < (s)->len ? (uint8_t)(s)->buf[(s)->pos] : 0)
#define PEEK_AT(s, n) ((s)->pos + (n) < (s)->len ? (uint8_t)(s)->buf[(s)->pos + (n)] : 0)
#define REMAINING(s)  ((s)->len - (s)->pos)
#define AT_END(s)     ((s)->pos >= (s)->len)
#define BUF_AT(s)     ((s)->buf + (s)->pos)

static uint8_t hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static oyl_mark mark(oyl_scanner *s) {
    return (oyl_mark){s->pos, s->line, s->col};
}

static void advance(oyl_scanner *s, size_t n) {
    for (size_t i = 0; i < n && s->pos < s->len; i++) {
        if (s->buf[s->pos] == '\n') {
            s->line++;
            s->col = 1;
        } else {
            s->col++;
        }
        s->pos++;
    }
}

/* Advance over `n` bytes known to contain no line break. */
static inline void advance_cols(oyl_scanner *s, size_t n) {
    s->pos += n;
    s->col += n;
}

static void skip_break(oyl_scanner *s) {
    if (s->pos >= s->len) return;
    if (s->buf[s->pos] == '\r') {
        s->pos++;
        if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
    } else if (s->buf[s->pos] == '\n') {
        s->pos++;
    }
    s->line++;
    s->col = 1;
}

/* A continuation line of a multi-line flow scalar or collection must be
 * indented (with spaces) more than the enclosing block. Called with the
 * position just past the line's leading blanks. A line that starts with
 * the construct's closing character (quote or bracket) is allowed at any
 * indentation, as other parsers do and as is common in practice. */
static bool continuation_indent_ok(const oyl_scanner *s, char closer) {
    if (s->pos >= s->len || s->buf[s->pos] == closer) return true;
    size_t line_start = s->pos - (s->col - 1);
    size_t k = line_start;
    while (k < s->pos && s->buf[k] == ' ') k++;
    return (long)(k - line_start) > (long)s->indent;
}

/* Skip whitespace, line breaks and comments. Returns an error message, or
 * NULL on success:
 *  - a '#' that would start a comment must be separated from the previous
 *    token by whitespace;
 *  - in block context a tab may not be part of a line's indentation: the
 *    spaces before it must already exceed the current block indent. */
ALWAYS_INLINE const char *skip_blanks_and_comments(oyl_scanner *s) {
    size_t line_start = (s->col == 1) ? s->pos : SIZE_MAX;
    for (;;) {
        /* skip spaces and tabs */
        size_t skip = oyl_skip_blanks(BUF_AT(s), REMAINING(s));
        advance_cols(s, skip);

        /* comment? skip to end of line */
        if (PEEK(s) == '#') {
            if (s->pos > s->begin && !oyl_is_blank_or_break((uint8_t)s->buf[s->pos - 1]))
                return "comment must be separated from other tokens by whitespace";
            size_t to_break = oyl_scan_to_break(BUF_AT(s), REMAINING(s));
            advance_cols(s, to_break);
        }

        /* line break? continue with the next line */
        if (oyl_is_break(PEEK(s))) {
            skip_break(s);
            line_start = s->pos;
            continue;
        }

        break;
    }

    /* first token on its line inside a flow collection: it must be indented
     * more than the enclosing block (closing brackets excepted) */
    if (line_start != SIZE_MAX && s->flow_level > 0 && !AT_END(s)) {
        char c = s->buf[s->pos];
        if (c != ']' && c != '}' && !continuation_indent_ok(s, 0))
            return "flow collection content must be indented more than the enclosing block";
    }

    /* first token on its line, in block context: check the indentation.
     * [line_start, pos) holds only blanks, so it contains a tab exactly
     * when its leading run of spaces stops short of pos. */
    if (line_start != SIZE_MAX && s->flow_level == 0 && !AT_END(s)) {
        size_t spaces = line_start;
        while (spaces < s->pos && s->buf[spaces] == ' ') spaces++;
        if (spaces < s->pos && (long)(spaces - line_start) <= (long)s->indent)
            return "tabs are not allowed as indentation";
    }
    return NULL;
}

/* ── Flow bracket stack (for the implicit-key check) ─────── */

static bool only_blanks_between(const oyl_scanner *s, size_t from, size_t to);

static bool flow_push(oyl_scanner *s, bool is_map) {
    if (s->flows_len >= s->flows_cap) {
        int nc = s->flows_cap ? s->flows_cap * 2 : 16;
        void *nf = realloc(s->flows, (size_t)nc * sizeof(*s->flows));
        if (!nf) return false;
        s->flows = nf;
        s->flows_cap = nc;
    }
    s->flows[s->flows_len].open = mark(s);
    s->flows[s->flows_len].is_map = is_map;
    s->flows[s->flows_len].props_col =
        only_blanks_between(s, s->props_end, s->pos) ? s->props_col : -1;
    s->flows_len++;
    return true;
}

/* Close a flow collection. Returns its opening mark, and restores the
 * record of props right before it (props inside it overwrote that). */
static oyl_mark flow_pop(oyl_scanner *s) {
    if (s->flows_len == 0) return mark(s);
    s->flows_len--;
    oyl_mark open = s->flows[s->flows_len].open;
    if (s->flows[s->flows_len].props_col >= 0) {
        s->props_col = s->flows[s->flows_len].props_col;
        s->props_end = open.offset;
    }
    return open;
}

/* Record a token that could be an implicit key: a flow scalar, an alias
 * or a flow collection (start line of the node, end offset of the token). */
static inline void note_node(oyl_scanner *s, oyl_mark start, size_t end) {
    s->node_line = start.line;
    s->node_start = start.offset;
    s->node_col = (int)start.col - 1;
    s->node_end = end;
}

/* Only blanks (on one line) between offsets `from` and `to`? */
static bool only_blanks_between(const oyl_scanner *s, size_t from, size_t to) {
    if (from == SIZE_MAX || from > to) return false;
    for (size_t i = from; i < to; i++)
        if (s->buf[i] != ' ' && s->buf[i] != '\t') return false;
    return true;
}

/* Record an anchor or tag token spanning [start, end): consecutive props
 * on a line form one run, which starts at the first of them. */
static void note_props(oyl_scanner *s, oyl_mark start, size_t end) {
    if (!only_blanks_between(s, s->props_end, start.offset))
        s->props_col = (int)start.col - 1;
    s->props_end = end;
}

/* Is the whitespace between the last block indicator (- ? :) and offset
 * `at` on one line and does it contain a tab? A block collection may not
 * be separated from its indicator by a tab ("-\t- a", "?\tkey: v"). */
static bool tab_after_indicator(const oyl_scanner *s, size_t at) {
    if (s->indicator_end == SIZE_MAX || s->indicator_end > at) return false;
    bool tab = false;
    for (size_t i = s->indicator_end; i < at; i++) {
        char c = s->buf[i];
        if (c == '\t') tab = true;
        else if (c != ' ') return false;
    }
    return tab;
}

/* Classify a ':' value indicator at the current position. */
enum { COLON_IMPLICIT, COLON_EXPLICIT, COLON_BAD_KEY };

/* An implicit key (one not introduced by '?') must lie on a single line:
 * if a node directly precedes the ':' on its line, it must also start on
 * that line. In a flow sequence the ':' must also be on the key's line
 * ("[ key\n : value ]" is invalid). Flow mappings are exempt (their keys
 * may span lines). */
static int classify_colon(oyl_scanner *s) {
    if (s->flows_len == 0) {
        /* block context: an explicit value (or empty key) ':' starts its
         * line; an implicit key's ':' never does */
        size_t k = s->pos;
        while (k > 0 && (s->buf[k - 1] == ' ' || s->buf[k - 1] == '\t')) k--;
        if (k == s->begin || s->buf[k - 1] == '\n' || s->buf[k - 1] == '\r') {
            s->explicit_key = false;
            return COLON_EXPLICIT;
        }
    } else if (s->explicit_key && s->explicit_depth == s->flows_len) {
        s->explicit_key = false; /* value of a '?' key: may span lines */
        return COLON_EXPLICIT;
    }
    if (s->node_end > s->pos || s->node_line >= s->line) return COLON_IMPLICIT;
    bool in_seq = false;
    if (s->flows_len > 0) {
        if (s->flows[s->flows_len - 1].is_map) return COLON_IMPLICIT;
        in_seq = true;
    }
    size_t j = s->pos;
    while (j > s->node_end) {
        char c = s->buf[j - 1];
        if (c == ' ' || c == '\t' || (in_seq && (c == '\n' || c == '\r'))) j--;
        else break;
    }
    return j == s->node_end ? COLON_BAD_KEY : COLON_IMPLICIT;
}

/* ── Indent management ───────────────────────────────────── */

static bool indent_push(oyl_scanner *s, int col) {
    indent_stack *st = &s->indents;
    if (st->len >= st->cap) {
        int new_cap = st->cap * 2;
        int *new_data = (int *)realloc(st->data, new_cap * sizeof(int));
        if (!new_data) return false;
        st->data = new_data;
        st->cap  = new_cap;
    }
    st->data[st->len++] = s->indent;
    s->indent = col;
    return true;
}

static void indent_pop(oyl_scanner *s) {
    if (s->indents.len > 0) {
        s->indent = s->indents.data[--s->indents.len];
    }
}

/* ── Token constructors ──────────────────────────────────── */

static oyl_token tok_simple(oyl_token_type type, oyl_mark start, oyl_mark end) {
    return (oyl_token){
        .type  = type,
        .value = OYL_STR_NULL,
        .start = start,
        .end   = end,
    };
}

static oyl_token tok_scalar(oyl_str value, oyl_scalar_style style,
                            oyl_mark start, oyl_mark end) {
    return (oyl_token){
        .type         = OYL_TOK_SCALAR,
        .value        = value,
        .scalar_style = style,
        .start        = start,
        .end          = end,
    };
}

static bool maybe_unroll_indents(oyl_scanner *s, int col) {
    while (s->indent > col) {
        indent_pop(s);
    }
    return true;
}

static bool is_doc_indicator_at(const char *buf, size_t pos, size_t len);

/* ── Scan specific token types ───────────────────────────── */

static oyl_status scan_plain_scalar(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);

    /* We use a two-phase approach:
     * Phase 1: scan single-line (zero-copy, fast)
     * Phase 2: if multiline, copy to arena buffer and continue */
    char *buf = NULL;
    size_t buf_cap = 0;
    size_t out = 0;
    const char *single_start = BUF_AT(s);
    size_t single_len = 0;
    bool in_buf = false; /* true once we've switched to arena buffer */

    /* macro to append data to the output (buffer or track single-line) */
    #define APPEND_RUN(src, len) do { \
        if (in_buf) { \
            if (out + (len) >= buf_cap) { \
                buf_cap = (out + (len)) * 2 + 64; \
                char *nb = oyl_arena_alloc(s->arena, buf_cap, 1); \
                if (!nb) return OYL_ERR_MEMORY; \
                memcpy(nb, buf, out); \
                buf = nb; \
            } \
            memcpy(buf + out, (src), (len)); \
            out += (len); \
        } else { \
            single_len += (len); \
        } \
    } while(0)

    #define APPEND_CHAR(ch) do { \
        if (in_buf) { \
            if (out + 1 >= buf_cap) { \
                buf_cap = (out + 1) * 2 + 64; \
                char *nb = oyl_arena_alloc(s->arena, buf_cap, 1); \
                if (!nb) return OYL_ERR_MEMORY; \
                memcpy(nb, buf, out); \
                buf = nb; \
            } \
            buf[out++] = (ch); \
        } else { \
            single_len++; \
        } \
    } while(0)

    #define SWITCH_TO_BUF() do { \
        if (!in_buf) { \
            buf_cap = single_len * 2 + 256; /* APPEND_* grow it */ \
            buf = oyl_arena_alloc(s->arena, buf_cap, 1); \
            if (!buf) return OYL_ERR_MEMORY; \
            memcpy(buf, single_start, single_len); \
            out = single_len; \
            in_buf = true; \
        } \
    } while(0)

    #define TRIM_TRAILING() do { \
        if (in_buf) { \
            while (out > 0 && (buf[out-1] == ' ' || buf[out-1] == '\t')) out--; \
        } else { \
            while (single_len > 0 && (single_start[single_len-1] == ' ' || single_start[single_len-1] == '\t')) single_len--; \
        } \
    } while(0)

    for (;;) {
        /* SIMD fast scan over "boring" bytes */
        size_t run = oyl_scan_plain_scalar(BUF_AT(s), REMAINING(s));
        if (run > 0) {
            if (in_buf) {
                APPEND_RUN(BUF_AT(s), run);
            } else {
                single_len += run;
            }
            s->pos += run;
            s->col += run;
        }

        if (AT_END(s)) break;

        uint8_t c = PEEK(s);

        /* ':' is only value indicator if followed by whitespace (or EOF in flow) */
        if (c == ':') {
            uint8_t next = PEEK_AT(s, 1);
            if (!oyl_is_blank_or_break(next) && next != 0
                && !(s->flow_level > 0 && oyl_is_flow(next))) {
                if (in_buf) { APPEND_CHAR(':'); } else { single_len++; }
                advance(s, 1);
                continue;
            }
            break;
        }

        /* '#' is only comment if preceded by whitespace */
        if (c == '#') {
            size_t cur_len = in_buf ? out : single_len;
            const char *cur_data = in_buf ? buf : single_start;
            if (cur_len > 0 && (cur_data[cur_len-1] == ' ' || cur_data[cur_len-1] == '\t')) {
                TRIM_TRAILING();
                break;
            }
            if (cur_len == 0) break;
            if (in_buf) { APPEND_CHAR('#'); } else { single_len++; }
            advance(s, 1);
            continue;
        }

        /* flow indicators in flow context end the scalar */
        if (s->flow_level > 0 && oyl_is_flow(c)) break;

        /* whitespace — potential end or mid-line */
        if (oyl_is_blank(c)) {
            size_t blank_run = oyl_skip_blanks(BUF_AT(s), REMAINING(s));
            const char *blank_start = BUF_AT(s);
            s->pos += blank_run;
            s->col += blank_run;

            if (AT_END(s)) break;
            if (oyl_is_break(PEEK(s))) {
                /* trailing blanks before break — don't include them,
                 * but continue loop so break handler can check for continuation */
                continue;
            }

            /* blanks mid-line */
            if (in_buf) {
                APPEND_RUN(blank_start, blank_run);
            } else {
                single_len += blank_run;
            }
            continue;
        }

        if (oyl_is_break(c)) {
            /* check if next line continues this scalar */
            size_t save = s->pos;
            size_t save_line = s->line;
            size_t save_col = s->col;

            int break_count = 0;
            while (!AT_END(s) && oyl_is_break(PEEK(s))) {
                skip_break(s);
                break_count++;
                size_t blanks = oyl_skip_blanks(BUF_AT(s), REMAINING(s));
                s->pos += blanks;
                s->col += blanks;
                if (!AT_END(s) && !oyl_is_break(PEEK(s))) break;
            }

            if (AT_END(s)) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }

            int next_indent = (int)s->col - 1;
            int min_indent = s->indent + 1;
            if (min_indent < 0) min_indent = 0;

            if (next_indent < min_indent) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }

            /* check for indicators that end the scalar */
            uint8_t nc = PEEK(s);
            /* - and ? only start block entries at indent levels where
             * a new block collection could begin (at or below current indent).
             * On deeper continuation lines, they're just content.
             * At top level (indent < 0), always terminate. */
            if ((nc == '-' || nc == '?') &&
                (s->indent < 0 || next_indent <= s->indent)) {
                uint8_t after = PEEK_AT(s, 1);
                if (oyl_is_blank_or_break(after) || after == 0) {
                    s->pos = save; s->line = save_line; s->col = save_col;
                    break;
                }
            }
            /* : followed by whitespace always ends (it introduces a mapping value) */
            if (nc == ':') {
                uint8_t after = PEEK_AT(s, 1);
                if (oyl_is_blank_or_break(after) || after == 0) {
                    s->pos = save; s->line = save_line; s->col = save_col;
                    break;
                }
            }
            if (nc == '#') {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }
            /* & and * could be anchors/aliases — they terminate the
             * scalar only at indent levels where a new node could start
             * (at or below current indent). On deeper continuation lines
             * they're just plain text. At top level, terminate at col 0. */
            if ((nc == '&' || nc == '*') &&
                next_indent <= s->indent + (s->indent < 0 ? 1 : 0)) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }
            if (s->col == 1 && REMAINING(s) >= 3) {
                if ((nc == '-' && PEEK_AT(s,1) == '-' && PEEK_AT(s,2) == '-') ||
                    (nc == '.' && PEEK_AT(s,1) == '.' && PEEK_AT(s,2) == '.')) {
                    if (REMAINING(s) == 3 || oyl_is_blank_or_break(PEEK_AT(s,3))) {
                        s->pos = save; s->line = save_line; s->col = save_col;
                        break;
                    }
                }
            }

            /* it IS a continuation — switch to buffer mode */
            TRIM_TRAILING();
            SWITCH_TO_BUF();

            /* fold breaks: 1 break → space, N breaks → (N-1) newlines */
            if (break_count == 1) {
                APPEND_CHAR(' ');
            } else {
                for (int i = 1; i < break_count; i++) {
                    APPEND_CHAR('\n');
                }
            }
            continue;
        }

        /* control characters are not allowed in YAML text */
        if (c < 0x20 || c == 0x7F)
            SCAN_ERROR(s, "control character in plain scalar");

        /* anything else is part of the scalar */
        if (in_buf) {
            APPEND_CHAR(s->buf[s->pos]);
        } else {
            single_len++;
        }
        advance(s, 1);
    }

    TRIM_TRAILING();

    oyl_mark end = mark(s);
    s->last_token_col = (int)start.col - 1;  /* 0-based col of this scalar */
    if (in_buf) {
        buf[out] = '\0';
        note_node(s, start, end.offset);
        *tok = tok_scalar((oyl_str){buf, out}, OYL_SCALAR_PLAIN, start, end);
    } else {
        note_node(s, start, end.offset);
        *tok = tok_scalar((oyl_str){single_start, single_len}, OYL_SCALAR_PLAIN, start, end);
    }
    return OYL_OK;

    #undef APPEND_RUN
    #undef APPEND_CHAR
    #undef SWITCH_TO_BUF
    #undef TRIM_TRAILING
}

static oyl_status scan_single_quoted(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);
    advance(s, 1); /* skip opening ' */

    /* fast path: no '' escapes or line breaks → zero-copy slice of the input */
    {
        size_t j = s->pos + oyl_find_any4_short(BUF_AT(s), REMAINING(s), '\'', '\n', '\r', '\'');
        if (j < s->len && s->buf[j] == '\'' &&
            !(j + 1 < s->len && s->buf[j + 1] == '\'')) {
            oyl_str val = {s->buf + s->pos, j - s->pos};
            s->col += j + 1 - s->pos;
            s->pos = j + 1;
            s->last_token_col = (int)start.col - 1;
            s->last_was_quoted = true;
            note_node(s, start, s->pos);
            *tok = tok_scalar(val, OYL_SCALAR_SINGLE_QUOTED, start, mark(s));
            return OYL_OK;
        }
    }

    /* Allocate buffer for the result (handles escapes and line folding) */
    size_t buf_cap = 64;
    char *buf = oyl_arena_alloc(s->arena, buf_cap, 1);
    if (!buf) return OYL_ERR_MEMORY;
    size_t out = 0;

    #define SQ_ENSURE(n) do { \
        if (out + (n) >= buf_cap) { \
            buf_cap = (out + (n)) * 2 + 64; \
            char *nb = oyl_arena_alloc(s->arena, buf_cap, 1); \
            if (!nb) return OYL_ERR_MEMORY; \
            memcpy(nb, buf, out); \
            buf = nb; \
        } \
    } while(0)

    while (!AT_END(s)) {
        if (PEEK(s) == '\'') {
            if (s->pos + 1 < s->len && s->buf[s->pos + 1] == '\'') {
                /* escaped single quote '' → ' */
                SQ_ENSURE(1);
                buf[out++] = '\'';
                advance(s, 2);
                continue;
            }
            break; /* closing quote */
        }
        if (oyl_is_break(PEEK(s))) {
            /* line folding: trim trailing blanks, fold breaks */
            while (out > 0 && (buf[out-1] == ' ' || buf[out-1] == '\t')) out--;
            int break_count = 0;
            while (!AT_END(s) && oyl_is_break(PEEK(s))) {
                skip_break(s);
                break_count++;
                while (!AT_END(s) && oyl_is_blank(PEEK(s))) advance(s, 1);
                if (!AT_END(s) && !oyl_is_break(PEEK(s))) break;
            }
            /* document indicators at start of line in multi-line quoted scalar → error */
            if (s->col == 1 && is_doc_indicator_at(s->buf, s->pos, s->len))
                SCAN_ERROR(s, "document indicator in single-quoted scalar");
            if (!continuation_indent_ok(s, '\''))
                SCAN_ERROR(s, "continuation line of a quoted scalar must be indented");
            SQ_ENSURE(break_count);
            if (break_count == 1) {
                buf[out++] = ' ';
            } else {
                for (int i = 1; i < break_count; i++)
                    buf[out++] = '\n';
            }
        } else {
            /* a run of ordinary bytes, up to the next quote or break */
            size_t n = oyl_find_any4(BUF_AT(s), REMAINING(s), '\'', '\n', '\r', '\'');
            SQ_ENSURE(n);
            memcpy(buf + out, BUF_AT(s), n);
            out += n;
            advance_cols(s, n);
        }
    }

    if (PEEK(s) != '\'') SCAN_ERROR(s, "unterminated single-quoted scalar");
    advance(s, 1); /* skip closing ' */

    buf[out] = '\0';
    s->last_token_col = (int)start.col - 1;
    s->last_was_quoted = true;
    note_node(s, start, s->pos);
    *tok = tok_scalar((oyl_str){buf, out}, OYL_SCALAR_SINGLE_QUOTED, start, mark(s));
    return OYL_OK;

    #undef SQ_ENSURE
}

static oyl_status scan_double_quoted(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);
    advance(s, 1); /* skip opening " */

    /* fast path: no escapes or line breaks → zero-copy slice of the input */
    {
        size_t j = s->pos + oyl_find_any4_short(BUF_AT(s), REMAINING(s), '"', '\\', '\n', '\r');
        if (j < s->len && s->buf[j] == '"') {
            oyl_str val = {s->buf + s->pos, j - s->pos};
            s->col += j + 1 - s->pos;
            s->pos = j + 1;
            s->last_token_col = (int)start.col - 1;
            s->last_was_quoted = true;
            note_node(s, start, s->pos);
            *tok = tok_scalar(val, OYL_SCALAR_DOUBLE_QUOTED, start, mark(s));
            return OYL_OK;
        }
    }

    /* double-quoted always needs processing for escape sequences */
    /* pre-scan for length */
    size_t max_len = 0;
    size_t scan = s->pos;
    while (scan < s->len) {
        size_t n = oyl_find_any4(s->buf + scan, s->len - scan, '"', '\\', '"', '"');
        scan += n;
        max_len += n;
        if (scan >= s->len || s->buf[scan] == '"') break;
        /* a backslash and the escaped byte: one output byte (two for \N, \_) */
        if (scan + 1 < s->len && (s->buf[scan+1] == 'N' || s->buf[scan+1] == '_'))
            max_len++; /* 2-byte UTF-8 output */
        scan += 2;
        max_len++;
    }

    char *buf = oyl_arena_alloc(s->arena, max_len + 1, 1);
    if (!buf) return OYL_ERR_MEMORY;

    size_t out = 0;
    size_t content_end = 0; /* tracks end of non-literal-whitespace content for trimming */
    while (!AT_END(s) && PEEK(s) != '"') {
        if (PEEK(s) == '\\') {
            advance(s, 1);
            if (AT_END(s)) SCAN_ERROR(s, "incomplete escape at end of input");

            uint8_t esc = PEEK(s);
            advance(s, 1);
            switch (esc) {
                case '0':  buf[out++] = '\0'; break;
                case 'a':  buf[out++] = '\a'; break;
                case 'b':  buf[out++] = '\b'; break;
                case 't':  buf[out++] = '\t'; break;
                case '\t': buf[out++] = '\t'; break;
                case 'n':  buf[out++] = '\n'; break;
                case 'v':  buf[out++] = '\v'; break;
                case 'f':  buf[out++] = '\f'; break;
                case 'r':  buf[out++] = '\r'; break;
                case 'e':  buf[out++] = '\x1B'; break;
                case ' ':  buf[out++] = ' '; break;
                case '"':  buf[out++] = '"'; break;
                case '/':  buf[out++] = '/'; break;
                case '\\': buf[out++] = '\\'; break;
                case 'N':  /* NEL U+0085 — encode as UTF-8 */
                    buf[out++] = (char)0xC2;
                    buf[out++] = (char)0x85;
                    break;
                case '_':  /* NBSP U+00A0 */
                    buf[out++] = (char)0xC2;
                    buf[out++] = (char)0xA0;
                    break;
                case 'x': { /* \xNN */
                    if (REMAINING(s) < 2) SCAN_ERROR(s, "incomplete \\x escape sequence");
                    uint8_t hi = PEEK(s), lo = PEEK_AT(s, 1);
                    if (!oyl_is_hex(hi) || !oyl_is_hex(lo)) SCAN_ERROR(s, "invalid hex digit in \\x escape");
                    uint8_t byte = (hex_digit(hi) << 4) | hex_digit(lo);
                    advance(s, 2);
                    buf[out++] = (char)byte;
                    break;
                }
                case 'u': /* \uNNNN */
                case 'U': { /* \UNNNNNNNN */
                    int ndigits = (esc == 'u') ? 4 : 8;
                    if ((int)REMAINING(s) < ndigits) SCAN_ERROR(s, "incomplete unicode escape sequence");
                    uint32_t cp = 0;
                    for (int i = 0; i < ndigits; i++) {
                        uint8_t c = PEEK_AT(s, i);
                        if (!oyl_is_hex(c)) SCAN_ERROR(s, "invalid hex digit in unicode escape");
                        cp = (cp << 4) | hex_digit(c);
                    }
                    advance(s, ndigits);
                    if (cp > 0x10FFFF) SCAN_ERROR(s, "unicode codepoint exceeds U+10FFFF");
                    if (cp >= 0xD800 && cp <= 0xDFFF) SCAN_ERROR(s, "surrogate pair in unicode escape");
                    /* encode as UTF-8 */
                    if (cp <= 0x7F) {
                        buf[out++] = (char)cp;
                    } else if (cp <= 0x7FF) {
                        buf[out++] = (char)(0xC0 | (cp >> 6));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp <= 0xFFFF) {
                        buf[out++] = (char)(0xE0 | (cp >> 12));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        buf[out++] = (char)(0xF0 | (cp >> 18));
                        buf[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[out++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                case '\n': /* escaped newline — line folding */
                case '\r':
                    /* skip the break and any leading whitespace on next line */
                    if (esc == '\r' && PEEK(s) == '\n') advance(s, 1);
                    while (!AT_END(s) && oyl_is_blank(PEEK(s))) advance(s, 1);
                    if (!AT_END(s) && !oyl_is_break(PEEK(s)) &&
                        !continuation_indent_ok(s, '"'))
                        SCAN_ERROR(s, "continuation line of a quoted scalar must be indented");
                    content_end = out; /* escaped newline doesn't affect trim */
                    continue; /* skip content_end update below */
                default:
                    SCAN_ERROR(s, "invalid escape sequence");
            }
            /* all escape sequences produce content (not trimmable whitespace) */
            content_end = out;
        } else if (oyl_is_break(PEEK(s))) {
            /* line folding: 1 break→space, empty lines→\n each */
            /* trim trailing literal whitespace (escapes are content) */
            out = content_end;
            int break_count = 0;
            while (!AT_END(s) && oyl_is_break(PEEK(s))) {
                skip_break(s);
                break_count++;
                while (!AT_END(s) && oyl_is_blank(PEEK(s))) advance(s, 1);
                if (!AT_END(s) && !oyl_is_break(PEEK(s))) break;
            }
            /* document indicators at start of line in multi-line quoted scalar → error */
            if (s->col == 1 && is_doc_indicator_at(s->buf, s->pos, s->len))
                SCAN_ERROR(s, "document indicator in double-quoted scalar");
            if (!continuation_indent_ok(s, '"'))
                SCAN_ERROR(s, "continuation line of a quoted scalar must be indented");
            if (break_count == 1) {
                buf[out++] = ' ';
            } else {
                for (int i = 1; i < break_count; i++)
                    buf[out++] = '\n';
            }
        } else {
            /* a run of ordinary bytes, up to the next quote, escape or break */
            const char *run = BUF_AT(s);
            size_t n = oyl_find_any4(run, REMAINING(s), '"', '\\', '\n', '\r');
            memcpy(buf + out, run, n);
            size_t k = n;               /* trailing blanks don't count as content */
            while (k > 0 && oyl_is_blank((uint8_t)run[k - 1])) k--;
            if (k > 0) content_end = out + k;
            out += n;
            advance_cols(s, n);
        }
    }

    if (PEEK(s) != '"') SCAN_ERROR(s, "unterminated double-quoted scalar");
    advance(s, 1);

    buf[out] = '\0';
    s->last_token_col = (int)start.col - 1;
    s->last_was_quoted = true;
    note_node(s, start, s->pos);
    *tok = tok_scalar((oyl_str){buf, out}, OYL_SCALAR_DOUBLE_QUOTED, start, mark(s));
    return OYL_OK;
}

static oyl_status scan_tag(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);
    const char *tag_start = BUF_AT(s);
    advance(s, 1); /* skip ! */

    if (PEEK(s) == '!') {
        advance(s, 1); /* !! secondary tag */
    } else if (PEEK(s) == '<') {
        /* verbatim tag !<...>: at least one character, no whitespace,
         * closed by '>' */
        advance(s, 1);
        size_t uri_start = s->pos;
        while (!AT_END(s) && PEEK(s) != '>' && !oyl_is_blank_or_break(PEEK(s))) {
            if (PEEK(s) < 0x20 || PEEK(s) == 0x7F) SCAN_ERROR(s, "control character in tag");
            advance(s, 1);
        }
        if (PEEK(s) != '>' || s->pos == uri_start)
            SCAN_ERROR(s, "invalid verbatim tag");
        advance(s, 1);
        size_t len = BUF_AT(s) - tag_start;
        s->last_token_col = (int)start.col - 1;
        note_props(s, start, s->pos);
        *tok = (oyl_token){
            .type  = OYL_TOK_TAG,
            .value = {tag_start, len},
            .start = start,
            .end   = mark(s),
        };
        return OYL_OK;
    }

    /* consume tag characters */
    while (!AT_END(s) && !oyl_is_blank_or_break(PEEK(s))
           && !oyl_is_flow(PEEK(s))) {
        if (PEEK(s) < 0x20 || PEEK(s) == 0x7F) SCAN_ERROR(s, "control character in tag");
        advance(s, 1);
    }

    size_t len = BUF_AT(s) - tag_start;
    s->last_token_col = (int)start.col - 1;
    note_props(s, start, s->pos);
    *tok = (oyl_token){
        .type  = OYL_TOK_TAG,
        .value = {tag_start, len},
        .start = start,
        .end   = mark(s),
    };
    return OYL_OK;
}

static oyl_status scan_anchor_or_alias(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);
    bool is_anchor = (PEEK(s) == '&');
    advance(s, 1); /* skip & or * */

    const char *name_start = BUF_AT(s);
    while (!AT_END(s)) {
        uint8_t ch = PEEK(s);
        /* anchor name: any non-whitespace, non-flow indicator character */
        if (oyl_is_blank_or_break(ch) || ch == 0) break;
        if (ch == ',' || ch == '[' || ch == ']' || ch == '{' || ch == '}') break;
        if (ch < 0x20 || ch == 0x7F)
            SCAN_ERROR(s, "control character in anchor or alias name");
        advance(s, 1);
    }
    size_t name_len = BUF_AT(s) - name_start;
    if (name_len == 0) SCAN_ERROR(s, "empty anchor or alias name");

    s->last_token_col = (int)start.col - 1;
    if (!is_anchor) note_node(s, start, s->pos);
    else note_props(s, start, s->pos);
    *tok = (oyl_token){
        .type  = is_anchor ? OYL_TOK_ANCHOR : OYL_TOK_ALIAS,
        .value = {name_start, name_len},
        .start = start,
        .end   = mark(s),
    };
    return OYL_OK;
}

/* Check for --- or ... at line start */
/* check for document indicator (--- or ...) at a buffer position */
static bool is_doc_indicator_at(const char *buf, size_t pos, size_t len) {
    if (pos + 3 > len) return false;
    char c = buf[pos];
    if (c != '-' && c != '.') return false;
    if (buf[pos + 1] != c || buf[pos + 2] != c) return false;
    if (pos + 3 == len) return true;
    return oyl_is_blank_or_break((uint8_t)buf[pos + 3]);
}

static bool at_doc_indicator(oyl_scanner *s, char ch) {
    if (s->col != 1) return false;
    if (REMAINING(s) < 3) return false;
    return s->buf[s->pos]     == ch
        && s->buf[s->pos + 1] == ch
        && s->buf[s->pos + 2] == ch
        && (REMAINING(s) == 3 || oyl_is_blank_or_break(PEEK_AT(s, 3)));
}

/* ── Main scan function ──────────────────────────────────── */

oyl_scanner *oyl_scanner_new(const char *input, size_t len, oyl_arena *a) {
    oyl_scanner *s = (oyl_scanner *)malloc(sizeof(oyl_scanner));
    if (!s) return NULL;

    *s = (oyl_scanner){
        .buf   = input,
        .len   = len,
        .pos   = 0,
        .line  = 1,
        .col   = 1,
        .flow_level = 0,
        .indent     = -1,
        .stream_started = false,
        .stream_ended   = false,
        .implicit_key_allowed = true,
        .simple_key_allowed   = true,
        .at_doc_start         = true,
        .pending_count        = 0,
        .last_token_col       = 0,
        .node_end             = SIZE_MAX, /* no node seen yet */
        .indicator_end        = SIZE_MAX,
        .props_end            = SIZE_MAX,
        .arena = a,
    };

    /* skip a UTF-8 byte order mark; offsets stay relative to the caller's
     * buffer, and the text still starts at line 1, column 1 */
    if (len >= 3 && (uint8_t)input[0] == 0xEF && (uint8_t)input[1] == 0xBB &&
        (uint8_t)input[2] == 0xBF) {
        s->begin = 3;
        s->pos = 3;
    }

    s->indents.cap  = INDENT_STACK_INIT;
    s->indents.len  = 0;
    s->indents.data = (int *)malloc(INDENT_STACK_INIT * sizeof(int));
    if (!s->indents.data) { free(s); return NULL; }

    return s;
}


oyl_status oyl_scan_next(oyl_scanner *s, const oyl_token **tok) {
    oyl_status st = oyl_scan_token(s, &s->out_tok);
    *tok = st == OYL_OK ? &s->out_tok : NULL;
    return st;
}

oyl_status oyl_scan_token(oyl_scanner *s, oyl_token *tok) {
    /* drain pending tokens first */
    if (s->pending_count > 0) {
        *tok = s->pending[0];
        memmove(&s->pending[0], &s->pending[1],
                (--s->pending_count) * sizeof(oyl_token));
        return OYL_OK;
    }

    /* stream start */
    if (!s->stream_started) {
        s->stream_started = true;
        *tok = tok_simple(OYL_TOK_STREAM_START, mark(s), mark(s));
        return OYL_OK;
    }

    /* skip whitespace and comments */
    {
        const char *err = skip_blanks_and_comments(s);
        if (err) SCAN_ERROR(s, err);
    }

    /* stream end */
    if (AT_END(s)) {
        if (!s->stream_ended) {
            s->stream_ended = true;
            /* unwind all indents */
            maybe_unroll_indents(s, -1);
            *tok = tok_simple(OYL_TOK_STREAM_END, mark(s), mark(s));
            return OYL_OK;
        }
        *tok = tok_simple(OYL_TOK_NONE, mark(s), mark(s));
        return OYL_OK;
    }

    /* unroll indent stack to current column (block context only).
     * This ensures s->indent reflects the actual nesting depth,
     * which is needed for plain scalar continuation checks. */
    if (s->flow_level == 0) {
        int cur_col = (int)s->col - 1;  /* 0-based */
        maybe_unroll_indents(s, cur_col);
    }

    oyl_mark start = mark(s);
    uint8_t c = PEEK(s);

    /* document indicators at column 1 */
    if (at_doc_indicator(s, '-')) {
        maybe_unroll_indents(s, -1);
        advance(s, 3);
        *tok = tok_simple(OYL_TOK_DOC_START, start, mark(s));
        return OYL_OK;
    }
    if (at_doc_indicator(s, '.')) {
        maybe_unroll_indents(s, -1);
        advance(s, 3);
        /* only a comment may follow '...' on its line */
        size_t k = s->pos;
        while (k < s->len && (s->buf[k] == ' ' || s->buf[k] == '\t')) k++;
        if (k < s->len && s->buf[k] != '#' && !oyl_is_break((uint8_t)s->buf[k]))
            SCAN_ERROR(s, "content after document end marker '...'");
        *tok = tok_simple(OYL_TOK_DOC_END, start, mark(s));
        return OYL_OK;
    }

    /* directive lines: % at column 1 in block context → one token holding
     * the whole line; the parser validates it */
    if (c == '%' && s->col == 1 && s->flow_level == 0) {
        const char *line_start = BUF_AT(s);
        size_t len = oyl_scan_to_break(line_start, REMAINING(s));
        advance_cols(s, len);
        *tok = (oyl_token){
            .type  = OYL_TOK_DIRECTIVE,
            .value = {line_start, len},
            .start = start,
            .end   = mark(s),
        };
        return OYL_OK;
    }

    /* flow indicators */
    switch (c) {
    case '[':
        s->last_was_quoted = false;
        if (!flow_push(s, false)) return OYL_ERR_MEMORY;
        s->flow_level++;
        advance(s, 1);
        *tok = tok_simple(OYL_TOK_FLOW_SEQ_START, start, mark(s));
        return OYL_OK;
    case ']': {
        oyl_mark open = flow_pop(s);
        if (s->flow_level > 0) s->flow_level--;
        advance(s, 1);
        note_node(s, open, s->pos);
        s->last_was_quoted = true; /* allow ]:value like "key":value */
        *tok = tok_simple(OYL_TOK_FLOW_SEQ_END, start, mark(s));
        return OYL_OK;
    }
    case '{':
        s->last_was_quoted = false;
        if (!flow_push(s, true)) return OYL_ERR_MEMORY;
        s->flow_level++;
        advance(s, 1);
        *tok = tok_simple(OYL_TOK_FLOW_MAP_START, start, mark(s));
        return OYL_OK;
    case '}': {
        oyl_mark open = flow_pop(s);
        if (s->flow_level > 0) s->flow_level--;
        advance(s, 1);
        note_node(s, open, s->pos);
        s->last_was_quoted = true; /* allow }:value like "key":value */
        *tok = tok_simple(OYL_TOK_FLOW_MAP_END, start, mark(s));
        return OYL_OK;
    }
    case ',':
        s->last_was_quoted = false;   /* ":" after "," is not adjacent to a key */
        advance(s, 1);
        *tok = tok_simple(OYL_TOK_FLOW_ENTRY, start, mark(s));
        return OYL_OK;
    }

    /* block sequence entry: - followed by whitespace */
    if (c == '-' && s->flow_level == 0) {
        uint8_t next = PEEK_AT(s, 1);
        if (oyl_is_blank_or_break(next) || next == 0) {
            /* "key: - a" — a block sequence can't start on a key's line */
            if (s->key_colon_line == s->line)
                SCAN_ERROR(s, "block sequence entries are not allowed on the same line as a mapping key");
            if (tab_after_indicator(s, s->pos))
                SCAN_ERROR(s, "tabs are not allowed before a nested block collection");
            int col = (int)s->col - 1;
            if (col > s->indent) {
                indent_push(s, col);
            }
            advance(s, 1);
            s->indicator_end = s->pos;
            *tok = tok_simple(OYL_TOK_BLOCK_SEQ_ENTRY, start, mark(s));
            return OYL_OK;
        }
    }

    /* explicit mapping key: ? followed by whitespace (both block and flow) */
    if (c == '?' && s->flow_level == 0) {
        uint8_t next = PEEK_AT(s, 1);
        if (oyl_is_blank_or_break(next) || next == 0) {
            /* "key: ? x" — a block mapping can't start on a key's line */
            if (s->key_colon_line == s->line)
                SCAN_ERROR(s, "mapping values are not allowed on the same line as another key");
            if (tab_after_indicator(s, s->pos))
                SCAN_ERROR(s, "tabs are not allowed before a nested block collection");
            int qcol = (int)s->col - 1;
            if (qcol > s->indent) {
                indent_push(s, qcol);
            }
            advance(s, 1);
            s->indicator_end = s->pos;
            s->explicit_key = true;
            s->explicit_depth = s->flows_len;
            *tok = tok_simple(OYL_TOK_BLOCK_MAP_KEY, start, mark(s));
            return OYL_OK;
        }
    }
    /* ? in flow context */
    if (c == '?' && s->flow_level > 0) {
        uint8_t next = PEEK_AT(s, 1);
        if (oyl_is_blank_or_break(next) || next == 0) {
            advance(s, 1);
            s->explicit_key = true;
            s->explicit_depth = s->flows_len;
            *tok = tok_simple(OYL_TOK_BLOCK_MAP_KEY, start, mark(s));
            return OYL_OK;
        }
    }

    /* block mapping value: : followed by whitespace */
    if (c == ':') {
        uint8_t next = PEEK_AT(s, 1);
        if (s->flow_level == 0 && (oyl_is_blank_or_break(next) || next == 0)) {
            int colon_col = (int)s->col - 1;
            /* when a node (scalar, alias, or flow collection) directly
             * precedes the ':', the key starts where it does, or at props
             * right before it on its line ("&k key:", "!!map {a: b}:");
             * an empty key starts at its props ("&k :") or the ':' itself
             * ("- : v") */
            int key_col = colon_col;
            if (only_blanks_between(s, s->node_end, s->pos)) {
                key_col = s->node_col;
                if (only_blanks_between(s, s->props_end, s->node_start))
                    key_col = s->props_col;
            } else if (only_blanks_between(s, s->props_end, s->pos)) {
                key_col = s->props_col;
            }
            /* use minimum of key and colon column — handles explicit key
             * (? key\n: val) where colon_col is the mapping indent */
            int map_col = key_col < colon_col ? key_col : colon_col;
            int kind = classify_colon(s);
            if (kind == COLON_BAD_KEY)
                SCAN_ERROR(s, "implicit key must be on a single line");
            if (kind == COLON_IMPLICIT) {
                /* "a: b: c" — a block mapping can't start on the line of
                 * another implicit key */
                if (s->key_colon_line == s->line)
                    SCAN_ERROR(s, "mapping values are not allowed on the same line as another key");
                if (tab_after_indicator(s, s->node_start))
                    SCAN_ERROR(s, "tabs are not allowed before a nested block collection");
                s->key_colon_line = s->line;
            } else {
                /* a ':' starting its line (an explicit value, or an empty
                 * key) sits at its mapping's indentation */
                map_col = colon_col;
            }
            if (map_col > s->indent) {
                indent_push(s, map_col);
            }
            advance(s, 1);
            s->indicator_end = s->pos;
            *tok = tok_simple(OYL_TOK_BLOCK_MAP_VALUE, start, mark(s));
            return OYL_OK;
        }
        if (s->flow_level > 0 &&
            (oyl_is_blank_or_break(next) || next == 0 ||
             next == ',' || next == ']' || next == '}' ||
             /* `:` after a JSON-like key (quoted scalar) */
             s->last_was_quoted)) {
            s->last_was_quoted = false; /* consumed */
            if (classify_colon(s) == COLON_BAD_KEY)
                SCAN_ERROR(s, "implicit key must be on a single line");
            advance(s, 1);
            *tok = tok_simple(OYL_TOK_BLOCK_MAP_VALUE, start, mark(s));
            return OYL_OK;
        }
    }

    /* reset quoted-key tracker — the : check above already consumed it */
    s->last_was_quoted = false;

    /* tag */
    if (c == '!') return scan_tag(s, tok);

    /* anchor / alias */
    if (c == '&' || c == '*') return scan_anchor_or_alias(s, tok);

    /* quoted scalars */
    if (c == '\'') return scan_single_quoted(s, tok);
    if (c == '"')  return scan_double_quoted(s, tok);

    /* block scalar (literal | or folded >) */
    if ((c == '|' || c == '>') && s->flow_level == 0) {
        oyl_scalar_style style = (c == '|') ? OYL_SCALAR_LITERAL : OYL_SCALAR_FOLDED;
        advance(s, 1); /* skip | or > */

        /* parse optional chomping and indent indicators (any order) */
        int chomp = 0;  /* 0=clip, -1=strip, 1=keep */
        int explicit_indent = 0;

        for (int i = 0; i < 2 && !AT_END(s) && !oyl_is_break(PEEK(s)); i++) {
            uint8_t ch = PEEK(s);
            if (ch == '-') { chomp = -1; advance(s, 1); }
            else if (ch == '+') { chomp = 1; advance(s, 1); }
            else if (ch >= '1' && ch <= '9') { explicit_indent = ch - '0'; advance(s, 1); }
            else break;
        }

        /* skip any trailing blanks and comment on indicator line */
        size_t hdr_blanks = s->pos;
        while (!AT_END(s) && oyl_is_blank(PEEK(s))) advance(s, 1);
        if (!AT_END(s) && PEEK(s) == '#') {
            if (s->pos == hdr_blanks)
                SCAN_ERROR(s, "comment must be separated from other tokens by whitespace");
            while (!AT_END(s) && !oyl_is_break(PEEK(s))) advance(s, 1);
        }
        if (!AT_END(s) && !oyl_is_break(PEEK(s))) SCAN_ERROR(s, "invalid block scalar indicator");
        if (!AT_END(s)) skip_break(s);

        /* determine content indent level */
        int base_indent;
        if (explicit_indent > 0) {
            base_indent = s->indent + explicit_indent;
            if (base_indent < explicit_indent) base_indent = explicit_indent;
        } else {
            /* auto-detect from first non-empty content line */
            base_indent = -1; /* sentinel: not yet detected */
            int max_empty_indent = 0; /* longest whitespace-only line */
            size_t probe = s->pos;
            while (probe < s->len) {
                int li = 0;
                while (probe + li < s->len && s->buf[probe + li] == ' ') li++;
                if (probe + li >= s->len) {
                    if (li > max_empty_indent) max_empty_indent = li;
                    break;
                }
                /* document indicators terminate block scalar */
                if (li == 0 && is_doc_indicator_at(s->buf, probe, s->len)) break;
                if (oyl_is_break((uint8_t)s->buf[probe + li])) {
                    /* empty line — track longest for indent detection */
                    if (li > max_empty_indent) max_empty_indent = li;
                    probe += li;
                    if (probe < s->len && s->buf[probe] == '\r') probe++;
                    if (probe < s->len && s->buf[probe] == '\n') probe++;
                    continue;
                }
                /* content must be indented deeper than the current block level */
                int min_indent = s->indent + 1;
                if (min_indent < 0) min_indent = 0;
                if (li >= min_indent) {
                    base_indent = li;
                    /* leading empty lines may not be indented more than
                     * the first content line (YAML 1.2 §8.1.1.1) */
                    if (max_empty_indent > li)
                        SCAN_ERROR(s, "leading empty line in block scalar has more spaces than the first content line");
                }
                break;
            }
            if (base_indent < 0) {
                /* no content lines — use longest empty line (YAML 1.2 §8.1.2) */
                int min_indent = s->indent + 1;
                if (min_indent < 1) min_indent = 1;
                base_indent = max_empty_indent > min_indent ? max_empty_indent : min_indent;
            }
        }

        /* collect raw lines into arena buffer */
        /* first pass: measure size needed */
        size_t save_pos = s->pos, save_line = s->line, save_col = s->col;
        size_t needed = 0;
        while (s->pos < s->len) {
            int li = 0;
            while (s->pos + li < s->len && s->buf[s->pos + li] == ' ') li++;
            if (s->pos + li >= s->len) {
                /* trailing blank line at end; as in the copy pass below,
                 * spaces beyond base_indent are content */
                needed += (li > base_indent ? (size_t)(li - base_indent) : 0) + 1;
                s->pos += li;
                break;
            }
            if (oyl_is_break((uint8_t)s->buf[s->pos + li])) {
                if (li > base_indent) {
                    /* whitespace-only line with extra spaces beyond base indent:
                     * treat as content line (extra spaces are preserved) */
                    size_t rest = li - base_indent;
                    needed += rest + 1;
                    s->pos += li;
                    if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
                    if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
                } else {
                    needed++; /* empty line → newline */
                    s->pos += li;
                    if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
                    if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
                }
                continue;
            }
            /* document indicators at col 0 always terminate block scalar */
            if (li == 0 && is_doc_indicator_at(s->buf, s->pos, s->len)) break;
            if (li < base_indent) {
                /* a whitespace-only line whose indentation contains a tab
                 * is neither an empty line nor a trailing comment */
                size_t k = s->pos + li;
                if (s->buf[k] == '\t') {
                    while (k < s->len && (s->buf[k] == ' ' || s->buf[k] == '\t')) k++;
                    if (k >= s->len || oyl_is_break((uint8_t)s->buf[k])) {
                        s->pos = save_pos; s->line = save_line; s->col = save_col;
                        SCAN_ERROR(s, "tabs are not allowed as indentation in a block scalar");
                    }
                }
                break;
            }
            size_t line_start = s->pos + base_indent;
            size_t rest = oyl_scan_to_break(s->buf + line_start, s->len - line_start);
            needed += rest + 1; /* content + newline */
            s->pos = line_start + rest;
            if (s->pos < s->len && s->buf[s->pos] == '\r') s->pos++;
            if (s->pos < s->len && s->buf[s->pos] == '\n') s->pos++;
        }
        size_t end_pos = s->pos;
        /* restore and do real pass */
        s->pos = save_pos; s->line = save_line; s->col = save_col;

        char *buf = oyl_arena_alloc(s->arena, needed + 1, 1);
        if (!buf) return OYL_ERR_MEMORY;
        size_t out = 0;

        /* track line structure for folded mode */
        typedef struct { size_t start; size_t len; bool empty; int extra_indent; } bsline;
        bsline *lines = NULL;
        int nlines = 0, lines_cap = 0;
        if (style == OYL_SCALAR_FOLDED) {
            /* count lines for allocation */
            size_t tp = s->pos;
            int cnt = 0;
            while (tp < end_pos) {
                tp += oyl_scan_to_break(s->buf + tp, end_pos - tp);
                cnt++;
                if (tp < end_pos && s->buf[tp] == '\r') tp++;
                if (tp < end_pos && s->buf[tp] == '\n') tp++;
            }
            lines_cap = cnt + 1;
            lines = (bsline *)oyl_arena_alloc(s->arena, lines_cap * sizeof(bsline), sizeof(void*));
            if (!lines) return OYL_ERR_MEMORY;
        }

        while (s->pos < end_pos) {
            int li = 0;
            while (s->pos + li < s->len && s->buf[s->pos + li] == ' ') li++;
            bool line_is_break = (s->pos + li >= end_pos) ||
                                 (s->pos + li < s->len && oyl_is_break((uint8_t)s->buf[s->pos + li]));
            if (line_is_break && li <= base_indent) {
                /* empty line (indent at or below base, no extra content spaces) */
                if (style == OYL_SCALAR_FOLDED && lines) {
                    lines[nlines++] = (bsline){out, 0, true, 0};
                }
                buf[out++] = '\n';
                advance_cols(s, (size_t)li);
                if (!AT_END(s) && oyl_is_break(PEEK(s))) skip_break(s);
                continue;
            }
            /* whitespace-only lines ABOVE base indent (li > base_indent)
             * fall through to content handler (extra spaces are content) */
            if (!line_is_break && li < base_indent) break;
            int extra = li - base_indent;
            advance_cols(s, (size_t)base_indent);   /* indentation: spaces */

            size_t line_content_start = out;
            /* copy extra indent spaces */
            memset(buf + out, ' ', (size_t)extra);
            out += (size_t)extra;
            advance_cols(s, (size_t)extra);
            /* check if content starts with whitespace (tab counts as more-indented) */
            bool starts_with_ws = (extra > 0) ||
                (!AT_END(s) && !oyl_is_break(PEEK(s)) && oyl_is_blank(PEEK(s)));
            /* copy line content: up to the break, so no line changes */
            size_t n = oyl_scan_to_break(BUF_AT(s), REMAINING(s));
            memcpy(buf + out, BUF_AT(s), n);
            out += n;
            advance_cols(s, n);

            if (style == OYL_SCALAR_FOLDED && lines) {
                lines[nlines++] = (bsline){line_content_start, out - line_content_start, false,
                                           starts_with_ws ? 1 : 0};
            }

            buf[out++] = '\n';
            if (!AT_END(s) && oyl_is_break(PEEK(s))) skip_break(s);
        }

        /* apply folding for '>' style */
        if (style == OYL_SCALAR_FOLDED && lines && nlines > 0) {
            char *fbuf = oyl_arena_alloc(s->arena, out + 1, 1);
            if (!fbuf) return OYL_ERR_MEMORY;
            size_t fout = 0;

            int i = 0;
            while (i < nlines) {
                if (lines[i].empty) {
                    /* leading empty lines (before any content) → preserve */
                    fbuf[fout++] = '\n';
                    i++;
                    continue;
                }

                /* non-empty line: emit its content */
                memcpy(fbuf + fout, buf + lines[i].start, lines[i].len);
                fout += lines[i].len;

                bool more_indented = lines[i].extra_indent > 0;

                /* count following empty lines */
                int empty_count = 0;
                int j = i + 1;
                while (j < nlines && lines[j].empty) {
                    empty_count++;
                    j++;
                }

                if (j >= nlines) {
                    /* no more content after this — final break + any trailing empties */
                    fbuf[fout++] = '\n';
                    for (int k = 0; k < empty_count; k++)
                        fbuf[fout++] = '\n';
                } else if (more_indented) {
                    /* more-indented: always preserve break */
                    fbuf[fout++] = '\n';
                    /* plus any empty lines */
                    for (int k = 0; k < empty_count; k++)
                        fbuf[fout++] = '\n';
                } else if (empty_count > 0) {
                    /* normal line → empty line(s):
                     * if next content is more-indented, fold break is preserved
                     * (empty_count + 1), otherwise absorbed (empty_count) */
                    int n = empty_count;
                    if (j < nlines && lines[j].extra_indent > 0) n++;
                    for (int k = 0; k < n; k++)
                        fbuf[fout++] = '\n';
                } else if (lines[j].extra_indent > 0) {
                    /* next is more-indented: preserve break */
                    fbuf[fout++] = '\n';
                } else {
                    /* next is non-empty, same indent: fold to space */
                    fbuf[fout++] = ' ';
                }
                i = j; /* skip past empty lines we already counted */
            }
            buf = fbuf;
            out = fout;
        }

        /* apply chomping to trailing newlines */
        if (chomp == -1) {
            /* strip: remove all trailing newlines */
            while (out > 0 && buf[out - 1] == '\n') out--;
        } else if (chomp == 0) {
            /* clip: keep at most one trailing newline after content.
             * If there is no non-newline content, result is empty. */
            /* check if there's any non-newline content */
            bool has_content = false;
            for (size_t ci = 0; ci < out; ci++) {
                if (buf[ci] != '\n') { has_content = true; break; }
            }
            if (!has_content) {
                out = 0;
            } else {
                while (out > 1 && buf[out - 1] == '\n' && buf[out - 2] == '\n') out--;
                if (out > 0 && buf[out - 1] != '\n') buf[out++] = '\n';
            }
        }
        /* keep (chomp == 1): preserve all trailing newlines as-is */

        buf[out] = '\0';
        *tok = tok_scalar((oyl_str){buf, out}, style, start, mark(s));
        return OYL_OK;
    }

    /* in flow context a plain scalar can't start with '-' unless a safe
     * character follows ('-a' is fine; '- ', '-,' and '-]' are not) */
    if (c == '-' && s->flow_level > 0) {
        uint8_t nc = PEEK_AT(s, 1);
        if (nc == 0 || oyl_is_blank_or_break(nc) || nc == ',' || nc == '[' ||
            nc == ']' || nc == '{' || nc == '}')
            SCAN_ERROR(s, "block sequence entry not allowed in flow context");
    }

    /* plain scalar — the common case, SIMD-accelerated */
    return scan_plain_scalar(s, tok);
}

const char *oyl_scanner_error(oyl_scanner *s) {
    if (!s || s->error_msg[0] == '\0') return NULL;
    return s->error_msg;
}

oyl_mark oyl_scanner_error_mark(oyl_scanner *s) {
    if (!s) return (oyl_mark){0, 0, 0};
    return s->error_mark;
}

/* Copy src's state into dst, a scanner over the same input: a checkpoint
 * the parser can rewind to. dst keeps its own stack buffers, growing them
 * as needed; if one can't grow, dst's state is unchanged and this returns
 * false. */
bool oyl_scanner_copy(oyl_scanner *dst, const oyl_scanner *src) {
    if (dst->indents.cap < src->indents.len) {
        int *d = realloc(dst->indents.data, (size_t)src->indents.cap * sizeof *d);
        if (!d) return false;
        dst->indents.data = d;
        dst->indents.cap = src->indents.cap;
    }
    if (dst->flows_cap < src->flows_len) {
        void *f = realloc(dst->flows, (size_t)src->flows_cap * sizeof *src->flows);
        if (!f) return false;
        dst->flows = f;
        dst->flows_cap = src->flows_cap;
    }
    indent_stack indents = dst->indents;
    void *flows = dst->flows;
    int flows_cap = dst->flows_cap;
    *dst = *src;
    dst->indents.data = indents.data;
    dst->indents.cap = indents.cap;
    if (src->indents.len)
        memcpy(dst->indents.data, src->indents.data, (size_t)src->indents.len * sizeof *src->indents.data);
    dst->flows = flows;
    dst->flows_cap = flows_cap;
    if (src->flows_len)
        memcpy(dst->flows, src->flows, (size_t)src->flows_len * sizeof *src->flows);
    return true;
}

void oyl_scanner_free(oyl_scanner *s) {
    if (!s) return;
    free(s->indents.data);
    free(s->flows);
    free(s);
}

/* ── String utilities ────────────────────────────────────── */

const char *oyl_status_str(oyl_status s) {
    switch (s) {
        case OYL_OK:         return "ok";
        case OYL_ERR_MEMORY: return "memory error";
        case OYL_ERR_INPUT:  return "input error";
        case OYL_ERR_SCAN:   return "scan error";
        case OYL_ERR_PARSE:  return "parse error";
        case OYL_ERR_EMIT:   return "emit error";
        case OYL_ERR_LIMIT:  return "limit exceeded";
    }
    return "unknown";
}

const char *oyl_token_type_str(oyl_token_type t) {
    switch (t) {
        case OYL_TOK_NONE:            return "NONE";
        case OYL_TOK_STREAM_START:    return "STREAM_START";
        case OYL_TOK_STREAM_END:      return "STREAM_END";
        case OYL_TOK_DOC_START:       return "DOC_START";
        case OYL_TOK_DOC_END:         return "DOC_END";
        case OYL_TOK_BLOCK_SEQ_ENTRY: return "BLOCK_SEQ_ENTRY";
        case OYL_TOK_BLOCK_MAP_KEY:   return "BLOCK_MAP_KEY";
        case OYL_TOK_BLOCK_MAP_VALUE: return "BLOCK_MAP_VALUE";
        case OYL_TOK_FLOW_SEQ_START:  return "FLOW_SEQ_START";
        case OYL_TOK_FLOW_SEQ_END:    return "FLOW_SEQ_END";
        case OYL_TOK_FLOW_MAP_START:  return "FLOW_MAP_START";
        case OYL_TOK_FLOW_MAP_END:    return "FLOW_MAP_END";
        case OYL_TOK_FLOW_ENTRY:      return "FLOW_ENTRY";
        case OYL_TOK_SCALAR:          return "SCALAR";
        case OYL_TOK_TAG:             return "TAG";
        case OYL_TOK_ANCHOR:          return "ANCHOR";
        case OYL_TOK_ALIAS:           return "ALIAS";
        case OYL_TOK_DIRECTIVE:       return "DIRECTIVE";
    }
    return "UNKNOWN";
}

const char *oyl_event_type_str(oyl_event_type t) {
    switch (t) {
        case OYL_EVT_NONE:           return "NONE";
        case OYL_EVT_STREAM_START:   return "STREAM_START";
        case OYL_EVT_STREAM_END:     return "STREAM_END";
        case OYL_EVT_DOC_START:      return "DOC_START";
        case OYL_EVT_DOC_END:        return "DOC_END";
        case OYL_EVT_MAPPING_START:  return "MAPPING_START";
        case OYL_EVT_MAPPING_END:    return "MAPPING_END";
        case OYL_EVT_SEQUENCE_START: return "SEQUENCE_START";
        case OYL_EVT_SEQUENCE_END:   return "SEQUENCE_END";
        case OYL_EVT_SCALAR:         return "SCALAR";
        case OYL_EVT_ALIAS:          return "ALIAS";
    }
    return "UNKNOWN";
}
