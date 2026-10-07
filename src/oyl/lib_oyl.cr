@[Link(ldflags: "#{__DIR__}/../../ext/oyl/build/liboyl.a")]
lib LibOyl
  # ── String view ──────────────────────────────────────────

  struct Str
    data : LibC::Char*
    len : LibC::SizeT
  end

  # ── Source location ──────────────────────────────────────

  struct Mark
    offset : LibC::SizeT
    line : LibC::SizeT
    col : LibC::SizeT
  end

  # ── Status codes ─────────────────────────────────────────

  enum Status
    OK        = 0
    ErrMemory
    ErrInput
    ErrScan
    ErrParse
    ErrEmit
    ErrLimit
  end

  # ── Token types ──────────────────────────────────────────

  enum TokenType
    None          = 0
    StreamStart
    StreamEnd
    DocStart
    DocEnd
    BlockSeqEntry
    BlockMapKey
    BlockMapValue
    FlowSeqStart
    FlowSeqEnd
    FlowMapStart
    FlowMapEnd
    FlowEntry
    Scalar
    Tag
    Anchor
    Alias
    Directive
  end

  # ── Scalar style ─────────────────────────────────────────

  enum ScalarStyle
    Plain        = 0
    SingleQuoted
    DoubleQuoted
    Literal
    Folded
  end

  # ── Event types ──────────────────────────────────────────

  enum EventType
    None          = 0
    StreamStart
    StreamEnd
    DocStart
    DocEnd
    MappingStart
    MappingEnd
    SequenceStart
    SequenceEnd
    Scalar
    Alias
  end

  # ── Emit style ───────────────────────────────────────────

  enum EmitStyle
    Block   = 0
    Flow
    Minimal
  end

  # ── Match type ───────────────────────────────────────────

  enum MatchType
    Exact   = 0
    Icase
    Builtin
  end

  # ── Token and event ──────────────────────────────────────
  # Owned by the library and handed out by const pointer, so fields can
  # be appended in later versions: read them through the pointer (or copy
  # the struct), never allocate one.

  struct Token
    type : TokenType
    value : Str
    scalar_style : ScalarStyle
    start : Mark
    end_ : Mark
  end

  struct Event
    type : EventType
    value : Str
    anchor : Str
    tag : Str
    scalar_style : ScalarStyle
    implicit : Bool
    flow : Bool
    start : Mark
    end_ : Mark
  end

  # ── Opaque types ─────────────────────────────────────────

  type Arena = Void
  type Parser = Void
  type Scanner = Void
  type Emitter = Void
  type Schema = Void
  type SchemaBuilder = Void

  # ── Tag constants ────────────────────────────────────────

  $oyl_tag_null = OYL_TAG_NULL : Str
  $oyl_tag_bool = OYL_TAG_BOOL : Str
  $oyl_tag_int = OYL_TAG_INT : Str
  $oyl_tag_float = OYL_TAG_FLOAT : Str
  $oyl_tag_str = OYL_TAG_STR : Str
  $oyl_tag_seq = OYL_TAG_SEQ : Str
  $oyl_tag_map = OYL_TAG_MAP : Str
  $oyl_tag_merge = OYL_TAG_MERGE : Str

  # ── Arena ────────────────────────────────────────────────

  fun arena_new = oyl_arena_new(initial_cap : LibC::SizeT) : Arena*
  fun arena_alloc = oyl_arena_alloc(a : Arena*, size : LibC::SizeT, align : LibC::SizeT) : Void*
  fun arena_dup = oyl_arena_dup(a : Arena*, src : LibC::Char*, len : LibC::SizeT) : LibC::Char*
  fun arena_reset = oyl_arena_reset(a : Arena*) : Void
  fun arena_free = oyl_arena_free(a : Arena*) : Void

  # ── File input ───────────────────────────────────────────

  fun read_file = oyl_read_file(path : LibC::Char*, a : Arena*) : Str

  # ── Scanner ──────────────────────────────────────────────

  fun scanner_new = oyl_scanner_new(input : LibC::Char*, len : LibC::SizeT, a : Arena*) : Scanner*
  fun scan_next = oyl_scan_next(s : Scanner*, tok : Token**) : Status
  fun scanner_error = oyl_scanner_error(s : Scanner*) : LibC::Char*
  fun scanner_error_mark = oyl_scanner_error_mark(s : Scanner*) : Mark
  fun scanner_free = oyl_scanner_free(s : Scanner*) : Void

  # ── Schema ───────────────────────────────────────────────

  fun schema_failsafe = oyl_schema_failsafe : Schema*
  fun schema_json = oyl_schema_json : Schema*
  fun schema_core = oyl_schema_core : Schema*
  fun schema_resolve = oyl_schema_resolve(schema : Schema*, value : Str, style : ScalarStyle) : Str

  fun schema_builder_new = oyl_schema_builder_new(a : Arena*) : SchemaBuilder*
  fun schema_builder_add = oyl_schema_builder_add(b : SchemaBuilder*, match : MatchType, pattern : LibC::Char*, tag : Str) : Void
  fun schema_builder_add_bools = oyl_schema_builder_add_bools(b : SchemaBuilder*, true_terms : LibC::Char**, ntrue : LibC::Int, false_terms : LibC::Char**, nfalse : LibC::Int) : Void
  fun schema_builder_add_nulls = oyl_schema_builder_add_nulls(b : SchemaBuilder*, terms : LibC::Char**, nterms : LibC::Int) : Void
  fun schema_builder_add_int = oyl_schema_builder_add_int(b : SchemaBuilder*) : Void
  fun schema_builder_add_float = oyl_schema_builder_add_float(b : SchemaBuilder*) : Void
  fun schema_builder_finish = oyl_schema_builder_finish(b : SchemaBuilder*) : Schema*
  fun schema_builder_free = oyl_schema_builder_free(b : SchemaBuilder*) : Void

  # ── Parser ───────────────────────────────────────────────

  fun parser_new = oyl_parser_new(input : LibC::Char*, len : LibC::SizeT, a : Arena*) : Parser*
  fun parse_next = oyl_parse_next(p : Parser*, evt : Event**) : Status
  fun parser_set_schema = oyl_parser_set_schema(p : Parser*, schema : Schema*) : Void
  fun parser_set_merge = oyl_parser_set_merge(p : Parser*, enable : Bool) : Void
  fun parser_set_resolve = oyl_parser_set_resolve(p : Parser*, enable : Bool) : Void
  fun parser_set_max_events = oyl_parser_set_max_events(p : Parser*, max : LibC::Int) : Void
  fun parser_set_max_depth = oyl_parser_set_max_depth(p : Parser*, max : LibC::Int) : Void
  fun parser_error = oyl_parser_error(p : Parser*) : LibC::Char*
  fun parser_error_mark = oyl_parser_error_mark(p : Parser*) : Mark
  fun parser_free = oyl_parser_free(p : Parser*) : Void

  # ── Emitter ──────────────────────────────────────────────

  fun emitter_new = oyl_emitter_new(a : Arena*) : Emitter*
  fun emitter_set_style = oyl_emitter_set_style(e : Emitter*, style : EmitStyle) : Void
  fun emitter_set_indent = oyl_emitter_set_indent(e : Emitter*, indent : LibC::Int) : Void
  fun emit = oyl_emit(e : Emitter*, evt : Event*) : Status
  fun emit_stream_start = oyl_emit_stream_start(e : Emitter*) : Status
  fun emit_stream_end = oyl_emit_stream_end(e : Emitter*) : Status
  fun emit_document_start = oyl_emit_document_start(e : Emitter*, implicit : Bool) : Status
  fun emit_document_end = oyl_emit_document_end(e : Emitter*, implicit : Bool) : Status
  fun emit_scalar = oyl_emit_scalar(e : Emitter*, value : Str, style : ScalarStyle, anchor : Str, tag : Str) : Status
  fun emit_alias = oyl_emit_alias(e : Emitter*, name : Str) : Status
  fun emit_mapping_start = oyl_emit_mapping_start(e : Emitter*, anchor : Str, tag : Str, flow : Bool) : Status
  fun emit_mapping_end = oyl_emit_mapping_end(e : Emitter*) : Status
  fun emit_sequence_start = oyl_emit_sequence_start(e : Emitter*, anchor : Str, tag : Str, flow : Bool) : Status
  fun emit_sequence_end = oyl_emit_sequence_end(e : Emitter*) : Status
  fun emitter_output = oyl_emitter_output(e : Emitter*) : Str
  fun emitter_free = oyl_emitter_free(e : Emitter*) : Void

  # ── Convenience ──────────────────────────────────────────

  fun status_str = oyl_status_str(s : Status) : LibC::Char*
  fun token_type_str = oyl_token_type_str(t : TokenType) : LibC::Char*
  fun event_type_str = oyl_event_type_str(t : EventType) : LibC::Char*
end
