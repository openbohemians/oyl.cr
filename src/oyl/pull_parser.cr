class Oyl::PullParser
  @content : String

  # Safety limits: *max_events* bounds the events in the stream (alias and
  # merge expansion included) and *max_depth* the nesting of collections;
  # exceeding either raises `Oyl::ParseException`. `nil` keeps the
  # library's defaults (10,000 events, depth 256); 0 disables a limit.
  def initialize(content : String | IO, *, max_events : Int32? = nil, max_depth : Int32? = nil)
    input = content.is_a?(IO) ? content.gets_to_end : content
    @content = input
    @arena = LibOyl.arena_new(input.bytesize < 4096 ? 4096_u64 : input.bytesize.to_u64)
    raise Oyl::Error.new("Failed to allocate arena") unless @arena
    @parser = LibOyl.parser_new(@content, @content.bytesize, @arena)
    raise Oyl::Error.new("Failed to create parser") unless @parser
    max_events.try { |max| LibOyl.parser_set_max_events(@parser, max) }
    max_depth.try { |max| LibOyl.parser_set_max_depth(@parser, max) }
    @event = LibOyl::Event.new
    @closed = false
    read_next
    raise "Expected STREAM_START" unless kind.stream_start?
  end

  def self.new(content, *, max_events : Int32? = nil, max_depth : Int32? = nil, &)
    parser = new(content, max_events: max_events, max_depth: max_depth)
    yield parser ensure parser.close
  end

  def kind : Oyl::EventKind
    Oyl::EventKind.new(@event.type.value)
  end

  def value : String
    case kind
    when .scalar?, .alias?
      oyl_str_to_s(@event.value)
    else
      raise "Expected SCALAR or ALIAS but was #{kind}"
    end
  end

  def tag : String?
    oyl_str_to_s?(@event.tag)
  end

  def anchor : String?
    case kind
    when .scalar?, .sequence_start?, .mapping_start?, .alias?
      oyl_str_to_s?(@event.anchor)
    else
      nil
    end
  end

  def scalar_style : Oyl::ScalarStyle
    expect_kind Oyl::EventKind::SCALAR
    Oyl::ScalarStyle.new(@event.scalar_style.value + 1)
  end

  def sequence_style : Oyl::SequenceStyle
    expect_kind Oyl::EventKind::SEQUENCE_START
    @event.flow ? Oyl::SequenceStyle::FLOW : Oyl::SequenceStyle::BLOCK
  end

  def mapping_style : Oyl::MappingStyle
    expect_kind Oyl::EventKind::MAPPING_START
    @event.flow ? Oyl::MappingStyle::FLOW : Oyl::MappingStyle::BLOCK
  end

  def read_next : Oyl::EventKind
    # the parser owns the event; copy it (its strings point into the input
    # and the arena, which live as long as this parser)
    status = LibOyl.parse_next(@parser, out event)
    @event = event.value if status == LibOyl::Status::OK
    unless status == LibOyl::Status::OK
      msg_ptr = LibOyl.parser_error(@parser)
      mark = LibOyl.parser_error_mark(@parser)
      msg = msg_ptr ? String.new(msg_ptr) : "Parse error"
      raise Oyl::ParseException.new(msg, mark.line.to_i32, mark.col.to_i32)
    end
    kind
  end

  def read_stream(&)
    read_stream_start
    value = yield
    read_stream_end
    value
  end

  def read_document(&)
    read_document_start
    value = yield
    read_document_end
    value
  end

  def read_sequence(&)
    read_sequence_start
    value = yield
    read_sequence_end
    value
  end

  def read_mapping(&)
    read_mapping_start
    value = yield
    read_mapping_end
    value
  end

  def read_alias : String?
    expect_kind Oyl::EventKind::ALIAS
    anchor = oyl_str_to_s?(@event.value)
    read_next
    anchor
  end

  def read_scalar : String
    expect_kind Oyl::EventKind::SCALAR
    value = self.value
    read_next
    value
  end

  def read_stream_start
    read Oyl::EventKind::STREAM_START
  end

  def read_stream_end
    read Oyl::EventKind::STREAM_END
  end

  def read_document_start
    read Oyl::EventKind::DOCUMENT_START
  end

  def read_document_end
    read Oyl::EventKind::DOCUMENT_END
  end

  def read_sequence_start
    read Oyl::EventKind::SEQUENCE_START
  end

  def read_sequence_end
    read Oyl::EventKind::SEQUENCE_END
  end

  def read_mapping_start
    read Oyl::EventKind::MAPPING_START
  end

  def read_mapping_end
    read Oyl::EventKind::MAPPING_END
  end

  def read(expected_kind : Oyl::EventKind) : Oyl::EventKind
    expect_kind expected_kind
    read_next
  end

  def skip : Oyl::EventKind
    case kind
    when .scalar?, .alias?
      read_next
    when .sequence_start?
      read_next
      until kind.sequence_end?
        skip
      end
      read_next
    when .mapping_start?
      read_next
      until kind.mapping_end?
        skip
        skip
      end
      read_next
    when .document_start?
      read_next
      until kind.document_end?
        skip
      end
      read_next
    when .stream_start?
      read_next
      until kind.stream_end?
        skip
      end
      read_next
    else
      read_next
    end
  end

  def location : {Int32, Int32}
    {start_line, start_column}
  end

  # Oyl marks are already 1-based
  def start_line : Int32
    @event.start.line.to_i32
  end

  def start_column : Int32
    @event.start.col.to_i32
  end

  def end_line : Int32
    @event.end_.line.to_i32
  end

  def end_column : Int32
    @event.end_.col.to_i32
  end

  def expect_kind(expected : Oyl::EventKind) : Nil
    raise "Expected #{expected} but was #{kind}" unless kind == expected
  end

  def close : Nil
    return if @closed
    @closed = true
    LibOyl.parser_free(@parser)
    LibOyl.arena_free(@arena)
  end

  def finalize
    close
  end

  def raise(msg : String, line_number = self.start_line, column_number = self.start_column) : NoReturn
    ::raise Oyl::ParseException.new(msg, line_number, column_number)
  end

  private def oyl_str_to_s(s : LibOyl::Str) : String
    if s.data.null? || s.len == 0
      ""
    else
      String.new(s.data, s.len)
    end
  end

  private def oyl_str_to_s?(s : LibOyl::Str) : String?
    if s.data.null? || s.len == 0
      nil
    else
      String.new(s.data, s.len)
    end
  end
end
