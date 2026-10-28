require "base64"

module Oyl
  private TAG_BINARY    = "tag:yaml.org,2002:binary"
  private TAG_TIMESTAMP = "tag:yaml.org,2002:timestamp"

  # What varies between YAML worlds: the types plain scalars resolve to,
  # a few syntax leniencies, and how `<<` merges. A document chooses
  # YAML 1.1 with a `%YAML 1.1` directive; the parser's dialect reads the
  # others.
  enum Dialect
    # YAML 1.2 with its Core schema: `null`, `~` and empty; `true` and
    # `false`; decimal, `0o` and `0x` integers; floats, `.inf` and `.nan`.
    # Everything else, dates included, is a string.
    YAML12
    # YAML 1.1 as PyYAML reads it: also `yes`/`no`/`on`/`off`, 0-octal,
    # `0b` binary, base 60 and timestamps.
    YAML11
    # go-yaml v2's types, as Kubernetes reads YAML: YAML 1.1's and `y`/`n`.
    # (Its merge order, where a `<<` overrides the keys written before
    # it, isn't applied: merges here keep the keys written, as YAML's do.)
    GOYAML2

    # :nodoc:
    def self.from(dialect : LibOyl::Dialect*) : self
      values.find { |d| d.to_unsafe == dialect } || begin
        LibOyl.dialect_version(dialect, out major, out minor)
        {major, minor} == {1, 1} ? YAML11 : YAML12
      end
    end

    def to_unsafe : LibOyl::Dialect*
      case self
      in YAML12  then LibOyl.dialect_yaml12
      in YAML11  then LibOyl.dialect_yaml11
      in GOYAML2 then LibOyl.dialect_goyaml2
      end
    end

    # The value of *scalar* in this dialect: resolved by its types when
    # plain and untagged, read by its tag's rules when tagged, and a
    # string when quoted. Raises `Oyl::ParseException` for a tagged
    # scalar its tag doesn't accept, as `!!int abc`.
    def value(scalar : Nodes::Scalar) : Nil | Bool | Int64 | UInt64 | Float64 | String | Time | Bytes
      text = scalar.value
      tag = scalar.tag
      style = scalar.style.any? ? LibOyl::ScalarStyle::Plain : LibOyl::ScalarStyle.new(scalar.style.value - 1)
      schema = LibOyl.dialect_schema(self)

      status = LibOyl.schema_value_of(schema, str(text), style, str(tag || ""), out value)
      unless status.ok?
        tag ||= resolve(schema, text, style)
        raise ParseException.new("Invalid #{tag.lchop("tag:yaml.org,2002:")}", scalar.start_line, scalar.start_column)
      end

      case value.kind
      when .null?  then nil
      when .bool?  then value.data.b
      when .int?   then value.data.i
      when .uint?  then value.data.u
      when .float? then value.data.f
      else
        s = value.data.s
        string = s.data == text.to_unsafe && s.len == text.bytesize ? text : String.new(s.data, s.len)
        # YAML 1.2's types have no timestamps; the others' come as text
        tag ||= resolve(schema, text, style) if style.plain? && !yaml12?
        case tag
        when TAG_BINARY
          begin
            Base64.decode(string)
          rescue ex : Base64::Error
            raise ParseException.new("Invalid binary: #{ex.message}", scalar.start_line, scalar.start_column)
          end
        when TAG_TIMESTAMP
          Time::Format::YAML_DATE.parse?(string) ||
            (scalar.tag ? raise ParseException.new("Invalid timestamp", scalar.start_line, scalar.start_column) : string)
        else
          string
        end
      end
    end

    private def resolve(schema, text, style) : String
      tag = LibOyl.schema_resolve(schema, str(text), style)
      String.new(tag.data, tag.len)
    end

    private def str(s : String) : LibOyl::Str
      LibOyl::Str.new(data: s.to_unsafe, len: LibC::SizeT.new(s.bytesize))
    end
  end
end
