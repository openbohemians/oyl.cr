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
 * oyl_str and oyl_mark are plain value types and will not change, nor
 * will the layouts of a tree's arrays (oyl_node, oyl_node_props,
 * oyl_pair), which programs and bindings index directly. */

/* ── Version ─────────────────────────────────────────────── */

#define OYL_VERSION_MAJOR 1
#define OYL_VERSION_MINOR 4
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
    bool             implicit;    /**< True for an implicit doc start/end, and for a
                                       node whose tag the parser's schema gave
                                       (oyl_parser_set_schema), not one written. */
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
 *  valid for the scanner's lifetime and is not copied. NULL with @p len 0
 *  is empty input. */
OYL_API oyl_scanner *oyl_scanner_new(const char *input, size_t len, oyl_arena *a);

/** Retrieve the next token. On OYL_OK, @p *tok points to a token owned by
 *  the scanner, valid until the next call or oyl_scanner_free(); on error
 *  it is set to NULL. Returns OYL_OK, OYL_ERR_SCAN on malformed input, or
 *  OYL_ERR_MEMORY on allocation failure. An error is final: every later
 *  call returns it again, with the same oyl_scanner_error() and
 *  oyl_scanner_error_mark(). */
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

/** YAML 1.2 JSON schema: null, true/false, integers, floats. Its numbers
 *  follow the Core schema's rules, so it accepts more than the spec's JSON
 *  schema (+1, 01, 0x1F, 0o17, .inf, .nan), and other plain scalars resolve
 *  to !!str rather than being an error. */
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

/* ── Scalar values ───────────────────────────────────────── */

/* A schema binds each tag to a parser. A plain scalar takes the tag of the
 * first rule whose parser accepts it, and that parser gives its value. */

/** The kind of a scalar's value. Kinds may be added at the end; treat one
 *  you don't know as text. */
typedef enum {
    OYL_VALUE_NULL,
    OYL_VALUE_BOOL,
    OYL_VALUE_INT,       /**< fits int64_t */
    OYL_VALUE_UINT,      /**< above INT64_MAX, fits uint64_t */
    OYL_VALUE_FLOAT,
    OYL_VALUE_STR,       /**< text: strings, timestamps, an int beyond 64 bits */
} oyl_value_kind;

/** A scalar's value. @c as.s points into the scalar's text or the
 *  parser's memory, valid as long as the event's value. */
typedef struct {
    oyl_value_kind kind;
    union {
        bool     b;
        int64_t  i;
        uint64_t u;
        double   f;
        oyl_str  s;
    } as;
} oyl_value;

/** A custom scalar parser: accept @p text by filling @p out and returning
 *  true, or decline by returning false. */
typedef bool (*oyl_scalar_parser)(oyl_str text, oyl_value *out, void *user);

/** Options for the built-in int parser. Decimal digits are always
 *  accepted. */
enum {
    OYL_INT_SIGN         = 1 << 0,  /**< + or - before decimal digits */
    OYL_INT_HEX          = 1 << 1,  /**< 0x1F */
    OYL_INT_OCT          = 1 << 2,  /**< 0o17 */
    OYL_INT_OCT_ZERO     = 1 << 3,  /**< 017: a leading zero means octal */
    OYL_INT_BIN          = 1 << 4,  /**< 0b101 */
    OYL_INT_PREFIX_CASE  = 1 << 5,  /**< 0X, 0O and 0B as well */
    OYL_INT_SIGN_PREFIX  = 1 << 6,  /**< a sign before a prefix: -0x1F */
    OYL_INT_UNDERSCORE   = 1 << 7,  /**< underscores anywhere but first are ignored: 1_000 */
    OYL_INT_RANGE        = 1 << 8,  /**< decline a value beyond 64 bits (else it is text) */
    OYL_INT_DIGIT_UNDERSCORE = 1 << 9,  /**< underscores after the first digit or a prefix:
                                             1_000, 0x_1F (YAML 1.1) */
    OYL_INT_BASE60       = 1 << 10, /**< 1:20 is 80, 190:20:30 is 685230 (YAML 1.1); the
                                         sign, underscore and range options apply */
    OYL_INT_BIN_SIGN     = 1 << 11, /**< a sign after a lowercase 0b with none before it:
                                         0b-101 is -5 (go-yaml v2) */
    /** The Core schema's ints (with a sign also allowed before 0x or 0o) */
    OYL_INT_CORE = OYL_INT_SIGN | OYL_INT_HEX | OYL_INT_OCT | OYL_INT_SIGN_PREFIX,
    /** YAML 1.1's ints (yaml.org/type/int) */
    OYL_INT_YAML11 = OYL_INT_SIGN | OYL_INT_HEX | OYL_INT_OCT_ZERO | OYL_INT_BIN |
                     OYL_INT_SIGN_PREFIX | OYL_INT_DIGIT_UNDERSCORE | OYL_INT_BASE60,
};

/** Options for the built-in float parser, which always accepts
 *  [-+]?(.D+|D+(.D*)?)([eE][-+]?D+)? with a dot or an exponent. */
enum {
    OYL_FLOAT_SPECIALS   = 1 << 0,  /**< [-+]?.inf, .Inf, .INF; .nan, .NaN, .NAN */
    OYL_FLOAT_DIGITS     = 1 << 1,  /**< digits alone, as when an int is too big */
    OYL_FLOAT_UNDERSCORE = 1 << 2,  /**< underscores, as go-yaml v2: anywhere after a leading
                                         digit or sign, else only between digits */
    OYL_FLOAT_RANGE      = 1 << 3,  /**< decline a value too large for a double */
    OYL_FLOAT_NEED_DOT   = 1 << 4,  /**< a dot is required: 1e3 is not a float (YAML 1.1) */
    OYL_FLOAT_EXP_SIGN   = 1 << 5,  /**< an exponent needs a sign: 1.0e+3 (YAML 1.1) */
    OYL_FLOAT_DIGIT_UNDERSCORE = 1 << 6,  /**< underscores after the first digit and in the
                                               fraction, not the exponent (YAML 1.1) */
    OYL_FLOAT_BASE60     = 1 << 7,  /**< 1:20.5 is 80.5 (YAML 1.1) */
    OYL_FLOAT_DOT_NO_SIGN = 1 << 8, /**< no sign before a leading dot: .5 but not -.5
                                         (PyYAML) */
    /** The Core schema's floats. Digits alone count for an explicit
     *  !!float; untagged, the int rule takes them first. */
    OYL_FLOAT_CORE = OYL_FLOAT_SPECIALS | OYL_FLOAT_DIGITS,
    /** YAML 1.1's floats, as PyYAML reads them (yaml.org/type/float) */
    OYL_FLOAT_YAML11 = OYL_FLOAT_SPECIALS | OYL_FLOAT_NEED_DOT | OYL_FLOAT_EXP_SIGN |
                       OYL_FLOAT_DIGIT_UNDERSCORE | OYL_FLOAT_BASE60 | OYL_FLOAT_DOT_NO_SIGN,
};

/** Add the built-in int parser with the given OYL_INT_ options. */
OYL_API void    oyl_schema_builder_add_int_flags(oyl_schema_builder *b, unsigned flags);

/** Add the built-in float parser with the given OYL_FLOAT_ options. */
OYL_API void    oyl_schema_builder_add_float_flags(oyl_schema_builder *b, unsigned flags);

/** Bind @p tag to a custom parser. @p user is passed to it; it must
 *  outlive the schema. */
OYL_API void    oyl_schema_builder_add_type(oyl_schema_builder *b, oyl_str tag,
                                    oyl_scalar_parser parse, void *user);

/** The value of a scalar event. An untagged scalar (no schema on the
 *  parser) is resolved first; a tagged one is parsed by its tag's rules.
 *  Returns OYL_ERR_PARSE when none of them accepts it, as for
 *  @c !!int "abc" (or for a YAML 1.1 timestamp that doesn't exist, see
 *  oyl_schema_yaml11), and OYL_ERR_INPUT if @p scalar isn't a scalar event. A
 *  tag with no rules (!!str, !foo) gives the text. YAML 1.1's and go-yaml
 *  v2's schemas read a tagged scalar as their libraries do (see them).
 *  Floats are read alike in any locale, and need no memory. */
OYL_API oyl_status oyl_schema_value(const oyl_schema *schema, const oyl_event *scalar,
                                    oyl_value *out);

/** As oyl_schema_value(), for a scalar given by its parts: its @p text,
 *  @p style and @p tag (empty for untagged). @c out->as.s points into
 *  @p text. For bindings, which shouldn't make events of their own. */
OYL_API oyl_status oyl_schema_value_of(const oyl_schema *schema, oyl_str text,
                                       oyl_scalar_style style, oyl_str tag, oyl_value *out);

/** YAML 1.1's types as PyYAML reads them, PyYAML being the usual loader
 *  on libyaml: yes/no/on/off booleans (not y/n, which yaml.org/type also
 *  lists), 0b binary, 0-octal, base 60, underscores, floats with a dot
 *  (.5, but not -.5), timestamps, << as !!merge and = as !!value. An
 *  explicit tag reads its scalar as PyYAML's constructor for the type
 *  does, more leniently than resolution: !!float 1 is 1.0, !!int 0o17 is
 *  15, !!bool YeS is true, and anything tagged !!null is null. A timestamp
 *  whose date or time doesn't exist, as 2001-02-29, has no value, tagged
 *  or not (OYL_ERR_PARSE): PyYAML refuses it. */
OYL_API const oyl_schema *oyl_schema_yaml11(void);

/** go-yaml v2's types, as Kubernetes reads YAML: yes/no/on/off and y/n
 *  booleans, 0777 octal, 0b binary (0b-101 too), underscores in numbers,
 *  an int beyond 64 bits as a float, timestamps. An explicit !!float
 *  reads its scalar as if untagged and takes an int as a float, as
 *  go-yaml v2 does (!!float 017 is 15). Like go-yaml v2 it gives << no
 *  merge tag; merge keys are the parser's option (oyl_parser_set_merge). */
OYL_API const oyl_schema *oyl_schema_goyaml2(void);

/** The tag YAML gives timestamps. */
OYL_API extern const oyl_str OYL_TAG_TIMESTAMP;   /**< tag:yaml.org,2002:timestamp */

/** The tag of YAML 1.1's = key. */
OYL_API extern const oyl_str OYL_TAG_VALUE;       /**< tag:yaml.org,2002:value */

/** The tag of base64 data, which oyl_parse_json decodes. */
OYL_API extern const oyl_str OYL_TAG_BINARY;      /**< tag:yaml.org,2002:binary */

/* ── Parser ──────────────────────────────────────────────── */

/** Opaque event parser. Consumes tokens from the scanner and produces
 *  a well-formed stream of events. */
typedef struct oyl_parser oyl_parser;

/** Create a parser over the given input buffer. The buffer must remain
 *  valid for the parser's lifetime and is not copied. NULL with @p len 0
 *  is empty input.
 *  @return Parser instance, or NULL on allocation failure (or NULL input
 *          with a length). */
OYL_API oyl_parser *oyl_parser_new(const char *input, size_t len, oyl_arena *a);

/** Retrieve the next event. On OYL_OK, @p *evt points to an event owned
 *  by the parser, valid until the next call or oyl_parser_free(); copy it
 *  (or the fields you need) to keep it longer. On error @p *evt is set to
 *  NULL. After OYL_EVT_STREAM_END every call returns a OYL_EVT_NONE event.
 *  An error is final: every later call returns it again, with the same
 *  oyl_parser_error(), as do oyl_parse_tree() and oyl_parse_json(),
 *  whose own errors count too. The events delivered before it stand.
 *
 *  Events are produced incrementally as input is consumed, except when
 *  merge keys, alias resolution, a schema, directives, or node properties
 *  require looking at a whole document first. Such documents are parsed
 *  one at a time, so memory follows the largest document, not the stream.
 *  @return OYL_OK on success, or an error status; oyl_parser_error() and
 *          oyl_parser_error_mark() describe the error. */
OYL_API oyl_status  oyl_parse_next(oyl_parser *p, const oyl_event **evt);

/** Set a tag schema for automatic tag resolution on scalars. The schema
 *  must outlive the parser. Without a default dialect, it types documents
 *  with no %YAML or with %YAML 1.2 (see Dialects). Every node's event then
 *  has a tag; one the schema gave is marked implicit. */
OYL_API void        oyl_parser_set_schema(oyl_parser *p, const oyl_schema *schema);

/** Enable/disable merge key (@c <<) expansion. Disabled by default.
 *  A merge value must be a mapping, an alias to one, or a sequence of
 *  those, also through an alias; anything else is a OYL_ERR_PARSE. Keys
 *  written in the mapping win, then the merges as YAML's merge orders
 *  them (OYL_MERGE_YAML), keys compared by tag and value in the
 *  document's schema (0x1 is 1, "1" isn't). A << whose value contains
 *  the mapping is an ordinary key. */
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

/* ── Dialects and warnings ───────────────────────────────── */

/* A dialect is what varies between YAML worlds: a schema (types and
 * values), syntax leniencies, and how << merges. Each is a dialect of one
 * YAML version. A document chooses its dialect with directives, among
 * those the program allows:
 *   1. %TAG !! <prefix>: the dialect registered for that prefix sets the
 *      types of explicit !! tags and of untagged values
 *   2. %YAML <version>: the program's dialect for that version: the one
 *      registered for it, else the default if it is of that version (so
 *      %YAML 1.1 keeps go-yaml v2, and %YAML 1.2 the schema set with
 *      oyl_parser_set_schema), else the built-in one; other 1.x versions
 *      read as 1.2, with a warning
 *   3. otherwise the parser's default dialect
 * The document picks the version, the program the dialect for each.
 * (YAML 1.2 applies a %TAG !! prefix to explicit tags only; Oyl applies it
 * to untagged values as well, the intent of !! in YAML 1.2's design.) */

typedef struct oyl_dialect oyl_dialect;

/** Syntax leniencies a dialect may take on: each is an error, or reads
 *  differently, in YAML 1.2. */
enum {
    OYL_SYNTAX_SQUOTE_ESCAPE = 1 << 0,  /**< \' in double quotes is ' (go-yaml v2) */
    OYL_SYNTAX_EOF_NO_BREAK  = 1 << 1,  /**< a block scalar ending the input gets no
                                             final line feed (libyaml, so PyYAML and
                                             go-yaml v2) */
    OYL_SYNTAX_YAML11_ONLY   = 1 << 2,  /**< %YAML other than 1.1 is an error, in the
                                             default dialect for every document but one
                                             choosing a registered dialect (go-yaml v2) */
    OYL_SYNTAX_FLOW_INDENT   = 1 << 3,  /**< a quoted scalar's or flow collection's
                                             later lines needn't be indented past the
                                             block it sits in, and a tab may indent
                                             them: [a,\nb] (libyaml, so PyYAML and
                                             go-yaml v2) */
    /** go-yaml v2's */
    OYL_SYNTAX_GOYAML2 = OYL_SYNTAX_SQUOTE_ESCAPE | OYL_SYNTAX_EOF_NO_BREAK | OYL_SYNTAX_YAML11_ONLY
                         | OYL_SYNTAX_FLOW_INDENT,
};

/** How << merge keys combine with the keys written in a mapping. */
typedef enum {
    OYL_MERGE_NONE,     /**< << is an ordinary key */
    OYL_MERGE_YAML,     /**< keys written in the mapping win, then the merges: a later <<
                             over an earlier one, a sequence's earlier items over its
                             later ones, as in PyYAML (YAML 1.1's merge type) */
    OYL_MERGE_ORDERED,  /**< go-yaml v2: a merge overrides the keys before it */
} oyl_merge_order;

/** A dialect of YAML @p version ("1.1", "1.2"), allocated in the arena.
 *  NULL for a version not 1.x (or whose minor an int can't hold), or out
 *  of memory. */
OYL_API const oyl_dialect *oyl_dialect_new(oyl_arena *a, const char *version,
                                           const oyl_schema *schema, unsigned syntax,
                                           oyl_merge_order merge);
OYL_API const oyl_schema *oyl_dialect_schema(const oyl_dialect *d);
OYL_API unsigned          oyl_dialect_syntax(const oyl_dialect *d);
OYL_API oyl_merge_order   oyl_dialect_merge(const oyl_dialect *d);
/** The YAML version it is a dialect of. */
OYL_API void              oyl_dialect_version(const oyl_dialect *d, int *major, int *minor);

/** YAML 1.2: the Core schema, YAML 1.2's syntax, YAML's merge. */
OYL_API const oyl_dialect *oyl_dialect_yaml12(void);
/** YAML 1.1 with PyYAML's types (oyl_schema_yaml11) and YAML's merge: a
 *  document PyYAML reads gets the same values. The exceptions: ints
 *  beyond 64 bits are text, as is a base 60 !!int with a part beyond 64
 *  bits even where later parts cancel it out; keys are equal as YAML has
 *  them, by tag and value, where Python's == also makes 1, 1.0 and true
 *  one key, and two timestamps of one instant written otherwise one key;
 *  and a base 60 float of more than 14 parts may differ in its last bits.
 *  The syntax is YAML 1.2's, with two of libyaml's leniencies: a block
 *  scalar ending the input gets no final line feed, and a quoted scalar's
 *  or flow collection's later lines needn't be indented past the block it
 *  sits in (OYL_SYNTAX_EOF_NO_BREAK, OYL_SYNTAX_FLOW_INDENT). Other
 *  malformed YAML that libyaml lets through stays an error, such as a
 *  comment touching a closing quote or bracket or a block scalar's header
 *  ("a"#c, [a]#c, |#c). */
OYL_API const oyl_dialect *oyl_dialect_yaml11(void);
/** go-yaml v2, as Kubernetes reads YAML: its types, its syntax
 *  leniencies (OYL_SYNTAX_GOYAML2), and its merge order. A dialect of
 *  YAML 1.1, as go-yaml v2 reads it. Its other libyaml quirks aren't
 *  copied; as YAML 1.2 has it, NEL, U+2028 and U+2029 aren't line breaks,
 *  the escape \/ and a raw DEL may be quoted, an anchor may hold a colon
 *  (&a:b), and a zero-indented top-level block scalar keeps its text. */
OYL_API const oyl_dialect *oyl_dialect_goyaml2(void);

/** The default dialect, for documents whose directives choose none, and
 *  for those declaring its version (%YAML). Without one, it is YAML 1.2,
 *  with the schema of oyl_parser_set_schema if set. */
OYL_API void oyl_parser_set_dialect(oyl_parser *p, const oyl_dialect *d);

/** Let documents choose @p d: by %YAML @p version ("1.1"), or by a
 *  %TAG !! @p tag_prefix; give one, the other NULL. A version's dialect
 *  comes before the default and the built-in one for it. OYL_ERR_INPUT for
 *  both or neither, a version not 1.x (as oyl_dialect_new) or a prefix too
 *  long, OYL_ERR_LIMIT past 8 of each. */
OYL_API oyl_status oyl_parser_add_dialect(oyl_parser *p, const oyl_dialect *d,
                                          const char *version, const char *tag_prefix);

/** The dialect of the document an OYL_EVT_DOC_START begins: ask when that
 *  event arrives (the parser may read ahead, so by the document's DOC_END
 *  it may describe the next). */
OYL_API const oyl_dialect *oyl_parser_document_dialect(oyl_parser *p);

/** Likewise the document's %YAML version; false if it has none. */
OYL_API bool oyl_parser_yaml_version(oyl_parser *p, int *major, int *minor);

/** Likewise the document's %TAG directive @p i (from 0): its handle
 *  ("!e!") and its prefix, %-escapes decoded as in tags; false past the
 *  last. The strings are the parser's: use them before reading on. To
 *  re-emit a document, pass these and its %YAML version to
 *  oyl_emit_tag_directive() and oyl_emit_yaml_directive() before its
 *  DOC_START. */
OYL_API bool oyl_parser_tag_directive(oyl_parser *p, int i, oyl_str *handle, oyl_str *prefix);

/** What a warning is about. Codes may be added at the end. */
typedef enum {
    OYL_WARN_YAML_VERSION = 1,  /**< a %YAML 1.x version other than 1.1 or 1.2 */
    OYL_WARN_DIALECT,           /**< a %TAG !! prefix with no dialect registered */
    OYL_WARN_LENIENCY,          /**< the dialect accepted what YAML 1.2 doesn't */
    OYL_WARN_MERGE_ORDER,       /**< in go-yaml v2's merge order, a merge overrode a key
                                     written before it, which YAML's merge would keep */
} oyl_warning;

/** A warning: @p message says what, @p mark where. Parsing goes on. */
typedef void (*oyl_warn_fn)(void *user, oyl_warning code, oyl_mark mark, const char *message);

/** Receive warnings; NULL for none (the default). */
OYL_API void oyl_parser_set_warn(oyl_parser *p, oyl_warn_fn fn, void *user);

/* ── JSON ────────────────────────────────────────────────── */

/** Options for oyl_parse_json. */
enum {
    OYL_JSON_MERGE       = 1 << 0,  /**< expand << merge keys; keys written in the mapping win */
    OYL_JSON_MERGE_ORDER = 1 << 1,  /**< with MERGE, as go-yaml v2: a merge overrides the
                                         keys before it, and keys after it override it */
    OYL_JSON_STRICT      = 1 << 2,  /**< a key set twice in any mapping is an error, as
                                         are two keys that become one string (1, "1") */
    /** As Kubernetes reads YAML (sigs.k8s.io/yaml's YAMLToJSON), with
     *  oyl_schema_goyaml2() */
    OYL_JSON_KUBERNETES  = OYL_JSON_MERGE | OYL_JSON_MERGE_ORDER,
};

/** Read the parser's next document and write it as JSON, typing scalars
 *  with @p schema; with @p schema NULL, with the document's dialect, whose
 *  merge order then replaces the MERGE flags. The output lives in the
 *  parser's arena. At the end of
 *  the stream, returns OYL_OK with an empty @p json.
 *
 *  Keys are sorted by their bytes, and a key set twice keeps its last
 *  value (an error under OYL_JSON_STRICT; -0.0 and 0.0 are one key, as
 *  in Go, written as the last one is); aliases are expanded, within
 *  go-yaml v2's limit on aliasing, counted as go-yaml v2 counts it (in
 *  YAML's merge, as written); in YAML's merge, each mapping merged, and
 *  each sequence of them merged through an alias, is worked out once,
 *  within the parser's bound on merges for events
 *  with max_events off; numbers and strings are written as Go's
 *  encoding/json writes them. With oyl_schema_goyaml2() and
 *  OYL_JSON_KUBERNETES, the output is byte for byte that of
 *  sigs.k8s.io/yaml's YAMLToJSON for the first document.
 *
 *  Leave the parser's own merge and alias resolution off: this function
 *  does both. Errors are reported like parse errors, with
 *  oyl_parser_error() and oyl_parser_error_mark(): as OYL_ERR_PARSE, or
 *  OYL_ERR_LIMIT for too much aliasing, merging or nesting (merges
 *  chained count as nesting). Every node is checked as decoding it
 *  checks it, as go-yaml v2 and PyYAML decode even the value of a key set
 *  again later: a scalar its tag rejects, a collection as a key, a merge
 *  of what isn't a mapping, an unknown anchor or one inside its own
 *  value. What only JSON can't hold (a null or uint64 key, inf or NaN)
 *  fails only where it would be written. */
OYL_API oyl_status oyl_parse_json(oyl_parser *p, const oyl_schema *schema,
                                  unsigned flags, oyl_str *json);

/* ── Trees ───────────────────────────────────────────────── */

/* A document in memory. Its nodes are in document order, the order of the
 * parser's events, and a collection records how many nodes its subtree
 * holds instead of links to its children:
 *   - a collection's first child is the node after it
 *   - the next sibling of node i is node i + span
 *   - a mapping's children alternate: key, value, key, value
 * A tree holds offsets, not pointers: text is in the input (from the
 * document's start) or in the tree's pool, so a binding can copy the
 * arrays whole or read them in place. A finished tree is read-only, and
 * threads may share it. Aliases and << merge keys stay as written:
 * lookups follow aliases and apply merges in the order of the document's
 * dialect. The merges are worked out as the tree is built, each mapping
 * merged once however the merges nest, so reading a mapping costs its
 * pairs. That work is bounded as the parser's merges for events are;
 * past the bound nothing is kept: oyl_node_entries gives OYL_ERR_LIMIT
 * for a mapping that merges, and oyl_node_get walks the merges, visiting
 * each mapping once, at most the size of the tree. */

typedef struct oyl_tree oyl_tree;

/** No node: what a lookup returns when it finds nothing. */
#define OYL_NO_NODE UINT32_MAX

/** What a node is. */
typedef enum {
    OYL_NODE_SCALAR = 1,
    OYL_NODE_SEQUENCE,
    OYL_NODE_MAPPING,
    OYL_NODE_ALIAS,
} oyl_node_kind;

/** oyl_node flags */
enum {
    OYL_NODE_POOLED = 1 << 0,  /**< its text is in the pool, not the input */
    OYL_NODE_KEY    = 1 << 1,  /**< it is a mapping key */
    OYL_NODE_FLOW   = 1 << 2,  /**< a flow collection: [ ] or { } */
};

/** A node: 32 bytes, laid out for good, so that bindings can read arrays
 *  of them directly. Positions are offsets in the input from the
 *  document's start (oyl_tree_base), except text in the pool. */
typedef struct {
    uint32_t text;     /**< scalar: where its text starts; alias: its anchor's name */
    uint32_t len;      /**< scalar, alias: the text's length; sequence: its items;
                            mapping: its pairs */
    uint32_t span;     /**< the nodes in its subtree, itself included */
    uint32_t parent;   /**< how far back its parent is (0: the root) */
    uint32_t props;    /**< its tag and anchor: an index into oyl_tree_props (0: none);
                            an alias: the node it refers to */
    uint32_t start;    /**< where it starts */
    uint32_t end;      /**< where it ends */
    uint8_t  kind;     /**< oyl_node_kind */
    uint8_t  style;    /**< a scalar's oyl_scalar_style */
    uint8_t  type;     /**< an untagged plain scalar's type, as the document's schema
                            resolved it (0: the default); read it with oyl_node_tag()
                            and oyl_node_value() */
    uint8_t  flags;    /**< OYL_NODE_ flags */
} oyl_node;

/** A node's tag and anchor, both in the pool (length 0: none). The tag is
 *  as written, its handle expanded. */
typedef struct {
    uint32_t tag, tag_len;
    uint32_t anchor, anchor_len;
} oyl_node_props;

/** A mapping's key and its value. */
typedef struct {
    uint32_t key, value;
} oyl_pair;

/** Read the parser's next document into a tree. At the end of the stream,
 *  returns OYL_OK with @p *tree NULL. Free the tree with oyl_tree_free();
 *  the input, and a schema or dialect given to the parser, must outlive
 *  it, but the parser need not. The parser's settings apply (dialects,
 *  limits, warnings); leave its merge and alias resolution off. Errors
 *  (the parser's, an alias to an unknown anchor, a document over 4 GB,
 *  memory running out working out merges) are reported by
 *  oyl_parser_error() and oyl_parser_error_mark(). */
OYL_API oyl_status oyl_parse_tree(oyl_parser *p, oyl_tree **tree);

/** Free a tree. */
OYL_API void oyl_tree_free(oyl_tree *t);

/** The nodes in document order; the root is node 0. */
OYL_API const oyl_node *oyl_tree_nodes(const oyl_tree *t, uint32_t *count);

/** The tags and anchors that nodes' props refer to (entry 0 is unused). */
OYL_API const oyl_node_props *oyl_tree_props(const oyl_tree *t, uint32_t *count);

/** The pool: text the parser rewrote (escapes, folded lines), tags and
 *  anchor names. */
OYL_API oyl_str oyl_tree_pool(const oyl_tree *t);

/** Where the document starts in the input: node positions count from here. */
OYL_API size_t oyl_tree_base(const oyl_tree *t);

/** The document's dialect, which typed its scalars. */
OYL_API const oyl_dialect *oyl_tree_dialect(const oyl_tree *t);

/** The document's %YAML version; false if it has none. */
OYL_API bool oyl_tree_yaml_version(const oyl_tree *t, int *major, int *minor);

/** The document's %TAG directive @p i (from 0), as
 *  oyl_parser_tag_directive() gives it; false past the last. */
OYL_API bool oyl_tree_tag_directive(const oyl_tree *t, int i, oyl_str *handle, oyl_str *prefix);

/** A scalar's text, or an alias's anchor name (empty for a collection). */
OYL_API oyl_str oyl_node_text(const oyl_tree *t, uint32_t i);

/** A node's tag: the one written, else the one its dialect resolves
 *  (empty for an alias). */
OYL_API oyl_str oyl_node_tag(const oyl_tree *t, uint32_t i);

/** A node's anchor name, or empty. */
OYL_API oyl_str oyl_node_anchor(const oyl_tree *t, uint32_t i);

/** A scalar's value in the document's schema, through aliases. As
 *  oyl_schema_value(): OYL_ERR_PARSE if its explicit tag rejects it (or
 *  it is a YAML 1.1 timestamp that doesn't exist), OYL_ERR_INPUT if it
 *  isn't a scalar. */
OYL_API oyl_status oyl_node_value(const oyl_tree *t, uint32_t i, oyl_value *v);

/** Where a node starts: offset, line and column, as the parser's events
 *  give them. */
OYL_API oyl_mark oyl_node_mark(const oyl_tree *t, uint32_t i);

/** The node an alias refers to, through any chain of aliases; any other
 *  node is itself. An alias may refer to a collection that contains it. */
OYL_API uint32_t oyl_node_target(const oyl_tree *t, uint32_t i);

/** The value of @p key in mapping @p map (or an alias to one), or
 *  OYL_NO_NODE. @p key is the string key; when the mapping has none, the
 *  key a scalar written as @p key is, so "80" finds 80. Keys are equal
 *  when their tags and values are, as YAML compares them: 0x50 and 80
 *  are one key, 80 and "80" two. A key written twice gives its last
 *  value; << merges apply in the order of the document's dialect. A <<
 *  whose value can't be merged (not a mapping, an alias to one, or a
 *  sequence of those, which YAML's merge also takes through an alias) is
 *  an ordinary key. OYL_NO_NODE also if memory runs out walking merges
 *  too costly to work out. */
OYL_API uint32_t oyl_node_get(const oyl_tree *t, uint32_t map, oyl_str key);

/** As oyl_node_get(), for the key equal to @p key: a scalar of its kind's
 *  tag (!!null, !!bool, !!int, !!float, or for text !!str) with its
 *  value, so an OYL_VALUE_INT 80 finds 80 and 0x50 but not "80". */
OYL_API uint32_t oyl_node_get_value(const oyl_tree *t, uint32_t map, const oyl_value *key);

/** Item @p k of sequence @p seq (or an alias to one), or OYL_NO_NODE. */
OYL_API uint32_t oyl_node_at(const oyl_tree *t, uint32_t seq, uint32_t k);

/** Options for oyl_node_entries. */
enum {
    OYL_ENTRIES_STRICT = 1 << 0,   /**< a key written twice in a mapping is an error */
};

/** A mapping's pairs as a program sees it, by the rules of oyl_node_get():
 *  merges applied and each key once, in the order keys first appear. The
 *  array is allocated in @p a. Scalar keys are equal when their tags and
 *  values are (YAML 1.2.2 §3.2.1.3), so 1 and "1" are two keys and 0x1
 *  and 1 one; collection keys are all kept. Returns OYL_ERR_PARSE under
 *  OYL_ENTRIES_STRICT for a repeated key, the error YAML makes it, with
 *  @p *n the first key written again; OYL_ERR_LIMIT if @p map merges and
 *  the document's merges were too costly to work out; OYL_ERR_MEMORY;
 *  OYL_ERR_INPUT if @p map isn't a mapping. */
OYL_API oyl_status oyl_node_entries(const oyl_tree *t, uint32_t map, unsigned flags,
                                    oyl_arena *a, const oyl_pair **pairs, size_t *n);

/* ── Upgrade: a YAML 1.1 file made YAML 1.2 in place ─────── */

/** What oyl_upgrade() fixed, or found and left. */
typedef enum {
    OYL_FIX_SCALAR = 1,  /**< a scalar YAML 1.2 reads otherwise */
    OYL_FIX_DIRECTIVE,   /**< %YAML 1.1, removed */
    OYL_FIX_ESCAPE,      /**< \' in double quotes (go-yaml v2's), the backslash removed */
    OYL_FIX_CHOMP,       /**< a block scalar ending the input without a line break */
    OYL_FIX_MERGE,       /**< a key go-yaml v2's merge overrides, commented out */
    OYL_FIX_INDENT,      /**< a quoted scalar's or flow collection's line, indented */
} oyl_fix_kind;

/** One change, or one difference left as it was. */
typedef struct {
    oyl_fix_kind kind;
    oyl_mark     mark;     /**< where, in the input */
    oyl_str      before;   /**< the input's text there */
    oyl_str      after;    /**< what replaced it (empty if removed, or left) */
    bool         fixed;    /**< false: reported only, the input left as it was */
    const char  *message;  /**< what each reader reads, and what was done */
} oyl_fix;

/** oyl_upgrade() options. Values keep the source's value and keys their
 *  text (as YAML 1.2 reads it) unless these say otherwise. */
enum {
    OYL_UPGRADE_KEYS_KEEP_VALUE  = 1 << 0,  /**< keys keep the source's value too */
    OYL_UPGRADE_VALUES_KEEP_TEXT = 1 << 1,  /**< values keep YAML 1.2's reading too */
    OYL_UPGRADE_TIMESTAMPS_TEXT  = 1 << 2,  /**< a timestamp's value is its text, as
                                                 go-yaml v2 hands it to programs (implied
                                                 by oyl_dialect_goyaml2()) */
};

/** Make the rest of @p p's stream, a YAML 1.1 file, read the same in YAML
 *  1.2, rewriting only what a 1.2 reader would read otherwise and keeping
 *  every other byte (comments, layout, quoting, key order). The parser's
 *  default dialect names the file's reader (oyl_dialect_yaml11() for
 *  PyYAML, oyl_dialect_goyaml2(), or a program's): afterwards the file
 *  reads the same in it and in YAML 1.2. Left at YAML 1.2, the default,
 *  the reader is unknown: what PyYAML and go-yaml v2 read alike is fixed,
 *  what they read otherwise is reported, unless the file rules one out
 *  (\' is an error to PyYAML, %YAML 1.2 to go-yaml v2). A document
 *  choosing YAML 1.2 by its directives is left alone.
 *
 *  A scalar is rewritten in a spelling every reader reads alike: true,
 *  !!int 0o755 for 0755, "on", 1.0e+3, !!timestamp 2001-12-14. %YAML 1.1
 *  is removed, \' in double quotes loses its backslash, a block scalar
 *  ending the input without a line break gets the strip indicator, a
 *  quoted scalar's or flow collection's line not indented past its block
 *  is indented, and a key go-yaml v2's merge overrides is commented out.
 *  Each is in @p fixes, with what can't be fixed (as two keys made equal),
 *  left as it was. The output is then read in every reader and YAML 1.2:
 *  if any scalar reads otherwise than decided, OYL_ERR_PARSE, never a
 *  file that reads otherwise.
 *
 *  @p out and @p fixes are in the parser's arena. Errors are reported as
 *  parse errors (oyl_parser_error(), oyl_parser_error_mark()); the
 *  parser's warning callback still hears its warnings. */
OYL_API oyl_status oyl_upgrade(oyl_parser *p, unsigned flags, oyl_str *out,
                               const oyl_fix **fixes, size_t *nfixes);

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
 * "tag:yaml.org,2002:str", written as !!str) or local tags ("!foo"),
 * written with the document's %TAG handles where they fit.
 * Events in another order (two roots, a mapping ended after a key, an end
 * that doesn't match its start), and anchor or alias names YAML can't
 * hold (empty, or with blanks, flow indicators or control characters),
 * are OYL_ERR_EMIT. An error is final: every later call returns it, and
 * the output so far may be incomplete. A mapping key longer than YAML
 * allows an implicit key (1024 characters, its anchor and tag included)
 * is written explicit ("? key"). */

/** Re-emit an event obtained from oyl_parse_next() (e.g. to reformat a
 *  document). A document's directives aren't among its events: see
 *  oyl_parser_tag_directive(). To build output yourself, use the
 *  functions below. */
OYL_API oyl_status   oyl_emit(oyl_emitter *e, const oyl_event *evt);

OYL_API oyl_status   oyl_emit_stream_start(oyl_emitter *e);
OYL_API oyl_status   oyl_emit_stream_end(oyl_emitter *e);

/** Write a %YAML directive for the next document: @p major must be 1. A
 *  document without one is YAML 1.2 to a YAML 1.2 reader; one with it
 *  chooses that version's dialect in Oyl (see Dialects), but go-yaml v2
 *  and v3 reject a %YAML other than 1.1, and libyaml reads YAML 1.1
 *  whatever it says. Call it, and oyl_emit_tag_directive(), between
 *  documents, before oyl_emit_document_start(): a document with
 *  directives starts with "---", and the one before it ends with "...".
 *  One %YAML per document; a directive and no document after it, or
 *  given inside one, is OYL_ERR_EMIT. */
OYL_API oyl_status   oyl_emit_yaml_directive(oyl_emitter *e, int major, int minor);

/** Write a %TAG directive for the next document: @p handle is "!", "!!"
 *  or "!name!" (letters, digits, '-'), @p prefix the tags it stands for,
 *  written with %-escapes where YAML needs them. The document's tags are
 *  written with its handles where they fit, and with "!" and "!!" only as
 *  its directives define them (otherwise verbatim, "!<...>"). One per
 *  handle, at most 8 per document, as long as the parser reads (handle
 *  under 16 bytes, prefix as written under 256); otherwise OYL_ERR_EMIT. */
OYL_API oyl_status   oyl_emit_tag_directive(oyl_emitter *e, oyl_str handle, oyl_str prefix);

/** Start a document; @p implicit omits the "---" marker where YAML
 *  allows: not after a document that ended without "...", not for an
 *  empty one, and not with directives. */
OYL_API oyl_status   oyl_emit_document_start(oyl_emitter *e, bool implicit);

/** End a document; @p implicit omits the "..." marker. */
OYL_API oyl_status   oyl_emit_document_end(oyl_emitter *e, bool implicit);

/** Emit a scalar. OYL_SCALAR_PLAIN writes the value plain when that reads
 *  back as the same text, and quotes it otherwise; the other styles are
 *  honored where valid (block styles fall back to double-quoted in flow
 *  context). Characters YAML doesn't allow unescaped (control characters
 *  but tab and line feed, DEL, the C1 controls, U+FEFF, U+FFFE, U+FFFF),
 *  and NEL, U+2028 and U+2029, which YAML 1.1 reads as line breaks, are
 *  written escaped, in double quotes. The emitter doesn't decode UTF-8:
 *  invalid UTF-8 is written as it is. */
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

/** Emit a tree as one document, with its %YAML and %TAG directives, and
 *  "---" and "..." where they were written; call it between
 *  oyl_emit_stream_start() and oyl_emit_stream_end(). Directives given
 *  for it already (oyl_emit_yaml_directive(), oyl_emit_tag_directive())
 *  take the place of its own. The output is what the document's events
 *  give, but for a parser with a schema (oyl_parser_set_schema()): its
 *  events tag every node, and the tree keeps only the tags written. */
OYL_API oyl_status   oyl_emit_tree(oyl_emitter *e, const oyl_tree *t);

/** The emitter's output so far, copied into the emitter's arena and valid
 *  until that is freed. Its data is NULL only when the copy can't be
 *  allocated (or for a NULL emitter). */
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
