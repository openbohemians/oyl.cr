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
#if defined(OYL_SSE2)
#  include <emmintrin.h>
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Indent stack ────────────────────────────────────────── */

#define INDENT_STACK_INIT 16

/* Indentation and columns are signed, -1 being none (the top level), and
 * as wide as offsets, so a line can be as long as the input */
typedef struct {
    ptrdiff_t *data;
    int        len;
    int        cap;
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
    oyl_status   failed;    /* the first error, for oyl_scan_next() */
    ptrdiff_t    indent;
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
    ptrdiff_t    last_token_col;
    /* true if last token was a quoted scalar (for JSON-like key detection) */
    bool         last_was_quoted;

    /* arena for string duplication when needed */
    oyl_arena   *arena;

    /* implicit-key check (see check_implicit_key) */
    struct { oyl_mark open; bool is_map; ptrdiff_t props_col; size_t props_start; } *flows;
                                   /* open flow brackets, with the column of
                                    * props right before them (or -1) and
                                    * their offset */
    int          flows_len, flows_cap;
    size_t       node_line;        /* start line of the last node that */
    size_t       node_end;         /* could be a key, and its end offset */
    bool         explicit_key;     /* a '?' awaits its ':' ... */
    int          explicit_depth;   /* ... at this flow depth */
    size_t       key_colon_line;   /* line of the last block implicit-key ':' */
    size_t       node_start;       /* start offset of the last noted node */
    ptrdiff_t    node_col;         /* its 0-based start column */
    size_t       props_end;        /* end offset of the last anchor/tag run */
    ptrdiff_t    props_col;        /* 0-based column where that run starts */
    size_t       props_start;      /* and its offset */
    size_t       indicator_end;    /* offset just past the last block - ? : */

    /* where a plain scalar ending at a line break looked ahead to, past the
     * break(s) and blanks after it: the next token's skip starts at
     * ahead_from, and can go straight there (skip_blanks_and_comments) */
    size_t       ahead_from;       /* the break, or SIZE_MAX */
    size_t       ahead_pos, ahead_line, ahead_col;
    size_t       ahead_line_start; /* start of the line ahead_pos is on */

    /* the document's syntax leniencies (OYL_SYNTAX_), and where to warn
     * when one changes the result */
    unsigned        syntax;
    oyl_warn_state *warn;

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

ALWAYS_INLINE void skip_break(oyl_scanner *s) {
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
    return (ptrdiff_t)(k - line_start) > s->indent;
}

/* oyl_skip_blanks at the cursor, with the runs between tokens it is mostly
 * called for (none, or one space) taken inline. */
ALWAYS_INLINE size_t skip_blanks_here(const oyl_scanner *s) {
    const char *b = BUF_AT(s);
    size_t n = REMAINING(s);
    if (n == 0 || (b[0] != ' ' && b[0] != '\t')) return 0;
    if (n == 1 || (b[1] != ' ' && b[1] != '\t')) return 1;
    return 2 + oyl_skip_blanks(b + 2, n - 2);
}

/* Skip whitespace, line breaks and comments. Returns an error message, or
 * NULL on success:
 *  - a '#' that would start a comment must be separated from the previous
 *    token by whitespace;
 *  - in block context a tab may not be part of a line's indentation: the
 *    spaces before it must already exceed the current block indent. */
ALWAYS_INLINE const char *skip_blanks_and_comments(oyl_scanner *s) {
    size_t line_start = (s->col == 1) ? s->pos : SIZE_MAX;
    if (s->pos == s->ahead_from) {
        /* a plain scalar ending here looked past the breaks and blanks
         * that follow (only those): go on from where it got to */
        s->pos = s->ahead_pos;
        s->line = s->ahead_line;
        s->col = s->ahead_col;
        line_start = s->ahead_line_start;
        s->ahead_from = SIZE_MAX;
    } else {
        /* skip spaces and tabs: here those after a token, below a line's
         * indentation (two inline copies, which branch prediction tells
         * apart) */
        advance_cols(s, skip_blanks_here(s));
    }
    for (;;) {
        /* comment? skip to end of line */
        if (PEEK(s) == '#') {
            if (s->pos > s->begin && !oyl_is_blank_or_break((uint8_t)s->buf[s->pos - 1]))
                return "comment must be separated from other tokens by whitespace";
            size_t to_break = oyl_scan_to_break(BUF_AT(s), REMAINING(s));
            advance_cols(s, to_break);
            if (!AT_END(s) && !oyl_is_break(PEEK(s))) return "control character in comment";
        }

        /* line break? continue with the next line */
        if (!oyl_is_break(PEEK(s))) break;
        skip_break(s);
        line_start = s->pos;
        advance_cols(s, skip_blanks_here(s));
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
        if (spaces < s->pos && (ptrdiff_t)(spaces - line_start) <= s->indent)
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
    s->flows[s->flows_len].props_start = s->props_start;
    s->flows_len++;
    return true;
}

/* Close a flow collection. Returns its opening mark, and restores the
 * record of props right before it (props inside it overwrote that). */
ALWAYS_INLINE oyl_mark flow_pop(oyl_scanner *s) {
    if (s->flows_len == 0) return mark(s);
    /* closing the collection a '?' entry is in ends that entry: a ':'
     * after this is no longer its value */
    if (s->explicit_key && s->explicit_depth >= s->flows_len) s->explicit_key = false;
    s->flows_len--;
    oyl_mark open = s->flows[s->flows_len].open;
    if (s->flows[s->flows_len].props_col >= 0) {
        s->props_col = s->flows[s->flows_len].props_col;
        s->props_start = s->flows[s->flows_len].props_start;
        s->props_end = open.offset;
    }
    return open;
}

/* Record a token that could be an implicit key: a flow scalar, an alias
 * or a flow collection (start line of the node, end offset of the token). */
static inline void note_node(oyl_scanner *s, oyl_mark start, size_t end) {
    s->node_line = start.line;
    s->node_start = start.offset;
    s->node_col = (ptrdiff_t)start.col - 1;
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
    if (!only_blanks_between(s, s->props_end, start.offset)) {
        s->props_col = (ptrdiff_t)start.col - 1;
        s->props_start = start.offset;
    }
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
ALWAYS_INLINE int classify_colon(oyl_scanner *s) {
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

/* Where the key of a ':' at the current position starts: at the node
 * right before it, or at props right before that node (or before the ':'
 * itself, for an empty node). SIZE_MAX when there's no such key. */
static size_t key_start(const oyl_scanner *s) {
    if (only_blanks_between(s, s->node_end, s->pos))
        return only_blanks_between(s, s->props_end, s->node_start) ? s->props_start
                                                                   : s->node_start;
    if (only_blanks_between(s, s->props_end, s->pos)) return s->props_start;
    return SIZE_MAX;
}

/* An implicit key is at most 1024 characters, its props and the blanks
 * before its ':' included. Characters, not bytes: UTF-8 continuation
 * bytes don't count. */
static bool key_too_long(const oyl_scanner *s, size_t start) {
    if (start == SIZE_MAX || s->pos - start <= 1024) return false;
    size_t chars = 0;
    for (size_t i = start; i < s->pos; i++) chars += ((uint8_t)s->buf[i] & 0xC0) != 0x80;
    return chars > 1024;
}

/* ── Indent management ───────────────────────────────────── */

static bool indent_push(oyl_scanner *s, ptrdiff_t col) {
    indent_stack *st = &s->indents;
    if (st->len >= st->cap) {
        int new_cap = st->cap * 2;
        ptrdiff_t *new_data = realloc(st->data, (size_t)new_cap * sizeof *new_data);
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

/* Tokens are written field by field through the caller's pointer, never
 * built as a whole value and assigned: GCC may stage such an 80-byte value
 * on the stack and copy it with `rep movs`, which is microcoded. After an
 * unrelated one-line change it did so on the ':' path, and block YAML
 * parsed 10-15% slower. */
static inline void tok_set(oyl_token *tok, oyl_token_type type, oyl_str value,
                           oyl_scalar_style style, oyl_mark start, oyl_mark end) {
    tok->type         = type;
    tok->value        = value;
    tok->scalar_style = style;
    tok->start        = start;
    tok->end          = end;
}

static inline void tok_simple(oyl_token *tok, oyl_token_type type, oyl_mark start, oyl_mark end) {
    tok_set(tok, type, OYL_STR_NULL, OYL_SCALAR_PLAIN, start, end);
}

static inline void tok_scalar(oyl_token *tok, oyl_str value, oyl_scalar_style style,
                              oyl_mark start, oyl_mark end) {
    tok_set(tok, OYL_TOK_SCALAR, value, style, start, end);
}

static bool maybe_unroll_indents(oyl_scanner *s, ptrdiff_t col) {
    while (s->indent > col) {
        indent_pop(s);
    }
    return true;
}

static bool is_doc_indicator_at(const char *buf, size_t pos, size_t len);

/* ── Scan specific token types ───────────────────────────── */

/* Inlined into oyl_scan_token, its one caller: the most common token */
ALWAYS_INLINE oyl_status scan_plain_scalar(oyl_scanner *s, oyl_token *tok) {
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
            size_t blank_run = skip_blanks_here(s);
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
            size_t ahead_line_start = 0;

            /* the scalar ends at the break: back to it, noting how far the
             * look ahead got for the next token's skip */
            #define END_AT_BREAK() do { \
                s->ahead_from = save; \
                s->ahead_pos = s->pos; \
                s->ahead_line = s->line; \
                s->ahead_col = s->col; \
                s->ahead_line_start = ahead_line_start; \
                s->pos = save; s->line = save_line; s->col = save_col; \
            } while (0)

            int break_count = 0;
            while (!AT_END(s) && oyl_is_break(PEEK(s))) {
                skip_break(s);
                ahead_line_start = s->pos;
                break_count++;
                size_t blanks = skip_blanks_here(s);
                s->pos += blanks;
                s->col += blanks;
                if (!AT_END(s) && !oyl_is_break(PEEK(s))) break;
            }

            if (AT_END(s)) {
                s->pos = save; s->line = save_line; s->col = save_col;
                break;
            }

            ptrdiff_t next_indent = (ptrdiff_t)s->col - 1;
            ptrdiff_t min_indent = s->indent + 1;
            if (min_indent < 0) min_indent = 0;

            if (next_indent < min_indent) {
                END_AT_BREAK();
                break;
            }

            /* check for indicators that end the scalar. Only ": ", "#" and
             * a document marker do: a line indented enough to continue the
             * scalar may start with any other indicator (YAML 1.2, 7.3.3:
             * only a plain scalar's first character is restricted). So
             * "a\n- b", "a\n&b" and "[a\n? b]" are "a - b", "a &b" and
             * ["a ? b"]. A line at or below a block collection's indent,
             * where "- " or "&" could start a node, failed the indentation
             * check above */
            uint8_t nc = PEEK(s);
            /* : followed by whitespace always ends (it introduces a mapping value) */
            if (nc == ':') {
                uint8_t after = PEEK_AT(s, 1);
                if (oyl_is_blank_or_break(after) || after == 0) {
                    END_AT_BREAK();
                    break;
                }
            }
            if (nc == '#') {
                END_AT_BREAK();
                break;
            }
            if (s->col == 1 && REMAINING(s) >= 3) {
                if ((nc == '-' && PEEK_AT(s,1) == '-' && PEEK_AT(s,2) == '-') ||
                    (nc == '.' && PEEK_AT(s,1) == '.' && PEEK_AT(s,2) == '.')) {
                    if (REMAINING(s) == 3 || oyl_is_blank_or_break(PEEK_AT(s,3))) {
                        END_AT_BREAK();
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
    s->last_token_col = (ptrdiff_t)start.col - 1;  /* 0-based col of this scalar */
    if (in_buf) {
        buf[out] = '\0';
        note_node(s, start, end.offset);
        tok_scalar(tok, (oyl_str){buf, out}, OYL_SCALAR_PLAIN, start, end);
    } else {
        note_node(s, start, end.offset);
        tok_scalar(tok, (oyl_str){single_start, single_len}, OYL_SCALAR_PLAIN, start, end);
    }
    return OYL_OK;

    #undef APPEND_RUN
    #undef APPEND_CHAR
    #undef SWITCH_TO_BUF
    #undef TRIM_TRAILING
    #undef END_AT_BREAK
}

static oyl_status scan_single_quoted_slow(oyl_scanner *s, oyl_token *tok, oyl_mark start);

/* The fast path, inlined into oyl_scan_token: no '' escapes or line
 * breaks → zero-copy slice of the input. The rest is out of line. */
ALWAYS_INLINE oyl_status scan_single_quoted(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);
    advance_cols(s, 1); /* skip opening ' */

    size_t j = s->pos + oyl_find_in_quotes_short(BUF_AT(s), REMAINING(s), '\'', '\'');
    if (j < s->len && s->buf[j] == '\'' &&
        !(j + 1 < s->len && s->buf[j + 1] == '\'')) {
        oyl_str val = {s->buf + s->pos, j - s->pos};
        s->col += j + 1 - s->pos;
        s->pos = j + 1;
        s->last_token_col = (ptrdiff_t)start.col - 1;
        s->last_was_quoted = true;
        note_node(s, start, s->pos);
        tok_scalar(tok, val, OYL_SCALAR_SINGLE_QUOTED, start, mark(s));
        return OYL_OK;
    }
    return scan_single_quoted_slow(s, tok, start);
}

/* Past the opening quote, at `start` */
NOINLINE oyl_status scan_single_quoted_slow(oyl_scanner *s, oyl_token *tok, oyl_mark start) {
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
            if (PEEK(s) < 0x20 && PEEK(s) != '\t') SCAN_ERROR(s, "control character in quoted scalar");
            size_t n = oyl_find_in_quotes(BUF_AT(s), REMAINING(s), '\'', '\'');
            SQ_ENSURE(n);
            memcpy(buf + out, BUF_AT(s), n);
            out += n;
            advance_cols(s, n);
        }
    }

    if (PEEK(s) != '\'') SCAN_ERROR(s, "unterminated single-quoted scalar");
    advance(s, 1); /* skip closing ' */

    buf[out] = '\0';
    s->last_token_col = (ptrdiff_t)start.col - 1;
    s->last_was_quoted = true;
    note_node(s, start, s->pos);
    tok_scalar(tok, (oyl_str){buf, out}, OYL_SCALAR_SINGLE_QUOTED, start, mark(s));
    return OYL_OK;

    #undef SQ_ENSURE
}

static oyl_status scan_double_quoted_slow(oyl_scanner *s, oyl_token *tok, oyl_mark start);

/* The fast path, inlined into oyl_scan_token: no escapes or line breaks →
 * zero-copy slice of the input. The rest is out of line. */
ALWAYS_INLINE oyl_status scan_double_quoted(oyl_scanner *s, oyl_token *tok) {
    oyl_mark start = mark(s);
    advance_cols(s, 1); /* skip opening " */

    size_t j = s->pos + oyl_find_in_quotes_short(BUF_AT(s), REMAINING(s), '"', '\\');
    if (j < s->len && s->buf[j] == '"') {
        oyl_str val = {s->buf + s->pos, j - s->pos};
        s->col += j + 1 - s->pos;
        s->pos = j + 1;
        s->last_token_col = (ptrdiff_t)start.col - 1;
        s->last_was_quoted = true;
        note_node(s, start, s->pos);
        tok_scalar(tok, val, OYL_SCALAR_DOUBLE_QUOTED, start, mark(s));
        return OYL_OK;
    }
    return scan_double_quoted_slow(s, tok, start);
}

/* Past the opening quote, at `start` */
NOINLINE oyl_status scan_double_quoted_slow(oyl_scanner *s, oyl_token *tok, oyl_mark start) {
    /* double-quoted always needs processing for escape sequences */
    /* pre-scan for length */
    size_t max_len = 0;
    size_t scan = s->pos;
    while (scan < s->len) {
        size_t n = oyl_find_any4(s->buf + scan, s->len - scan, '"', '\\', '"', '"');
        scan += n;
        max_len += n;
        if (scan >= s->len || s->buf[scan] == '"') break;
        /* a backslash and the escaped byte: one output byte (two for \N, \_,
         * three for \L, \P) */
        if (scan + 1 < s->len && (s->buf[scan+1] == 'N' || s->buf[scan+1] == '_'))
            max_len++; /* 2-byte UTF-8 output */
        else if (scan + 1 < s->len && (s->buf[scan+1] == 'L' || s->buf[scan+1] == 'P'))
            max_len += 2; /* 3-byte UTF-8 output */
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
                case '\'':  /* libyaml's: not a YAML 1.2 escape */
                    if (!(s->syntax & OYL_SYNTAX_SQUOTE_ESCAPE)) SCAN_ERROR(s, "invalid escape sequence");
                    buf[out++] = '\'';
                    if (s->warn) {
                        oyl_mark at = mark(s);
                        at.offset -= 2;
                        at.col -= 2;
                        oyl_warn(s->warn, OYL_WARN_LENIENCY, at,
                                 "\\' in double quotes is not a YAML 1.2 escape (read as ')");
                    }
                    break;
                case 'N':  /* NEL U+0085 — encode as UTF-8 */
                    buf[out++] = (char)0xC2;
                    buf[out++] = (char)0x85;
                    break;
                case '_':  /* NBSP U+00A0 */
                    buf[out++] = (char)0xC2;
                    buf[out++] = (char)0xA0;
                    break;
                case 'L':  /* line separator U+2028 */
                case 'P':  /* paragraph separator U+2029 */
                    buf[out++] = (char)0xE2;
                    buf[out++] = (char)0x80;
                    buf[out++] = (char)(esc == 'L' ? 0xA8 : 0xA9);
                    break;
                case 'x': { /* \xNN */
                    if (REMAINING(s) < 2) SCAN_ERROR(s, "incomplete \\x escape sequence");
                    uint8_t hi = PEEK(s), lo = PEEK_AT(s, 1);
                    if (!oyl_is_hex(hi) || !oyl_is_hex(lo)) SCAN_ERROR(s, "invalid hex digit in \\x escape");
                    uint8_t byte = (hex_digit(hi) << 4) | hex_digit(lo);
                    advance(s, 2);
                    /* the code point U+00NN, in UTF-8: \xB5 is µ (C2 B5) */
                    if (byte < 0x80) {
                        buf[out++] = (char)byte;
                    } else {
                        buf[out++] = (char)(0xC0 | (byte >> 6));
                        buf[out++] = (char)(0x80 | (byte & 0x3F));
                    }
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
            if (PEEK(s) < 0x20 && PEEK(s) != '\t') SCAN_ERROR(s, "control character in quoted scalar");
            const char *run = BUF_AT(s);
            size_t n = oyl_find_in_quotes(run, REMAINING(s), '"', '\\');
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
    s->last_token_col = (ptrdiff_t)start.col - 1;
    s->last_was_quoted = true;
    note_node(s, start, s->pos);
    tok_scalar(tok, (oyl_str){buf, out}, OYL_SCALAR_DOUBLE_QUOTED, start, mark(s));
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
        s->last_token_col = (ptrdiff_t)start.col - 1;
        note_props(s, start, s->pos);
        tok_set(tok, OYL_TOK_TAG, (oyl_str){tag_start, len}, OYL_SCALAR_PLAIN, start, mark(s));
        return OYL_OK;
    }

    /* consume tag characters */
    while (!AT_END(s) && !oyl_is_blank_or_break(PEEK(s))
           && !oyl_is_flow(PEEK(s))) {
        if (PEEK(s) < 0x20 || PEEK(s) == 0x7F) SCAN_ERROR(s, "control character in tag");
        advance(s, 1);
    }

    size_t len = BUF_AT(s) - tag_start;
    s->last_token_col = (ptrdiff_t)start.col - 1;
    note_props(s, start, s->pos);
    tok_set(tok, OYL_TOK_TAG, (oyl_str){tag_start, len}, OYL_SCALAR_PLAIN, start, mark(s));
    return OYL_OK;
}

/* A character at buf[i] that YAML doesn't allow, found without decoding
 * UTF-8: a C1 control but NEL, U+FEFF (a byte order mark), U+FFFE or
 * U+FFFF? */
static bool unallowed_char_at(const char *buf, size_t len, size_t i) {
    uint8_t c = (uint8_t)buf[i];
    uint8_t c1 = i + 1 < len ? (uint8_t)buf[i + 1] : 0, c2 = i + 2 < len ? (uint8_t)buf[i + 2] : 0;
    if (c == 0xC2) return c1 >= 0x80 && c1 <= 0x9F && c1 != 0x85;
    return c == 0xEF && ((c1 == 0xBB && c2 == 0xBF) || (c1 == 0xBF && (c2 == 0xBE || c2 == 0xBF)));
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
        /* a name can't be escaped: these can't be in one (in scalars, the
         * emitter escapes them) */
        if (ch >= 0xC2 && unallowed_char_at(s->buf, s->len, s->pos))
            SCAN_ERROR(s, "character not allowed in anchor or alias name");
        advance(s, 1);
    }
    size_t name_len = BUF_AT(s) - name_start;
    if (name_len == 0) SCAN_ERROR(s, "empty anchor or alias name");

    s->last_token_col = (ptrdiff_t)start.col - 1;
    if (!is_anchor) note_node(s, start, s->pos);
    else note_props(s, start, s->pos);
    tok_set(tok, is_anchor ? OYL_TOK_ANCHOR : OYL_TOK_ALIAS, (oyl_str){name_start, name_len},
            OYL_SCALAR_PLAIN, start, mark(s));
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

/* A block scalar (literal | or folded >), its indicator `c` at `start`. */
NOINLINE oyl_status scan_block_scalar(oyl_scanner *s, oyl_token *tok, oyl_mark start, uint8_t c) {
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
        while (!AT_END(s) && !oyl_is_break(PEEK(s))) {
            if (oyl_is_control(PEEK(s))) SCAN_ERROR(s, "control character in comment");
            advance(s, 1);
        }
    }
    if (!AT_END(s) && !oyl_is_break(PEEK(s))) SCAN_ERROR(s, "invalid block scalar indicator");
    if (!AT_END(s)) skip_break(s);

    /* determine content indent level */
    ptrdiff_t base_indent;
    if (explicit_indent > 0) {
        base_indent = s->indent + explicit_indent;
        if (base_indent < explicit_indent) base_indent = explicit_indent;
    } else {
        /* auto-detect from first non-empty content line */
        base_indent = -1; /* sentinel: not yet detected */
        ptrdiff_t max_empty_indent = 0; /* longest whitespace-only line */
        size_t probe = s->pos;
        while (probe < s->len) {
            ptrdiff_t li = 0;
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
            ptrdiff_t min_indent = s->indent + 1;
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
            ptrdiff_t min_indent = s->indent + 1;
            if (min_indent < 1) min_indent = 1;
            base_indent = max_empty_indent > min_indent ? max_empty_indent : min_indent;
        }
    }

    /* collect raw lines into arena buffer */
    /* first pass: measure size needed */
    size_t save_pos = s->pos, save_line = s->line, save_col = s->col;
    size_t needed = 0;
    while (s->pos < s->len) {
        ptrdiff_t li = 0;
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
        if (line_start + rest < s->len && !oyl_is_break((uint8_t)s->buf[line_start + rest])) {
            size_t at = line_start + rest;
            s->pos = save_pos; s->line = save_line; s->col = save_col;
            advance(s, at - save_pos);
            SCAN_ERROR(s, "control character in block scalar");
        }
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
    typedef struct { size_t start; size_t len; bool empty; ptrdiff_t extra_indent; } bsline;
    bsline *lines = NULL;
    size_t nlines = 0, lines_cap = 0;
    if (style == OYL_SCALAR_FOLDED) {
        /* count lines for allocation */
        size_t tp = s->pos;
        size_t cnt = 0;
        while (tp < end_pos) {
            tp += oyl_find_break(s->buf + tp, end_pos - tp);
            cnt++;
            if (tp < end_pos && s->buf[tp] == '\r') tp++;
            if (tp < end_pos && s->buf[tp] == '\n') tp++;
        }
        lines_cap = cnt + 1;
        lines = (bsline *)oyl_arena_alloc(s->arena, lines_cap * sizeof(bsline), sizeof(void*));
        if (!lines) return OYL_ERR_MEMORY;
    }

    /* whether the last line ended with a break: at the end of the input
     * it may not (see OYL_SYNTAX_EOF_NO_BREAK) */
    bool terminated = true;
    while (s->pos < end_pos) {
        ptrdiff_t li = 0;
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
            terminated = !AT_END(s) && oyl_is_break(PEEK(s));
            if (terminated) skip_break(s);
            continue;
        }
        /* whitespace-only lines ABOVE base indent (li > base_indent)
         * fall through to content handler (extra spaces are content) */
        if (!line_is_break && li < base_indent) break;
        ptrdiff_t extra = li - base_indent;
        advance_cols(s, (size_t)base_indent);   /* indentation: spaces */

        size_t line_content_start = out;
        /* copy extra indent spaces */
        memset(buf + out, ' ', (size_t)extra);
        out += (size_t)extra;
        advance_cols(s, (size_t)extra);
        /* check if content starts with whitespace (tab counts as more-indented) */
        bool starts_with_ws = (extra > 0) ||
            (!AT_END(s) && !oyl_is_break(PEEK(s)) && oyl_is_blank(PEEK(s)));
        /* copy line content: up to the break, so no line changes (the
         * first pass checked it for control characters) */
        size_t n = oyl_find_break(BUF_AT(s), REMAINING(s));
        memcpy(buf + out, BUF_AT(s), n);
        out += n;
        advance_cols(s, n);

        if (style == OYL_SCALAR_FOLDED && lines) {
            lines[nlines++] = (bsline){line_content_start, out - line_content_start, false,
                                       starts_with_ws ? 1 : 0};
        }

        buf[out++] = '\n';
        terminated = !AT_END(s) && oyl_is_break(PEEK(s));
        if (terminated) skip_break(s);
    }

    /* apply folding for '>' style */
    if (style == OYL_SCALAR_FOLDED && lines && nlines > 0) {
        char *fbuf = oyl_arena_alloc(s->arena, out + 1, 1);
        if (!fbuf) return OYL_ERR_MEMORY;
        size_t fout = 0;

        size_t i = 0;
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
            size_t empty_count = 0;
            size_t j = i + 1;
            while (j < nlines && lines[j].empty) {
                empty_count++;
                j++;
            }

            if (j >= nlines) {
                /* no more content after this — final break + any trailing empties */
                fbuf[fout++] = '\n';
                for (size_t k = 0; k < empty_count; k++)
                    fbuf[fout++] = '\n';
            } else if (more_indented) {
                /* more-indented: always preserve break */
                fbuf[fout++] = '\n';
                /* plus any empty lines */
                for (size_t k = 0; k < empty_count; k++)
                    fbuf[fout++] = '\n';
            } else if (empty_count > 0) {
                /* normal line → empty line(s):
                 * if next content is more-indented, fold break is preserved
                 * (empty_count + 1), otherwise absorbed (empty_count) */
                size_t n = empty_count;
                if (j < nlines && lines[j].extra_indent > 0) n++;
                for (size_t k = 0; k < n; k++)
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

    /* libyaml's: a last line ending the input without a break gives no
     * line feed; YAML 1.2 (and its test suite) gives one */
    bool eof_lenient = !terminated && (s->syntax & OYL_SYNTAX_EOF_NO_BREAK) && chomp != -1;
    bool dropped = false;
    if (eof_lenient && out > 0 && buf[out - 1] == '\n') { out--; dropped = true; }

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
            dropped = false;
        } else {
            while (out > 1 && buf[out - 1] == '\n' && buf[out - 2] == '\n') out--;
            if (!eof_lenient && out > 0 && buf[out - 1] != '\n') buf[out++] = '\n';
        }
    }
    if (dropped && s->warn)
        oyl_warn(s->warn, OYL_WARN_LENIENCY, start,
                 "block scalar ending the input without a line break: no final line feed, "
                 "as libyaml reads it (YAML 1.2 gives one)");
    /* keep (chomp == 1): preserve all trailing newlines as-is */

    buf[out] = '\0';
    tok_scalar(tok, (oyl_str){buf, out}, style, start, mark(s));
    return OYL_OK;
}

/* ── Main scan function ──────────────────────────────────── */

/* The bytes oyl_scan_token checks a token's first byte for before taking it
 * as a plain scalar's. */
static const bool indicator_start[256] = {
    ['-'] = true, ['.'] = true, ['%'] = true, ['['] = true, [']'] = true,
    ['{'] = true, ['}'] = true, [','] = true, ['?'] = true, [':'] = true,
    ['!'] = true, ['&'] = true, ['*'] = true, ['\''] = true, ['"'] = true,
    ['|'] = true, ['>'] = true,
};

oyl_scanner *oyl_scanner_new(const char *input, size_t len, oyl_arena *a) {
    if (!input) {   /* no input is empty text, so offsets aren't taken from NULL */
        if (len) return NULL;
        input = "";
    }
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
        .ahead_from           = SIZE_MAX,
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
    s->indents.data = malloc(INDENT_STACK_INIT * sizeof *s->indents.data);
    if (!s->indents.data) { free(s); return NULL; }

    return s;
}


oyl_status oyl_scan_next(oyl_scanner *s, const oyl_token **tok) {
    /* an error is final: going on from where it stopped would read the
     * rest of a bad token as new ones (the parser, which calls
     * oyl_scan_token, keeps the first error itself) */
    oyl_status st = s->failed ? s->failed : oyl_scan_token(s, &s->out_tok);
    /* In a flow collection, a ':' right before '[' or '{', not after a
     * quoted key, is neither a value indicator nor part of a plain scalar
     * ("{a:[1]}"): oyl_scan_token gives an empty scalar there without
     * moving on, which the parser reports as an error. Here it is one, or
     * the tokens would repeat it forever. */
    if (st == OYL_OK && s->out_tok.type == OYL_TOK_SCALAR && s->out_tok.value.len == 0 &&
        s->out_tok.scalar_style == OYL_SCALAR_PLAIN &&
        s->out_tok.end.offset == s->out_tok.start.offset) {
        snprintf(s->error_msg, sizeof(s->error_msg), "%s",
                 "':' must be followed by a space before a flow collection");
        s->error_mark = s->out_tok.start;
        st = OYL_ERR_SCAN;
    }
    if (st != OYL_OK) s->failed = st;
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
        tok_simple(tok, OYL_TOK_STREAM_START, mark(s), mark(s));
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
            tok_simple(tok, OYL_TOK_STREAM_END, mark(s), mark(s));
            return OYL_OK;
        }
        tok_simple(tok, OYL_TOK_NONE, mark(s), mark(s));
        return OYL_OK;
    }

    /* unroll indent stack to current column (block context only).
     * This ensures s->indent reflects the actual nesting depth,
     * which is needed for plain scalar continuation checks. */
    if (s->flow_level == 0) {
        ptrdiff_t cur_col = (ptrdiff_t)s->col - 1;  /* 0-based */
        maybe_unroll_indents(s, cur_col);
    }

    oyl_mark start = mark(s);
    uint8_t c = PEEK(s);

    /* most tokens start with a byte none of the checks below take (a
     * letter or digit): those are plain scalars */
    if (!indicator_start[c]) {
        s->last_was_quoted = false;
        goto plain;   /* one call site, so that it is inlined */
    }

    /* the rest, by first byte; those not taken here are plain scalars too */
    switch (c) {
    case '-':
        /* document start at column 1 */
        if (at_doc_indicator(s, '-')) {
            maybe_unroll_indents(s, -1);
            advance(s, 3);
            tok_simple(tok, OYL_TOK_DOC_START, start, mark(s));
            return OYL_OK;
        }
        /* block sequence entry: - followed by whitespace */
        if (s->flow_level == 0) {
            uint8_t next = PEEK_AT(s, 1);
            if (oyl_is_blank_or_break(next) || next == 0) {
                /* "key: - a" — a block sequence can't start on a key's line */
                if (s->key_colon_line == s->line)
                    SCAN_ERROR(s, "block sequence entries are not allowed on the same line as a mapping key");
                if (tab_after_indicator(s, s->pos))
                    SCAN_ERROR(s, "tabs are not allowed before a nested block collection");
                ptrdiff_t col = (ptrdiff_t)s->col - 1;
                if (col > s->indent && !indent_push(s, col)) return OYL_ERR_MEMORY;
                advance_cols(s, 1);
                s->indicator_end = s->pos;
                tok_simple(tok, OYL_TOK_BLOCK_SEQ_ENTRY, start, mark(s));
                return OYL_OK;
            }
        }
        s->last_was_quoted = false;
        /* in flow context a plain scalar can't start with '-' unless a safe
         * character follows ('-a' is fine; '- ', '-,' and '-]' are not) */
        if (s->flow_level > 0) {
            uint8_t nc = PEEK_AT(s, 1);
            if (nc == 0 || oyl_is_blank_or_break(nc) || nc == ',' || nc == '[' ||
                nc == ']' || nc == '{' || nc == '}')
                SCAN_ERROR(s, "block sequence entry not allowed in flow context");
        }
        goto plain;
    case '.':
        /* document end at column 1 */
        if (at_doc_indicator(s, '.')) {
            maybe_unroll_indents(s, -1);
            advance(s, 3);
            /* only a comment may follow '...' on its line */
            size_t k = s->pos;
            while (k < s->len && (s->buf[k] == ' ' || s->buf[k] == '\t')) k++;
            if (k < s->len && s->buf[k] != '#' && !oyl_is_break((uint8_t)s->buf[k]))
                SCAN_ERROR(s, "content after document end marker '...'");
            tok_simple(tok, OYL_TOK_DOC_END, start, mark(s));
            return OYL_OK;
        }
        break;
    case '%':
        /* directive lines: % at column 1 in block context → one token holding
         * the whole line; the parser validates it */
        if (s->col == 1 && s->flow_level == 0) {
            const char *line_start = BUF_AT(s);
            size_t len = oyl_scan_to_break(line_start, REMAINING(s));
            advance_cols(s, len);
            if (!AT_END(s) && !oyl_is_break(PEEK(s))) SCAN_ERROR(s, "control character in directive");
            tok_set(tok, OYL_TOK_DIRECTIVE, (oyl_str){line_start, len}, OYL_SCALAR_PLAIN, start, mark(s));
            return OYL_OK;
        }
        break;
    /* flow indicators */
    case '[':
        s->last_was_quoted = false;
        if (!flow_push(s, false)) return OYL_ERR_MEMORY;
        s->flow_level++;
        advance_cols(s, 1);
        tok_simple(tok, OYL_TOK_FLOW_SEQ_START, start, mark(s));
        return OYL_OK;
    case ']': {
        oyl_mark open = flow_pop(s);
        if (s->flow_level > 0) s->flow_level--;
        advance_cols(s, 1);
        note_node(s, open, s->pos);
        s->last_was_quoted = true; /* allow ]:value like "key":value */
        tok_simple(tok, OYL_TOK_FLOW_SEQ_END, start, mark(s));
        return OYL_OK;
    }
    case '{':
        s->last_was_quoted = false;
        if (!flow_push(s, true)) return OYL_ERR_MEMORY;
        s->flow_level++;
        advance_cols(s, 1);
        tok_simple(tok, OYL_TOK_FLOW_MAP_START, start, mark(s));
        return OYL_OK;
    case '}': {
        oyl_mark open = flow_pop(s);
        if (s->flow_level > 0) s->flow_level--;
        advance_cols(s, 1);
        note_node(s, open, s->pos);
        s->last_was_quoted = true; /* allow }:value like "key":value */
        tok_simple(tok, OYL_TOK_FLOW_MAP_END, start, mark(s));
        return OYL_OK;
    }
    case ',':
        s->last_was_quoted = false;   /* ":" after "," is not adjacent to a key */
        /* so does a ',' at its depth: the entry ended without a value */
        if (s->explicit_key && s->explicit_depth == s->flows_len) s->explicit_key = false;
        advance_cols(s, 1);
        tok_simple(tok, OYL_TOK_FLOW_ENTRY, start, mark(s));
        return OYL_OK;
    case '?':
        /* explicit mapping key: ? followed by whitespace (both block and flow) */
        if (s->flow_level == 0) {
            uint8_t next = PEEK_AT(s, 1);
            if (oyl_is_blank_or_break(next) || next == 0) {
                /* "key: ? x" — a block mapping can't start on a key's line */
                if (s->key_colon_line == s->line)
                    SCAN_ERROR(s, "mapping values are not allowed on the same line as another key");
                if (tab_after_indicator(s, s->pos))
                    SCAN_ERROR(s, "tabs are not allowed before a nested block collection");
                ptrdiff_t qcol = (ptrdiff_t)s->col - 1;
                if (qcol > s->indent && !indent_push(s, qcol)) return OYL_ERR_MEMORY;
                advance_cols(s, 1);
                s->indicator_end = s->pos;
                s->explicit_key = true;
                s->explicit_depth = s->flows_len;
                tok_simple(tok, OYL_TOK_BLOCK_MAP_KEY, start, mark(s));
                return OYL_OK;
            }
        }
        /* ? in flow context */
        if (s->flow_level > 0) {
            uint8_t next = PEEK_AT(s, 1);
            if (oyl_is_blank_or_break(next) || next == 0) {
                advance_cols(s, 1);
                s->explicit_key = true;
                s->explicit_depth = s->flows_len;
                tok_simple(tok, OYL_TOK_BLOCK_MAP_KEY, start, mark(s));
                return OYL_OK;
            }
        }
        break;
    case ':': {
        /* block mapping value: : followed by whitespace */
        uint8_t next = PEEK_AT(s, 1);
        if (s->flow_level == 0 && (oyl_is_blank_or_break(next) || next == 0)) {
            ptrdiff_t colon_col = (ptrdiff_t)s->col - 1;
            /* when a node (scalar, alias, or flow collection) directly
             * precedes the ':', the key starts where it does, or at props
             * right before it on its line ("&k key:", "!!map {a: b}:");
             * an empty key starts at its props ("&k :") or the ':' itself
             * ("- : v") */
            ptrdiff_t key_col = colon_col;
            size_t key_at = SIZE_MAX;   /* key_start(s), worked out here */
            if (only_blanks_between(s, s->node_end, s->pos)) {
                key_col = s->node_col;
                key_at = s->node_start;
                if (only_blanks_between(s, s->props_end, s->node_start)) {
                    key_col = s->props_col;
                    key_at = s->props_start;
                }
            } else if (only_blanks_between(s, s->props_end, s->pos)) {
                key_col = s->props_col;
                key_at = s->props_start;
            }
            /* use minimum of key and colon column — handles explicit key
             * (? key\n: val) where colon_col is the mapping indent */
            ptrdiff_t map_col = key_col < colon_col ? key_col : colon_col;
            int kind = classify_colon(s);
            if (kind == COLON_BAD_KEY)
                SCAN_ERROR(s, "implicit key must be on a single line");
            if (kind == COLON_IMPLICIT) {
                if (key_too_long(s, key_at))
                    SCAN_ERROR(s, "an implicit key is at most 1024 characters");
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
            if (map_col > s->indent && !indent_push(s, map_col)) return OYL_ERR_MEMORY;
            advance_cols(s, 1);
            s->indicator_end = s->pos;
            tok_simple(tok, OYL_TOK_BLOCK_MAP_VALUE, start, mark(s));
            return OYL_OK;
        }
        if (s->flow_level > 0 &&
            (oyl_is_blank_or_break(next) || next == 0 ||
             next == ',' || next == ']' || next == '}' ||
             /* `:` after a JSON-like key (quoted scalar) */
             s->last_was_quoted)) {
            s->last_was_quoted = false; /* consumed */
            int kind = classify_colon(s);
            if (kind == COLON_BAD_KEY)
                SCAN_ERROR(s, "implicit key must be on a single line");
            /* a flow mapping's keys may be longer; a pair's in a flow
             * sequence ("[key: value]") may not */
            if (kind == COLON_IMPLICIT && s->flows_len > 0 && !s->flows[s->flows_len - 1].is_map &&
                key_too_long(s, key_start(s)))
                SCAN_ERROR(s, "an implicit key is at most 1024 characters");
            advance_cols(s, 1);
            tok_simple(tok, OYL_TOK_BLOCK_MAP_VALUE, start, mark(s));
            return OYL_OK;
        }
        break;
    }
    case '!':
        s->last_was_quoted = false;
        return scan_tag(s, tok);
    case '&': case '*':
        s->last_was_quoted = false;
        return scan_anchor_or_alias(s, tok);
    case '\'':
        s->last_was_quoted = false;
        return scan_single_quoted(s, tok);
    case '"':
        s->last_was_quoted = false;
        return scan_double_quoted(s, tok);
    case '|': case '>':
        s->last_was_quoted = false;
        if (s->flow_level == 0) return scan_block_scalar(s, tok, start, c);
        goto plain;
    }

    /* reset quoted-key tracker — the : check above already consumed it */
    s->last_was_quoted = false;

plain:
    /* plain scalar — the common case, SIMD-accelerated */
    return scan_plain_scalar(s, tok);
}

/* ── Flow fast path (oyl_internal.h) ───────────────────────── */

static inline bool ff_alnum(uint8_t c) {
    return (uint8_t)((c | 0x20) - 'a') < 26 || (uint8_t)(c - '0') < 10;
}

/* A byte of a plain scalar the fast path takes: letters, digits, . _ + - */
static inline bool ff_plain(uint8_t c) {
    return ff_alnum(c) || c == '.' || c == '_' || c == '+' || c == '-';
}

#define FF_MAX_DEPTH 32

/* Its subset is chosen so that the flow-key lookahead (oyl_parser.c,
 * fk_scan) reads every byte of it as the scanner does: each quote opens a
 * quoted scalar after an entry start or a value's ':', there are no
 * comments, line breaks or escapes, and a ':' always follows a key. The
 * events are those the parser's flow states make from the tokens; a
 * scalar's end is where its token ends, past a plain scalar's trailing
 * blanks. */
HOT_ALIGNED int oyl_scan_flow_fast(const oyl_scanner *s, const oyl_token *open, oyl_event *out, int cap,
                       oyl_flow_fast *ff) {
    /* a '?' entry awaiting its ':' changes what ',' and a close do */
    if (s->explicit_key || s->flows_len == 0 || cap < 2) return -1;
    const char *b = s->buf;
    size_t len = s->len;
    size_t line = open->start.line;
    size_t col1 = open->start.offset - (open->start.col - 1);   /* column 1's offset */
    enum { ITEM, COLON, VALUE, SEP } state = ITEM;
    bool map[FF_MAX_DEPTH];
    int depth = 1, maxd = 1, n = 0;
    bool nested = false, last_value = false;
    ptrdiff_t last_col = -1;

    #define FF_EMIT(t, v, st, a, z, fl) do { \
        if (n >= cap) return -1; \
        oyl_event *e = &out[n++]; \
        e->type = (t); \
        e->value = (v); \
        e->anchor = OYL_STR_NULL; \
        e->tag = OYL_STR_NULL; \
        e->scalar_style = (st); \
        e->implicit = false; \
        e->flow = (fl); \
        e->start = (oyl_mark){(a), line, (a) - col1 + 1}; \
        e->end = (oyl_mark){(z), line, (z) - col1 + 1}; \
    } while (0)

    map[0] = open->type == OYL_TOK_FLOW_MAP_START;
    FF_EMIT(map[0] ? OYL_EVT_MAPPING_START : OYL_EVT_SEQUENCE_START, OYL_STR_NULL,
            OYL_SCALAR_PLAIN, open->start.offset, open->end.offset, true);
    size_t i = s->pos;
    for (;;) {
        while (i < len && (b[i] == ' ' || b[i] == '\t')) i++;
        if (i >= len) return -1;
        uint8_t c = (uint8_t)b[i];
        switch (state) {
        case ITEM:
        case VALUE:
            if (c == '}' || c == ']') {
                if (state == VALUE) return -1;   /* an empty value */
                goto close;                      /* empty, or after a final ',' */
            }
            if (c == '{' || c == '[') {
                if (state == ITEM && map[depth - 1]) return -1;   /* a collection as a key */
                if (depth == FF_MAX_DEPTH) return -1;
                nested = true;
                last_value = state == VALUE;
                map[depth++] = c == '{';
                if (depth > maxd) maxd = depth;
                FF_EMIT(c == '{' ? OYL_EVT_MAPPING_START : OYL_EVT_SEQUENCE_START, OYL_STR_NULL,
                        OYL_SCALAR_PLAIN, i, i + 1, true);
                i++;
                state = ITEM;
                continue;
            }
            {
                size_t a = i, z;
                oyl_str v;
                oyl_scalar_style st;
                if (c == '"') {
                    size_t j = i + 1 + oyl_find_in_quotes_short(b + i + 1, len - i - 1, '"', '\\');
                    if (j >= len || b[j] != '"') return -1;   /* an escape, a break, a control */
                    v = (oyl_str){b + i + 1, j - i - 1};
                    st = OYL_SCALAR_DOUBLE_QUOTED;
                    z = i = j + 1;
                } else {
                    if (!(ff_alnum(c) || c == '.' || c == '_' ||
                          ((c == '-' || c == '+') && i + 1 < len && ff_plain((uint8_t)b[i + 1]))))
                        return -1;
                    size_t j = i + 1;
                    while (j < len && ff_plain((uint8_t)b[j])) j++;
                    size_t k = j;   /* the token ends past trailing blanks */
                    while (k < len && (b[k] == ' ' || b[k] == '\t')) k++;
                    if (k >= len) return -1;
                    if (b[k] == ':') {
                        /* a value indicator only before a blank; otherwise
                         * part of the scalar */
                        if (k + 1 >= len || (b[k + 1] != ' ' && b[k + 1] != '\t')) return -1;
                    } else if (b[k] != ',' && b[k] != ']' && b[k] != '}') {
                        return -1;   /* more words, a comment, other bytes */
                    }
                    v = (oyl_str){b + i, j - i};
                    st = OYL_SCALAR_PLAIN;
                    z = i = k;
                }
                last_col = (ptrdiff_t)(a - col1);
                FF_EMIT(OYL_EVT_SCALAR, v, st, a, z, false);
                state = state == ITEM && map[depth - 1] ? COLON : SEP;
                continue;
            }
        case COLON:
            if (c != ':') return -1;   /* a key without a value */
            i++;
            state = VALUE;
            continue;
        case SEP:
            if (c == ',') {
                i++;
                state = ITEM;
                continue;
            }
            if (c == '}' || c == ']') goto close;
            return -1;   /* a ':' after a sequence entry (a pair), ... */
        }
    close:
        if ((c == '}') != map[depth - 1]) return -1;
        FF_EMIT(c == '}' ? OYL_EVT_MAPPING_END : OYL_EVT_SEQUENCE_END, OYL_STR_NULL,
                OYL_SCALAR_PLAIN, i, i + 1, false);
        if (--depth == 0) break;
        i++;
        state = SEP;
    }
    #undef FF_EMIT
    ff->close = i;
    ff->depth = maxd;
    ff->nested = nested;
    ff->last_value = last_value;
    ff->last_col = last_col;
    return n;
}

void oyl_scan_flow_commit(oyl_scanner *s, const oyl_flow_fast *ff) {
    if (ff->last_col >= 0) s->last_token_col = ff->last_col;
    /* as the closing bracket's token: the collections inside it opened and
     * closed, leaving nothing behind (no props before them, no '?') */
    oyl_mark open = flow_pop(s);
    if (s->flow_level > 0) s->flow_level--;
    s->col += ff->close + 1 - s->pos;
    s->pos = ff->close + 1;
    note_node(s, open, s->pos);
    s->last_was_quoted = true;
}

/* ── Block fast path (oyl_internal.h) ──────────────────────── */

/* A plain scalar's first byte, as oyl_scan_token takes it: one it sends
 * straight to the plain scanner, or a '-' or '.' that a letter or digit
 * follows (neither a sequence entry nor a document marker). */
static inline bool bf_plain_start(const char *b, size_t i, size_t len) {
    uint8_t c = (uint8_t)b[i];
    if (c == '-' || c == '.') return i + 1 < len && ff_alnum((uint8_t)b[i + 1]);
    return c > ' ' && c != '#' && c != 0x7F && !indicator_start[c];
}

/* The first byte from `i` that isn't simply more of a plain value's text in
 * block context (spaces are, for now): a control character (a line break
 * among them), ':', '#' or DEL; len if none. */
static inline size_t bf_special(const char *b, size_t i, size_t len) {
#if defined(OYL_SSE2)
    const __m128i ctl = _mm_set1_epi8(0x1F), colon = _mm_set1_epi8(':'),
                  hash = _mm_set1_epi8('#'), del = _mm_set1_epi8(0x7F);
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(b + i));
        __m128i m = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(_mm_min_epu8(v, ctl), v), _mm_cmpeq_epi8(v, colon)),
            _mm_or_si128(_mm_cmpeq_epi8(v, hash), _mm_cmpeq_epi8(v, del)));
        int mask = _mm_movemask_epi8(m);
        if (mask) return i + (size_t)oyl_ctz32((uint32_t)mask);
    }
#endif
    for (; i < len; i++) {
        uint8_t c = (uint8_t)b[i];
        if (c < 0x20 || c == ':' || c == '#' || c == 0x7F) return i;
    }
    return len;
}

/* A plain value from `i`, where scan_plain_scalar would end it in block
 * context: at its line's break, or at a comment after blanks. It may hold
 * text, runs of spaces, and ':' not before a blank. Returns where its
 * token ends (the break, or the '#'), with *vend the end of its text
 * (before trailing spaces); SIZE_MAX at anything else (a tab, a control
 * character, the end of the input). */
static inline size_t bf_value(const char *b, size_t i, size_t len, size_t *vend, bool *comment) {
    for (;;) {
        i = bf_special(b, i, len);
        if (i >= len) return SIZE_MAX;
        uint8_t c = (uint8_t)b[i];
        if (c == '\n' || (c == '#' && b[i - 1] == ' ')) {
            size_t e = i;
            while (b[e - 1] == ' ') e--;
            *vend = e;
            *comment = c == '#';
            return i;
        }
        if (c == '#') {
            i++;   /* in the text */
        } else if (c == ':') {
            if (i + 1 >= len || (uint8_t)b[i + 1] <= ' ' || b[i + 1] == 0x7F) return SIZE_MAX;
            i++;
        } else {
            return SIZE_MAX;
        }
    }
}

/* How many spaces from `i`. */
static inline size_t bf_spaces(const char *b, size_t i, size_t len) {
    size_t from = i;
#if defined(OYL_SSE2)
    const __m128i sp = _mm_set1_epi8(' ');
    for (; i + 16 <= len; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(b + i));
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v, sp)) ^ 0xFFFFu;
        if (mask) return i + (size_t)oyl_ctz32(mask) - from;
    }
#endif
    while (i < len && b[i] == ' ') i++;
    return i - from;
}

/* From the line break at `i`, past lines of only blanks, as
 * scan_plain_scalar's look past a break goes: returns where the blanks of
 * the next line with anything else end, with *line its number (the
 * break's going in), *ls its start and *tabs whether its blanks hold a
 * tab; len at the end of the input, SIZE_MAX at a '\r'. */
static inline size_t bf_blank_lines(const char *b, size_t i, size_t len, size_t *line, size_t *ls,
                                    bool *tabs) {
    for (;;) {
        i++;
        (*line)++;
        *ls = i;
        i += bf_spaces(b, i, len);
        *tabs = i < len && b[i] == '\t';
        while (i < len && (b[i] == ' ' || b[i] == '\t')) i++;
        if (i >= len) return len;
        if (b[i] == '\r') return SIZE_MAX;
        if (b[i] != '\n') return i;
    }
}

/* Writes the events of a block mapping's plain values, and of whole
 * "key: value" lines, straight from the bytes; see oyl_internal.h. The
 * scanner is left as scanning their tokens would have left it. */
int oyl_scan_block_fast(oyl_scanner *s, oyl_block_fast_mode mode, const oyl_token *key,
                        ptrdiff_t indent, oyl_event *out, int cap, oyl_mark *colon, bool *full) {
    *full = false;
    if (s->pending_count > 0 || s->flow_level > 0 || !s->stream_started || s->indent != indent ||
        indent < 0 || cap < (mode == OYL_BF_VALUE ? 1 : 2))
        return 0;
    const char *b = s->buf;
    size_t len = s->len;
    size_t i = s->pos;
    size_t line = s->line, ls = s->pos - (s->col - 1);   /* the line and its start */
    int n = 0;

    /* what the last value taken leaves behind: its token's end (a break or
     * a '#'), its start, and how far its look past a break went */
    size_t end = 0, vstart = 0, vline = 0, vls = 0;
    bool ahead = false;
    size_t ahead_pos = 0, ahead_line = 0, ahead_ls = 0;
    oyl_mark last_colon = {0, 0, 0};

    /* a scalar [a, z), its end mark at `to` (past trailing blanks) */
    #define BF_EMIT(a, z, to) do { \
        oyl_event *ev = &out[n++]; \
        ev->type = OYL_EVT_SCALAR; \
        ev->value = (oyl_str){b + (a), (z) - (a)}; \
        ev->anchor = OYL_STR_NULL; \
        ev->tag = OYL_STR_NULL; \
        ev->scalar_style = OYL_SCALAR_PLAIN; \
        ev->implicit = false; \
        ev->flow = false; \
        ev->start = (oyl_mark){(a), line, (a) - ls + 1}; \
        ev->end = (oyl_mark){(to), line, (to) - ls + 1}; \
    } while (0)

    size_t t = 0;      /* where the next line's key may start */
    bool tabs = false; /* and whether a tab is in its indentation */
    if (mode == OYL_BF_KEY) {
        /* the key just scanned, at the mapping's indentation, and its ':'
         * right after it, as oyl_scan_token's ':' case takes it: no props
         * before the key, which is short, and no error */
        size_t ks = key->start.offset;
        if (key->scalar_style != OYL_SCALAR_PLAIN || key->end.offset != i ||
            key->start.line != line || (ptrdiff_t)(ks - ls) != indent || i + 1 >= len ||
            b[i] != ':' || b[i + 1] != ' ' || s->node_start != ks || s->node_end != i ||
            only_blanks_between(s, s->props_end, ks) || i - ks > 1024 ||
            s->key_colon_line == line || tab_after_indicator(s, ks))
            return 0;
    } else if (mode == OYL_BF_LINES) {
        /* the next line, past blank lines, after the break at the cursor */
        if (i >= len || b[i] != '\n') return 0;
        t = bf_blank_lines(b, i, len, &line, &ls, &tabs);
    }

    for (bool first = true;; first = false) {
        size_t k = 0, k_text = 0, k_end = 0;   /* a key the line has: text, ':' */
        if (mode == OYL_BF_KEY && first) {
            k = key->start.offset;
            k_end = i;
            i++;
        } else if (mode != OYL_BF_VALUE || !first) {
            /* a plain key at the mapping's indentation (spaces only), on
             * one line, its ':' the first in it */
            if (t >= len || tabs || (ptrdiff_t)(t - ls) != indent || !bf_plain_start(b, t, len)) break;
            size_t q = bf_special(b, t, len);
            if (q + 1 >= len || b[q] != ':' || b[q + 1] != ' ' || q - t > 1024) break;
            k = t;
            k_text = q;
            while (b[k_text - 1] == ' ') k_text--;
            k_end = q;
            i = q + 1;
        }
        /* the value: blanks after the ':' (spaces only), then a plain
         * scalar */
        size_t v = i;
        while (v < len && b[v] == ' ') v++;
        if (v == i || v >= len || !bf_plain_start(b, v, len)) break;
        size_t vend;
        bool comment;
        size_t e = bf_value(b, v, len, &vend, &comment);
        if (e == SIZE_MAX) break;
        /* a value ending at its line's break looks past it: it ends there if
         * the next line with anything on it is indented no more than the
         * mapping, or is a comment (a deeper line may continue it); one
         * ending at a comment doesn't look */
        bool has_ahead = false, atabs = false;
        size_t al = line, als = ls, ap = len;
        if (!comment) {
            ap = bf_blank_lines(b, e, len, &al, &als, &atabs);
            if (ap == SIZE_MAX) break;
            if (ap < len) {
                if ((ptrdiff_t)(ap - als) > indent && b[ap] != '#') break;
                has_ahead = true;
            }
        }
        if (k_end) {
            if (cap - n < 2) {
                *full = true;
                break;
            }
            if (mode == OYL_BF_KEY && first) {
                BF_EMIT(k, k_end, k_end);
                out[n - 1].value = key->value;
            } else {
                BF_EMIT(k, k_text, k_end);   /* its text without blanks before the ':' */
            }
            last_colon = (oyl_mark){k_end, line, k_end - ls + 1};
        }
        BF_EMIT(v, vend, e);
        end = e;
        vstart = v;
        vline = line;
        vls = ls;
        ahead = has_ahead;
        ahead_pos = ap;
        ahead_line = al;
        ahead_ls = als;
        /* go on with the next line (not past a comment) */
        if (comment || !has_ahead) break;
        t = ap;
        tabs = atabs;
        line = al;
        ls = als;
    }
    #undef BF_EMIT
    if (n == 0) return 0;

    /* as the last value's token left it: at its end, having looked past
     * its break (scan_plain_scalar's END_AT_BREAK) unless the input ends
     * or a comment ended it */
    s->pos = end;
    s->line = vline;
    s->col = end - vls + 1;
    if (ahead) {
        s->ahead_from = end;
        s->ahead_pos = ahead_pos;
        s->ahead_line = ahead_line;
        s->ahead_col = ahead_pos - ahead_ls + 1;
        s->ahead_line_start = ahead_ls;
    } else {
        s->ahead_from = SIZE_MAX;
    }
    s->last_token_col = (ptrdiff_t)(vstart - vls);
    s->last_was_quoted = false;
    note_node(s, (oyl_mark){vstart, vline, vstart - vls + 1}, end);
    if (last_colon.line) {
        /* and the last key's ':' */
        s->key_colon_line = last_colon.line;
        s->indicator_end = last_colon.offset + 1;
        *colon = last_colon;
    }
    return n;
}

const char *oyl_scanner_error(oyl_scanner *s) {
    if (!s || s->error_msg[0] == '\0') return NULL;
    return s->error_msg;
}

oyl_mark oyl_scanner_error_mark(oyl_scanner *s) {
    if (!s) return (oyl_mark){0, 0, 0};
    return s->error_mark;
}

/* Set the document's syntax leniencies (OYL_SYNTAX_), and where to warn
 * when one changes the result */
void oyl_scanner_set_syntax(oyl_scanner *s, unsigned syntax, oyl_warn_state *w) {
    s->syntax = syntax;
    s->warn = w;
}

/* Copy src's state into dst, a scanner over the same input: a checkpoint
 * the parser can rewind to. dst keeps its own stack buffers, growing them
 * as needed; if one can't grow, dst's state is unchanged and this returns
 * false. */
bool oyl_scanner_copy(oyl_scanner *dst, const oyl_scanner *src) {
    if (dst->indents.cap < src->indents.len) {
        ptrdiff_t *d = realloc(dst->indents.data, (size_t)src->indents.cap * sizeof *d);
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
