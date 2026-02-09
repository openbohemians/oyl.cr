class Yam::PullParser
  @content : String

  def initialize(content : String | IO)
    input = content.is_a?(IO) ? content.gets_to_end : content
    @content = input
    @arena = LibYam.arena_new(input.bytesize < 4096 ? 4096_u64 : input.bytesize.to_u64)
    raise Yam::Error.new("Failed to allocate arena") unless @arena
    @parser = LibYam.parser_new(@content, @content.bytesize, @arena)
    raise Yam::Error.new("Failed to create parser") unless @parser
    @event = LibYam::Event.new
    @closed = false
    read_next
    raise "Expected STREAM_START" unless kind.stream_start?
  end

  def self.new(content, &)
    parser = new(content)
    yield parser ensure parser.close
  end

  def kind : Yam::EventKind
    Yam::EventKind.new(@event.type.value)
  end

  def value : String
    case kind
    when .scalar?, .alias?
      yam_str_to_s(@event.value)
    else
      raise "Expected SCALAR or ALIAS but was #{kind}"
    end
  end

  def tag : String?
    yam_str_to_s?(@event.tag)
  end

  def anchor : String?
    case kind
    when .scalar?, .sequence_start?, .mapping_start?, .alias?
      yam_str_to_s?(@event.anchor)
    else
      nil
    end
  end

  def scalar_style : Yam::ScalarStyle
    expect_kind Yam::EventKind::SCALAR
    Yam::ScalarStyle.new(@event.scalar_style.value + 1)
  end

  def sequence_style : Yam::SequenceStyle
    expect_kind Yam::EventKind::SEQUENCE_START
    @event.flow ? Yam::SequenceStyle::FLOW : Yam::SequenceStyle::BLOCK
  end

  def mapping_style : Yam::MappingStyle
    expect_kind Yam::EventKind::MAPPING_START
    @event.flow ? Yam::MappingStyle::FLOW : Yam::MappingStyle::BLOCK
  end

  def read_next : Yam::EventKind
    status = LibYam.parse_next(@parser, pointerof(@event))
    unless status == LibYam::Status::OK
      msg_ptr = LibYam.parser_error(@parser)
      mark = LibYam.parser_error_mark(@parser)
      msg = msg_ptr ? String.new(msg_ptr) : "Parse error"
      raise Yam::ParseException.new(msg, mark.line.to_i32, mark.col.to_i32)
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
    expect_kind Yam::EventKind::ALIAS
    anchor = yam_str_to_s?(@event.value)
    read_next
    anchor
  end

  def read_scalar : String
    expect_kind Yam::EventKind::SCALAR
    value = self.value
    read_next
    value
  end

  def read_stream_start
    read Yam::EventKind::STREAM_START
  end

  def read_stream_end
    read Yam::EventKind::STREAM_END
  end

  def read_document_start
    read Yam::EventKind::DOCUMENT_START
  end

  def read_document_end
    read Yam::EventKind::DOCUMENT_END
  end

  def read_sequence_start
    read Yam::EventKind::SEQUENCE_START
  end

  def read_sequence_end
    read Yam::EventKind::SEQUENCE_END
  end

  def read_mapping_start
    read Yam::EventKind::MAPPING_START
  end

  def read_mapping_end
    read Yam::EventKind::MAPPING_END
  end

  def read(expected_kind : Yam::EventKind) : Yam::EventKind
    expect_kind expected_kind
    read_next
  end

  def skip : Yam::EventKind
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

  # YAM marks are already 1-based
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

  def expect_kind(expected : Yam::EventKind) : Nil
    raise "Expected #{expected} but was #{kind}" unless kind == expected
  end

  def close : Nil
    return if @closed
    @closed = true
    LibYam.parser_free(@parser)
    LibYam.arena_free(@arena)
  end

  def finalize
    close
  end

  def raise(msg : String, line_number = self.start_line, column_number = self.start_column) : NoReturn
    ::raise Yam::ParseException.new(msg, line_number, column_number)
  end

  private def yam_str_to_s(s : LibYam::Str) : String
    if s.data.null? || s.len == 0
      ""
    else
      String.new(s.data, s.len)
    end
  end

  private def yam_str_to_s?(s : LibYam::Str) : String?
    if s.data.null? || s.len == 0
      nil
    else
      String.new(s.data, s.len)
    end
  end
end
