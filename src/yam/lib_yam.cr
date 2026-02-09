@[Link(ldflags: "#{__DIR__}/../../ext/yam/build/libyam.a")]
lib LibYam
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
  end

  # ── Token types ──────────────────────────────────────────

  enum TokenType
    None = 0
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
    None = 0
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

  # ── Token ────────────────────────────────────────────────

  struct Token
    type : TokenType
    value : Str
    scalar_style : ScalarStyle
    start : Mark
    end_ : Mark
  end

  # ── Event ────────────────────────────────────────────────

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

  # ── Emit options ─────────────────────────────────────────

  struct EmitOpts
    style : EmitStyle
    indent : LibC::Int
  end

  # ── Schema ───────────────────────────────────────────────

  struct SchemaRule
    match : MatchType
    pattern : LibC::Char*
    tag : Str
  end

  struct Schema
    rules : SchemaRule*
    rule_count : LibC::Int
    default_plain_tag : Str
    default_quoted_tag : Str
    default_seq_tag : Str
    default_map_tag : Str
  end

  # ── Opaque types ─────────────────────────────────────────

  type Arena = Void
  type Parser = Void
  type Scanner = Void
  type Emitter = Void
  type SchemaBuilder = Void

  # ── Tag constants ────────────────────────────────────────

  $yam_tag_null = YAM_TAG_NULL : Str
  $yam_tag_bool = YAM_TAG_BOOL : Str
  $yam_tag_int = YAM_TAG_INT : Str
  $yam_tag_float = YAM_TAG_FLOAT : Str
  $yam_tag_str = YAM_TAG_STR : Str
  $yam_tag_seq = YAM_TAG_SEQ : Str
  $yam_tag_map = YAM_TAG_MAP : Str
  $yam_tag_merge = YAM_TAG_MERGE : Str

  # ── Arena ────────────────────────────────────────────────

  fun arena_new = yam_arena_new(initial_cap : LibC::SizeT) : Arena*
  fun arena_alloc = yam_arena_alloc(a : Arena*, size : LibC::SizeT, align : LibC::SizeT) : Void*
  fun arena_dup = yam_arena_dup(a : Arena*, src : LibC::Char*, len : LibC::SizeT) : LibC::Char*
  fun arena_reset = yam_arena_reset(a : Arena*) : Void
  fun arena_free = yam_arena_free(a : Arena*) : Void

  # ── File input ───────────────────────────────────────────

  fun read_file = yam_read_file(path : LibC::Char*, a : Arena*) : Str

  # ── Scanner ──────────────────────────────────────────────

  fun scanner_new = yam_scanner_new(input : LibC::Char*, len : LibC::SizeT, a : Arena*) : Scanner*
  fun scan_next = yam_scan_next(s : Scanner*, tok : Token*) : Status
  fun scanner_error = yam_scanner_error(s : Scanner*) : LibC::Char*
  fun scanner_error_mark = yam_scanner_error_mark(s : Scanner*) : Mark
  fun scanner_free = yam_scanner_free(s : Scanner*) : Void

  # ── Parser ───────────────────────────────────────────────

  fun parser_new = yam_parser_new(input : LibC::Char*, len : LibC::SizeT, a : Arena*) : Parser*
  fun parse_next = yam_parse_next(p : Parser*, evt : Event*) : Status
  fun parser_set_schema = yam_parser_set_schema(p : Parser*, schema : Schema*) : Void
  fun parser_set_merge = yam_parser_set_merge(p : Parser*, enable : Bool) : Void
  fun parser_set_resolve = yam_parser_set_resolve(p : Parser*, enable : Bool) : Void
  fun parser_set_max_events = yam_parser_set_max_events(p : Parser*, max : LibC::Int) : Void
  fun parser_error = yam_parser_error(p : Parser*) : LibC::Char*
  fun parser_error_mark = yam_parser_error_mark(p : Parser*) : Mark
  fun parser_free = yam_parser_free(p : Parser*) : Void

  # ── Emitter ──────────────────────────────────────────────

  fun emitter_new = yam_emitter_new(opts : EmitOpts, a : Arena*) : Emitter*
  fun emit = yam_emit(e : Emitter*, evt : Event*) : Status
  fun emitter_output = yam_emitter_output(e : Emitter*) : Str
  fun emitter_free = yam_emitter_free(e : Emitter*) : Void

  # ── Schema functions ─────────────────────────────────────

  fun schema_failsafe = yam_schema_failsafe : Schema
  fun schema_json = yam_schema_json : Schema
  fun schema_core = yam_schema_core : Schema
  fun schema_resolve = yam_schema_resolve(schema : Schema*, evt : Event*) : Str

  # ── Schema builder ───────────────────────────────────────

  fun schema_builder_new = yam_schema_builder_new(a : Arena*) : SchemaBuilder*
  fun schema_builder_add = yam_schema_builder_add(b : SchemaBuilder*, match : MatchType, pattern : LibC::Char*, tag : Str) : Void
  fun schema_builder_add_bools = yam_schema_builder_add_bools(b : SchemaBuilder*, true_terms : LibC::Char**, ntrue : LibC::Int, false_terms : LibC::Char**, nfalse : LibC::Int) : Void
  fun schema_builder_add_nulls = yam_schema_builder_add_nulls(b : SchemaBuilder*, terms : LibC::Char**, nterms : LibC::Int) : Void
  fun schema_builder_add_int = yam_schema_builder_add_int(b : SchemaBuilder*) : Void
  fun schema_builder_add_float = yam_schema_builder_add_float(b : SchemaBuilder*) : Void
  fun schema_builder_finish = yam_schema_builder_finish(b : SchemaBuilder*) : Schema
  fun schema_builder_free = yam_schema_builder_free(b : SchemaBuilder*) : Void

  # ── Convenience ──────────────────────────────────────────

  fun status_str = yam_status_str(s : Status) : LibC::Char*
  fun event_type_str = yam_event_type_str(t : EventType) : LibC::Char*
end
