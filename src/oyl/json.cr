module Oyl
  # Converts the first YAML document in *data* to JSON, in Oyl's C, with
  # no Crystal values in between. Scalars are typed by *dialect* (a
  # `%YAML 1.1` document by YAML 1.1), aliases are expanded, `<<` merge
  # keys applied, keys sorted, and numbers and strings written as Go's
  # encoding/json writes them. A key set twice keeps its last value, or
  # with *strict* raises. An empty stream is `null`.
  #
  # With `dialect: :goyaml2`, the output is byte for byte what
  # sigs.k8s.io/yaml's `YAMLToJSON` writes, as Kubernetes reads YAML.
  #
  # ```
  # Oyl.yaml_to_json("b: yes\na: 0777\n")                    # => %({"a":777,"b":"yes"})
  # Oyl.yaml_to_json("b: yes\na: 0777\n", dialect: :goyaml2) # => %({"a":511,"b":true})
  # ```
  def self.yaml_to_json(data : String | IO, *, dialect : Dialect = :yaml12, strict : Bool = false) : String
    each_json(data, dialect, strict) { |json| return json }
    "null"
  end

  # Converts each YAML document in *data* to JSON (see `yaml_to_json`).
  def self.yaml_to_json_all(data : String | IO, *, dialect : Dialect = :yaml12, strict : Bool = false) : Array(String)
    docs = [] of String
    each_json(data, dialect, strict) { |json| docs << json }
    docs
  end

  private def self.each_json(data, dialect, strict, &)
    input = data.is_a?(IO) ? data.gets_to_end : data
    arena = LibOyl.arena_new(input.bytesize < 4096 ? 4096_u64 : input.bytesize.to_u64)
    raise Error.new("Failed to allocate arena") unless arena
    begin
      parser = LibOyl.parser_new(input, input.bytesize, arena)
      raise Error.new("Failed to create parser") unless parser
      begin
        # aliasing and merging are bounded by the JSON writer itself
        LibOyl.parser_set_max_events(parser, 0)
        LibOyl.parser_set_dialect(parser, dialect)
        flags = strict ? LibOyl::JSON_STRICT : 0_u32
        loop do
          status = LibOyl.parse_json(parser, nil, flags, out json)
          unless status.ok?
            msg_ptr = LibOyl.parser_error(parser)
            mark = LibOyl.parser_error_mark(parser)
            raise ParseException.new(msg_ptr ? String.new(msg_ptr) : "Parse error", mark.line.to_i32, mark.col.to_i32)
          end
          break unless json.data
          # the JSON lives in the parser's arena: copy it out
          yield String.new(json.data, json.len)
        end
      ensure
        LibOyl.parser_free(parser)
      end
    ensure
      LibOyl.arena_free(arena)
    end
  end
end
