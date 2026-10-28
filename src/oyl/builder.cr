# Writes YAML with Oyl's emitter: a stream of documents, each holding one
# node. `#write` writes a Crystal value so that it reads back as the same
# value; `#scalar`, `#sequence` and `#mapping` write what they are given.
#
# ```
# yaml = Oyl::Builder.build do |b|
#   b.document do
#     b.mapping do
#       b.write "name"
#       b.write "oyl"
#       b.write "fast"
#       b.write true
#     end
#   end
# end
# yaml # => "name: oyl\nfast: true\n"
# ```
class Oyl::Builder
  @closed = false
  @output : String?

  def initialize(*, style : Oyl::EmitStyle = :block, indent : Int32 = 2)
    @arena = LibOyl.arena_new(4096)
    raise Oyl::Error.new("Failed to allocate arena") unless @arena
    @emitter = LibOyl.emitter_new(@arena)
    unless @emitter
      LibOyl.arena_free(@arena)
      raise Oyl::Error.new("Failed to create emitter")
    end
    LibOyl.emitter_set_style(@emitter, LibOyl::EmitStyle.new(style.value))
    LibOyl.emitter_set_indent(@emitter, indent)
    check LibOyl.emit_stream_start(@emitter)
  end

  # Yields a builder and returns the YAML it wrote.
  def self.build(*, style : Oyl::EmitStyle = :block, indent : Int32 = 2, &) : String
    builder = new(style: style, indent: indent)
    begin
      yield builder
      builder.to_s
    ensure
      builder.close
    end
  end

  # Writes a document around what the block writes. `---` and `...` are
  # written where YAML needs them, or always with *explicit_start* and
  # *explicit_end*.
  def document(*, explicit_start : Bool = false, explicit_end : Bool = false, &) : Nil
    check LibOyl.emit_document_start(emitter, !explicit_start)
    yield
    check LibOyl.emit_document_end(emitter, !explicit_end)
  end

  # Writes *value* as a scalar's text. With `ScalarStyle::ANY` (or
  # `PLAIN`) it is written plain when that reads back as the same text,
  # else quoted; YAML may still read that text as another type (`true`).
  # *tag* is a full tag (`tag:yaml.org,2002:str`, written `!!str`) or a
  # local one (`!foo`).
  def scalar(value : String, *, style : Oyl::ScalarStyle = :any, tag : String? = nil, anchor : String? = nil) : Nil
    c_style = style.any? ? LibOyl::ScalarStyle::Plain : LibOyl::ScalarStyle.new(style.value - 1)
    check LibOyl.emit_scalar(emitter, str(value), c_style, str(anchor), str(tag))
  end

  # Writes a sequence of what the block writes.
  def sequence(*, style : Oyl::SequenceStyle = :any, tag : String? = nil, anchor : String? = nil, &) : Nil
    check LibOyl.emit_sequence_start(emitter, str(anchor), str(tag), style.flow?)
    yield
    check LibOyl.emit_sequence_end(emitter)
  end

  # Writes a mapping of what the block writes: a key, then its value.
  def mapping(*, style : Oyl::MappingStyle = :any, tag : String? = nil, anchor : String? = nil, &) : Nil
    check LibOyl.emit_mapping_start(emitter, str(anchor), str(tag), style.flow?)
    yield
    check LibOyl.emit_mapping_end(emitter)
  end

  # Writes an alias to the node anchored *anchor*.
  def alias(anchor : String) : Nil
    check LibOyl.emit_alias(emitter, str(anchor))
  end

  # Writes *value* so that `Oyl.parse` reads it back as the same value,
  # whatever its dialect: a string is quoted where YAML 1.2, YAML 1.1 or
  # go-yaml v2 would read it as another type (`"yes"`, `"1:30"`, `""`),
  # and written as a literal block when it has line breaks. YAML 1.2 has
  # no plain form for `Time` and `Bytes`, so they are tagged `!!timestamp`
  # and `!!binary`. Each type writes itself with `to_oyl(builder)`, which
  # a type of your own can define.
  def write(value) : Nil
    value.to_oyl(self)
  end

  # Ends the stream, if it hasn't ended, and returns the YAML written.
  def to_s : String
    @output ||= begin
      check LibOyl.emit_stream_end(emitter)
      text = LibOyl.emitter_output(emitter)
      raise Oyl::Error.new("Failed to allocate the output") if text.data.null?
      String.new(text.data, text.len)
    end
  end

  def to_s(io : IO) : Nil
    io << to_s
  end

  def close : Nil
    return if @closed
    @closed = true
    LibOyl.emitter_free(@emitter)
    LibOyl.arena_free(@arena)
  end

  def finalize
    close
  end

  STR_TAG = "tag:yaml.org,2002:str".to_slice

  # Whether YAML 1.2, YAML 1.1 or go-yaml v2 reads *text*, written plain,
  # as anything but a string.
  def self.typed?(text : String) : Bool
    s = LibOyl::Str.new(data: text.to_unsafe, len: LibC::SizeT.new(text.bytesize))
    {LibOyl.schema_core, LibOyl.schema_yaml11, LibOyl.schema_goyaml2}.any? do |schema|
      tag = LibOyl.schema_resolve(schema, s, LibOyl::ScalarStyle::Plain)
      Slice.new(tag.data.as(UInt8*), tag.len) != STR_TAG
    end
  end

  private def emitter : LibOyl::Emitter*
    raise Oyl::Error.new("Builder is closed") if @closed
    @emitter
  end

  private def check(status : LibOyl::Status) : Nil
    return if status.ok?
    raise Oyl::Error.new("Can't write that YAML: #{String.new(LibOyl.status_str(status))}")
  end

  private def str(s : String?) : LibOyl::Str
    if s
      LibOyl::Str.new(data: s.to_unsafe, len: LibC::SizeT.new(s.bytesize))
    else
      LibOyl::Str.new
    end
  end
end
