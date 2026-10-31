module Oyl
  # A change `Oyl.upgrade` made, or a difference it found and left.
  struct Fix
    enum Kind
      # A scalar YAML 1.2 reads otherwise
      Scalar = 1
      # `%YAML 1.1`, removed
      Directive
      # `\'` in double quotes (go-yaml v2's), the backslash removed
      Escape
      # A block scalar ending the input without a line break
      Chomp
      # A key go-yaml v2's merge order overrides, commented out
      Merge
      # A quoted scalar's or flow collection's line not indented past its
      # block, indented
      Indent
    end

    getter kind : Kind
    # Where, in the input (from 1)
    getter line : Int32
    getter column : Int32
    # The input's text there
    getter before : String
    # What replaced it (empty if removed, or left)
    getter after : String
    # What each reader reads, and what was done
    getter message : String

    def initialize(@kind, @line, @column, @before, @after, @fixed : Bool, @message)
    end

    # False when only reported, the input left as it was.
    def fixed? : Bool
      @fixed
    end
  end

  # Makes *data*, a YAML 1.1 file, read the same in YAML 1.2, rewriting
  # only what a 1.2 reader would read otherwise and keeping every other
  # byte: comments, layout, quoting, key order. Returns the new text and
  # each change, with what couldn't be fixed (reported, left as it was).
  #
  # *from* names the file's reader: `:yaml11` for PyYAML (and Ansible),
  # `:goyaml2` for go-yaml v2 (Kubernetes); afterwards the file reads the
  # same in it and in YAML 1.2. Left `nil`, the reader is unknown: what
  # PyYAML and go-yaml v2 read alike is fixed, what they read otherwise
  # reported. Values keep the reader's meaning, keys their text (as YAML
  # 1.2 reads them); *keys_keep_value* and *values_keep_text* flip either.
  #
  # ```
  # text, fixes = Oyl.upgrade("on: push\nenabled: yes\nmode: 0755\n", from: :yaml11)
  # text # => %("on": push\nenabled: true\nmode: !!int 0o755\n)
  # ```
  #
  # Raises `Oyl::ParseException` for input the reader can't read, or if
  # the output would read otherwise than decided (a bug, never a file).
  def self.upgrade(data : String | IO, *, from : Dialect? = nil, keys_keep_value : Bool = false,
                   values_keep_text : Bool = false) : {String, Array(Fix)}
    input = data.is_a?(IO) ? data.gets_to_end : data
    arena = LibOyl.arena_new(input.bytesize < 4096 ? 4096_u64 : input.bytesize.to_u64)
    raise Error.new("Failed to allocate arena") unless arena
    begin
      parser = LibOyl.parser_new(input, input.bytesize, arena)
      raise Error.new("Failed to create parser") unless parser
      begin
        LibOyl.parser_set_dialect(parser, from) if from
        flags = 0_u32
        flags |= LibOyl::UPGRADE_KEYS_KEEP_VALUE if keys_keep_value
        flags |= LibOyl::UPGRADE_VALUES_KEEP_TEXT if values_keep_text
        status = LibOyl.upgrade(parser, flags, out text, out fixes, out nfixes)
        unless status.ok?
          msg_ptr = LibOyl.parser_error(parser)
          mark = LibOyl.parser_error_mark(parser)
          raise ParseException.new(msg_ptr ? String.new(msg_ptr) : "Upgrade failed", mark.line.to_i32, mark.col.to_i32)
        end
        # the output and fixes live in the parser's arena: copy them out
        result = text.data ? String.new(text.data, text.len) : ""
        list = Array(Fix).new(nfixes) do |i|
          f = fixes[i]
          Fix.new(Fix::Kind.new(f.kind), f.mark.line.to_i32, f.mark.col.to_i32,
            String.new(f.before.data, f.before.len),
            f.after.len > 0 ? String.new(f.after.data, f.after.len) : "",
            f.fixed, String.new(f.message))
        end
        {result, list}
      ensure
        LibOyl.parser_free(parser)
      end
    ensure
      LibOyl.arena_free(arena)
    end
  end
end
