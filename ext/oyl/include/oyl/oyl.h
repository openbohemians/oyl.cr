/**
 * @file oyl.h
 * @brief oyl — YAML 1.2 parser/emitter
 *
 * Zero-copy, SIMD-accelerated, arena-allocated.
 * Spec: https://yaml.org/spec/1.2.2/
 */

#ifndef OYL_H
#define OYL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Symbol export ───────────────────────────────────────── */

/* The shared library is built with hidden visibility; only declarations
 * marked OYL_API are exported. */
#if defined(__GNUC__) || defined(__clang__)
#  define OYL_API __attribute__((visibility("default")))
#else
#  define OYL_API
#endif

/* ── ABI ─────────────────────────────────────────────────────
 * Structs whose fields are listed in this header (oyl_token, oyl_event)
 * are only ever allocated by the library and handed out by const pointer,
 * so fields can be appended in later versions without breaking compiled
 * callers. Configuration and schemas are opaque, set through functions.
 * oyl_str and oyl_mark are plain value types and will not change. */

/* ── Version ─────────────────────────────────────────────── */

#define OYL_VERSION_MAJOR 1
#define OYL_VERSION_MINOR 0
#define OYL_VERSION_PATCH 0

/* ── String view (zero-copy reference into source) ───────── */

/** Non-owning reference to a UTF-8 string. Points into the input buffer
 *  or arena-allocated memory; valid until the arena is freed/reset. */
typedef struct {
    const char *data;   /**< Pointer to string bytes (not NUL-terminated). */
    size_t      len;    /**< Length in bytes. */
} oyl_str;

/** Null/empty string view. */
#define OYL_STR_NULL ((oyl_str){NULL, 0})

/** Create a oyl_str from a string literal. */
#define OYL_STR_LIT(s) ((oyl_str){(s), sizeof(s) - 1})

/* ── Error codes ─────────────────────────────────────────── */

/** Status codes returned by parser, scanner, and emitter functions. */
typedef enum {
    OYL_OK = 0,         /**< Success. */
    OYL_ERR_MEMORY,     /**< Allocation failure. */
    OYL_ERR_INPUT,      /**< Invalid input (e.g. NULL pointer). */
    OYL_ERR_SCAN,       /**< Scanner error (malformed YAML). */
    OYL_ERR_PARSE,      /**< Parser error (structural YAML error). */
    OYL_ERR_EMIT,       /**< Emitter error (invalid event sequence). */
    OYL_ERR_LIMIT,      /**< A safety limit was exceeded (event count,
                             nesting depth, or alias expansion). */
} oyl_status;

/* ── Source location ─────────────────────────────────────── */

/** Byte-level position within the input buffer. */
typedef struct {
    size_t offset;  /**< Byte offset from start of input. */
    size_t line;    /**< 1-based line number. */
    size_t col;     /**< 1-based column in bytes. */
} oyl_mark;

/* ── Token types (scanner output) ────────────────────────── */

/** Token types produced by the scanner. */
typedef enum {
    OYL_TOK_NONE = 0,

    /* structural */
    OYL_TOK_STREAM_START,
    OYL_TOK_STREAM_END,
    OYL_TOK_DOC_START,       /**< @c --- */
    OYL_TOK_DOC_END,         /**< @c ... */

    /* indicators */
    OYL_TOK_BLOCK_SEQ_ENTRY, /**< @c - */
    OYL_TOK_BLOCK_MAP_KEY,   /**< @c ? */
    OYL_TOK_BLOCK_MAP_VALUE, /**< @c : */
    OYL_TOK_FLOW_SEQ_START,  /**< @c [ */
    OYL_TOK_FLOW_SEQ_END,    /**< @c ] */
    OYL_TOK_FLOW_MAP_START,  /**< @c { */
    OYL_TOK_FLOW_MAP_END,    /**< @c } */
    OYL_TOK_FLOW_ENTRY,      /**< @c , */

    /* content */
    OYL_TOK_SCALAR,
    OYL_TOK_TAG,             /**< @c !tag or @c !!type */
    OYL_TOK_ANCHOR,          /**< @c &name */
    OYL_TOK_ALIAS,           /**< @c *name */
    OYL_TOK_DIRECTIVE,       /**< @c %YAML / @c %TAG line; value is the whole line */
} oyl_token_type;

/* ── Scalar style ────────────────────────────────────────── */

/** How a scalar was (or should be) represented in YAML text. */
typedef enum {
    OYL_SCALAR_PLAIN,          /**< Unquoted. */
    OYL_SCALAR_SINGLE_QUOTED,  /**< @c 'single' */
    OYL_SCALAR_DOUBLE_QUOTED,  /**< @c "double" */
    OYL_SCALAR_LITERAL,        /**< Block literal @c | */
    OYL_SCALAR_FOLDED,         /**< Block folded @c > */
} oyl_scalar_style;

/* ── Token ───────────────────────────────────────────────── */

/** A single lexical token from the scanner. Tokens are owned by the
 *  scanner; see oyl_scan_next(). */
typedef struct {
    oyl_token_type  type;
    oyl_str         value;        /**< Scalar/tag/anchor text. */
    oyl_scalar_style scalar_style; /**< Only meaningful for OYL_TOK_SCALAR. */
    oyl_mark        start;        /**< Position of first byte. */
    oyl_mark        end;          /**< Position past last byte. */
} oyl_token;

/* ── Event types (parser output) ─────────────────────────── */

/** High-level event types produced by the parser.
 *  Events arrive in a well-formed sequence:
 *  STREAM_START (DOC_START node DOC_END)* STREAM_END,
 *  where @e node is a scalar, alias, or collection (mapping/sequence). */
typedef enum {
    OYL_EVT_NONE = 0,
    OYL_EVT_STREAM_START,
    OYL_EVT_STREAM_END,
    OYL_EVT_DOC_START,
    OYL_EVT_DOC_END,
    OYL_EVT_MAPPING_START,
    OYL_EVT_MAPPING_END,
    OYL_EVT_SEQUENCE_START,
    OYL_EVT_SEQUENCE_END,
    OYL_EVT_SCALAR,
    OYL_EVT_ALIAS,
} oyl_event_type;

/* ── Event ───────────────────────────────────────────────── */

/** A parsed YAML event. Events are owned by the parser; see
 *  oyl_parse_next(). String fields point into the input buffer or the
 *  arena and remain valid until those are freed or reset. */
typedef struct {
    oyl_event_type   type;
    oyl_str          value;       /**< Scalar value or alias name. */
    oyl_str          anchor;      /**< Anchor name (@c &name) if present. */
    oyl_str          tag;         /**< Tag (@c !tag) if present. */
    oyl_scalar_style scalar_style;
    bool             implicit;    /**< True for implicit doc start/end. */
    bool             flow;        /**< True for flow collections ({} / []). */
    oyl_mark         start;
    oyl_mark         end;
} oyl_event;

/* ── Arena allocator ─────────────────────────────────────── */

/** Opaque bump allocator. All memory allocated from an arena is freed in
 *  one call to oyl_arena_free(). No per-object deallocation needed. */
typedef struct oyl_arena oyl_arena;

/** Create a new arena with the given initial block capacity (min 4096). */
OYL_API oyl_arena  *oyl_arena_new(size_t initial_cap);

/** Allocate @p size bytes with @p align alignment from the arena.
 *  @p align must be a power of two (0 means 1). Returns NULL if it isn't,
 *  if the size is too large, or if memory runs out; the arena remains
 *  usable after a failed allocation. */
OYL_API void       *oyl_arena_alloc(oyl_arena *a, size_t size, size_t align);

/** Duplicate @p len bytes from @p src into the arena (NUL-terminated). */
OYL_API char       *oyl_arena_dup(oyl_arena *a, const char *src, size_t len);

/** Reset the arena for reuse, keeping the largest block allocated. */
OYL_API void        oyl_arena_reset(oyl_arena *a);

/** Free the arena and all memory allocated from it. */
OYL_API void        oyl_arena_free(oyl_arena *a);

/* ── File input ─────────────────────────────────────────── */

/** Read an entire file (or pipe) into the arena. Returns a oyl_str with
 *  .data=NULL on failure (including when @p path is a directory), with
 *  errno describing the cause. An empty file gives .len=0 and non-NULL
 *  .data. The buffer is not NUL-terminated. */
OYL_API oyl_str     oyl_read_file(const char *path, oyl_arena *a);

/* ── Scanner ─────────────────────────────────────────────── */

/** Opaque low-level tokenizer. Most users should use the parser instead. */
typedef struct oyl_scanner oyl_scanner;

/** Create a scanner over the given input buffer. The buffer must remain
 *  valid for the scanner's lifetime and is not copied. */
OYL_API oyl_scanner *oyl_scanner_new(const char *input, size_t len, oyl_arena *a);

/** Retrieve the next token. On OYL_OK, @p *tok points to a token owned by
 *  the scanner, valid until the next call or oyl_scanner_free(); on error
 *  it is set to NULL. Returns OYL_OK, OYL_ERR_SCAN on malformed input, or
 *  OYL_ERR_MEMORY on allocation failure. */
OYL_API oyl_status   oyl_scan_next(oyl_scanner *s, const oyl_token **tok);

/** Error message from the last failed scan, or NULL. */
OYL_API const char  *oyl_scanner_error(oyl_scanner *s);

/** Source location of the last scan error. */
OYL_API oyl_mark     oyl_scanner_error_mark(oyl_scanner *s);

/** Free the scanner (does not free the arena). */
OYL_API void         oyl_scanner_free(oyl_scanner *s);

/* ── Tag constants ───────────────────────────────────────── */

OYL_API extern const oyl_str OYL_TAG_NULL;    /**< tag:yaml.org,2002:null  */
OYL_API extern const oyl_str OYL_TAG_BOOL;    /**< tag:yaml.org,2002:bool  */
OYL_API extern const oyl_str OYL_TAG_INT;     /**< tag:yaml.org,2002:int   */
OYL_API extern const oyl_str OYL_TAG_FLOAT;   /**< tag:yaml.org,2002:float */
OYL_API extern const oyl_str OYL_TAG_STR;     /**< tag:yaml.org,2002:str   */
OYL_API extern const oyl_str OYL_TAG_SEQ;     /**< tag:yaml.org,2002:seq   */
OYL_API extern const oyl_str OYL_TAG_MAP;     /**< tag:yaml.org,2002:map   */
OYL_API extern const oyl_str OYL_TAG_MERGE;   /**< tag:yaml.org,2002:merge */

/* ── Schema ──────────────────────────────────────────────── */

/** How a schema rule matches a plain scalar value. */
typedef enum {
    OYL_MATCH_EXACT,     /**< strcmp match. */
    OYL_MATCH_ICASE,     /**< Case-insensitive match. */
    OYL_MATCH_BUILTIN,   /**< Procedural matcher (int, float). */
} oyl_match_type;

/** Opaque tag schema for resolving plain scalars to typed tags. Use one
 *  of the presets or build a custom schema. */
typedef struct oyl_schema oyl_schema;

/** YAML 1.2 Failsafe schema: everything is !!str / !!seq / !!map.
 *  Presets are static and never need freeing. */
OYL_API const oyl_schema *oyl_schema_failsafe(void);

/** YAML 1.2 JSON schema: null, true/false, integers, floats. */
OYL_API const oyl_schema *oyl_schema_json(void);

/** YAML 1.2 Core schema: JSON + Null/NULL/~, True/TRUE, 0x/0o ints, etc. */
OYL_API const oyl_schema *oyl_schema_core(void);

/** Resolve the tag of a scalar with the given value and style: quoted
 *  scalars are strings, plain ones are matched against the schema's rules. */
OYL_API oyl_str    oyl_schema_resolve(const oyl_schema *schema, oyl_str value,
                                      oyl_scalar_style style);

/* ── Schema builder ──────────────────────────────────────── */

/** Opaque builder for constructing custom tag schemas. */
typedef struct oyl_schema_builder oyl_schema_builder;

/** Create a new schema builder (allocates from the arena). */
OYL_API oyl_schema_builder *oyl_schema_builder_new(oyl_arena *a);

/** Add a tag resolution rule. */
OYL_API void    oyl_schema_builder_add(oyl_schema_builder *b,
                               oyl_match_type match,
                               const char *pattern, oyl_str tag);

/** Add boolean resolution rules (e.g. "true"/"yes" -> !!bool). */
OYL_API void    oyl_schema_builder_add_bools(oyl_schema_builder *b,
                                     const char **true_terms, int ntrue,
                                     const char **false_terms, int nfalse);

/** Add null resolution rules (e.g. "null"/"~" -> !!null). */
OYL_API void    oyl_schema_builder_add_nulls(oyl_schema_builder *b,
                                     const char **terms, int nterms);

/** Add the built-in integer matcher (decimal, hex, octal). */
OYL_API void    oyl_schema_builder_add_int(oyl_schema_builder *b);

/** Add the built-in float matcher (decimal, .inf, .nan). */
OYL_API void    oyl_schema_builder_add_float(oyl_schema_builder *b);

/** Finalize and return the schema, allocated in the builder's arena (it
 *  lives until the arena is freed). The builder can be freed after this.
 *  Returns NULL on allocation failure. */
OYL_API const oyl_schema *oyl_schema_builder_finish(oyl_schema_builder *b);

/** Free the schema builder. */
OYL_API void    oyl_schema_builder_free(oyl_schema_builder *b);

/* ── Parser ──────────────────────────────────────────────── */

/** Opaque event parser. Consumes tokens from the scanner and produces
 *  a well-formed stream of events. */
typedef struct oyl_parser oyl_parser;

/** Create a parser over the given input buffer. The buffer must remain
 *  valid for the parser's lifetime and is not copied.
 *  @return Parser instance, or NULL on allocation failure. */
OYL_API oyl_parser *oyl_parser_new(const char *input, size_t len, oyl_arena *a);

/** Retrieve the next event. On OYL_OK, @p *evt points to an event owned
 *  by the parser, valid until the next call or oyl_parser_free(); copy it
 *  (or the fields you need) to keep it longer. On error @p *evt is set to
 *  NULL. After OYL_EVT_STREAM_END every call returns a OYL_EVT_NONE event.
 *
 *  Events are produced incrementally as input is consumed, except when
 *  merge keys, alias resolution, a schema, directives, or node properties
 *  require looking at a whole document first. Such documents are parsed
 *  one at a time, so memory follows the largest document, not the stream.
 *  @return OYL_OK on success, or an error status; oyl_parser_error() and
 *          oyl_parser_error_mark() describe the error. */
OYL_API oyl_status  oyl_parse_next(oyl_parser *p, const oyl_event **evt);

/** Set a tag schema for automatic tag resolution on scalars. The schema
 *  must outlive the parser. */
OYL_API void        oyl_parser_set_schema(oyl_parser *p, const oyl_schema *schema);

/** Enable/disable merge key (@c <<) expansion. Disabled by default.
 *  A merge value must be a mapping, an alias to one, or a sequence of
 *  those; anything else is a OYL_ERR_PARSE. */
OYL_API void        oyl_parser_set_merge(oyl_parser *p, bool enable);

/** Enable/disable alias resolution (inline expansion of @c *alias
 *  references). Disabled by default. An alias refers to the most recent
 *  anchor of that name before it in the same document. Cyclic aliases,
 *  and aliases with no preceding anchor, are kept as OYL_EVT_ALIAS
 *  events. */
OYL_API void        oyl_parser_set_resolve(oyl_parser *p, bool enable);

/** Set the maximum number of events before the parser stops with an error.
 *  Default is 10,000. Set to 0 to disable the limit.
 *  Exceeding it returns OYL_ERR_LIMIT; alias/merge expansion is bounded
 *  by the same limit. @see README "Safety Limits" for sizing guidance. */
OYL_API void        oyl_parser_set_max_events(oyl_parser *p, int max);

/** Set the maximum nesting depth of collections. Exceeding it stops the
 *  parser with OYL_ERR_LIMIT. Default is 256. It applies to the events
 *  delivered, so it also bounds alias and merge expansion, which can nest
 *  deeper than the input. Set to 0 to disable the
 *  limit, but note that some inputs (tags, anchors, merge keys, alias
 *  resolution) are parsed recursively, using roughly 1 KB of stack per
 *  level, so very deep input can then overflow the stack. */
OYL_API void        oyl_parser_set_max_depth(oyl_parser *p, int max);

/** Error message from the last failed parse, or NULL. */
OYL_API const char *oyl_parser_error(oyl_parser *p);

/** Source location of the last parse error. */
OYL_API oyl_mark    oyl_parser_error_mark(oyl_parser *p);

/** Free the parser (does not free the arena). */
OYL_API void        oyl_parser_free(oyl_parser *p);

/* ── Emitter ─────────────────────────────────────────────── */

/** Opaque YAML emitter. Feed it events to produce YAML text. */
typedef struct oyl_emitter oyl_emitter;

/** Output style for the emitter. */
typedef enum {
    OYL_EMIT_BLOCK,      /**< Default block style (indented). */
    OYL_EMIT_FLOW,       /**< Flow style ({} / []). */
    OYL_EMIT_MINIMAL,    /**< Minimal whitespace. */
} oyl_emit_style;

/** Create an emitter: block style, 2-space indent. Output is written to
 *  an internal buffer retrievable with oyl_emitter_output().
 *  @return Emitter instance, or NULL on allocation failure. */
OYL_API oyl_emitter *oyl_emitter_new(oyl_arena *a);

/** Set the output style (default OYL_EMIT_BLOCK). */
OYL_API void         oyl_emitter_set_style(oyl_emitter *e, oyl_emit_style style);

/** Set the spaces per indentation level, 1-10 (default 2). */
OYL_API void         oyl_emitter_set_indent(oyl_emitter *e, int indent);

/* Events are fed in the same well-formed order the parser produces:
 * STREAM_START (DOC_START node DOC_END)* STREAM_END. Anchor and tag
 * arguments may be OYL_STR_NULL. Tags are full tags (e.g.
 * "tag:yaml.org,2002:str", written as !!str) or local tags ("!foo"). */

/** Re-emit an event obtained from oyl_parse_next() (e.g. to reformat a
 *  document). To build output yourself, use the functions below. */
OYL_API oyl_status   oyl_emit(oyl_emitter *e, const oyl_event *evt);

OYL_API oyl_status   oyl_emit_stream_start(oyl_emitter *e);
OYL_API oyl_status   oyl_emit_stream_end(oyl_emitter *e);

/** Start a document; @p implicit omits the "---" marker. */
OYL_API oyl_status   oyl_emit_document_start(oyl_emitter *e, bool implicit);

/** End a document; @p implicit omits the "..." marker. */
OYL_API oyl_status   oyl_emit_document_end(oyl_emitter *e, bool implicit);

/** Emit a scalar. OYL_SCALAR_PLAIN writes the value plain when that reads
 *  back as the same text, and quotes it otherwise; the other styles are
 *  honored where valid (block styles fall back to double-quoted in flow
 *  context). */
OYL_API oyl_status   oyl_emit_scalar(oyl_emitter *e, oyl_str value,
                                     oyl_scalar_style style,
                                     oyl_str anchor, oyl_str tag);

/** Emit an alias (@c *name). */
OYL_API oyl_status   oyl_emit_alias(oyl_emitter *e, oyl_str name);

/** Start a mapping; @p flow forces flow style ({...}) for it. */
OYL_API oyl_status   oyl_emit_mapping_start(oyl_emitter *e, oyl_str anchor,
                                            oyl_str tag, bool flow);
OYL_API oyl_status   oyl_emit_mapping_end(oyl_emitter *e);

/** Start a sequence; @p flow forces flow style ([...]) for it. */
OYL_API oyl_status   oyl_emit_sequence_start(oyl_emitter *e, oyl_str anchor,
                                             oyl_str tag, bool flow);
OYL_API oyl_status   oyl_emit_sequence_end(oyl_emitter *e);

/** Retrieve the emitter's output buffer. Valid until the arena is freed. */
OYL_API oyl_str      oyl_emitter_output(oyl_emitter *e);

/** Free the emitter (does not free the arena). */
OYL_API void         oyl_emitter_free(oyl_emitter *e);

/* ── Convenience ─────────────────────────────────────────── */

/** Return a human-readable name for a status code. */
OYL_API const char *oyl_status_str(oyl_status s);

/** Return a human-readable name for a token type. */
OYL_API const char *oyl_token_type_str(oyl_token_type t);

/** Return a human-readable name for an event type. */
OYL_API const char *oyl_event_type_str(oyl_event_type t);

#ifdef __cplusplus
}
#endif

#endif /* OYL_H */
