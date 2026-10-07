require "base64"

# `Oyl::Any` is a convenient wrapper around all possible YAML core
# schema types (`Oyl::Any::Type`) and can be used for traversing
# dynamic or unknown YAML structures.
#
# ```
# require "oyl"
#
# data = Oyl.parse("---\nname: oyl\nversion: 0.1.0")
# data["name"].as_s    # => "oyl"
# data["version"].as_s # => "0.1.0"
# ```
struct Oyl::Any
  # All possible YAML core schema types.
  alias Type = Nil | Bool | Int64 | Float64 | String | Time | Bytes |
               Array(Oyl::Any) | Hash(Oyl::Any, Oyl::Any)

  # Returns the raw underlying value.
  getter raw : Type

  # Creates a `Oyl::Any` that wraps the given value.
  def initialize(@raw : Type)
  end

  # Creates a `Oyl::Any` from a `Oyl::Nodes::Node` using the
  # core schema for scalar resolution.
  def self.new(ctx : Oyl::ParseContext, node : Oyl::Nodes::Node) : Oyl::Any
    case node
    when Oyl::Nodes::Scalar
      any = new(Oyl::Schema::Core.parse_scalar(node))
      if anchor = node.anchor
        ctx.record_anchor(anchor, any)
      end
      any
    when Oyl::Nodes::Sequence
      ary = Array(Oyl::Any).new(node.size)
      node.each do |child|
        ary << new(ctx, child)
      end
      any = new(ary)
      if anchor = node.anchor
        ctx.record_anchor(anchor, any)
      end
      any
    when Oyl::Nodes::Mapping
      hash = Hash(Oyl::Any, Oyl::Any).new
      Oyl::Schema::Core.each(node) do |key_node, value_node|
        key = new(ctx, key_node)
        val = new(ctx, value_node)
        hash[key] = val
      end
      any = new(hash)
      if anchor = node.anchor
        ctx.record_anchor(anchor, any)
      end
      any
    when Oyl::Nodes::Alias
      anchor = node.value
      ctx.read_alias(anchor)
    else
      raise Oyl::Error.new("Unknown node type: #{node.class}")
    end
  end

  # Assumes the underlying value is an `Array` and returns the
  # element at the given *index*.
  def [](index : Int) : Oyl::Any
    case object = @raw
    when Array
      object[index]
    when Hash
      object[Oyl::Any.new(index.to_i64)]
    else
      raise Oyl::Error.new("Expected Array or Hash, not #{object.class}")
    end
  end

  # Assumes the underlying value is a `Hash` and returns the
  # value for the given *key*.
  def [](key : String) : Oyl::Any
    case object = @raw
    when Hash
      object[Oyl::Any.new(key)]
    else
      raise Oyl::Error.new("Expected Hash, not #{object.class}")
    end
  end

  # Returns the element at the given *index*, or `nil`.
  def []?(index : Int) : Oyl::Any?
    case object = @raw
    when Array
      object[index]?
    when Hash
      object[Oyl::Any.new(index.to_i64)]?
    else
      nil
    end
  end

  # Returns the value for the given *key*, or `nil`.
  def []?(key : String) : Oyl::Any?
    case object = @raw
    when Hash
      object[Oyl::Any.new(key)]?
    else
      nil
    end
  end

  # Traverses the depth of a structure and returns the value,
  # otherwise raises.
  def dig(index_or_key, *subkeys) : Oyl::Any
    self[index_or_key].dig(*subkeys)
  end

  # :ditto:
  def dig(index_or_key) : Oyl::Any
    self[index_or_key]
  end

  # Traverses the depth of a structure and returns the value,
  # or `nil`.
  def dig?(index_or_key, *subkeys) : Oyl::Any?
    self[index_or_key]?.try &.dig?(*subkeys)
  end

  # :ditto:
  def dig?(index_or_key) : Oyl::Any?
    self[index_or_key]?
  end

  # Returns the size of the underlying `Array` or `Hash`.
  def size : Int
    case object = @raw
    when Array
      object.size
    when Hash
      object.size
    else
      raise Oyl::Error.new("Expected Array or Hash for #size, not #{object.class}")
    end
  end

  # Checks that the underlying value is `Nil`, and returns `nil`.
  def as_nil : Nil
    @raw.as(Nil)
  end

  # Checks that the underlying value is `Bool`, and returns its value.
  def as_bool : Bool
    @raw.as(Bool)
  end

  # Checks that the underlying value is `Bool`, and returns its
  # value. Returns `nil` otherwise.
  def as_bool? : Bool?
    @raw.as?(Bool)
  end

  # Checks that the underlying value is `Int64`, and returns its value.
  def as_i : Int32
    @raw.as(Int64).to_i
  end

  # Checks that the underlying value is `Int64`, and returns its
  # value as `Int32`. Returns `nil` otherwise.
  def as_i? : Int32?
    @raw.as?(Int64).try(&.to_i)
  end

  # Checks that the underlying value is `Int64`, and returns its value.
  def as_i64 : Int64
    @raw.as(Int64)
  end

  # Checks that the underlying value is `Int64`, and returns its
  # value. Returns `nil` otherwise.
  def as_i64? : Int64?
    @raw.as?(Int64)
  end

  # Checks that the underlying value is `Float64`, and returns its value.
  def as_f : Float64
    @raw.as(Float64)
  end

  # Checks that the underlying value is `Float64`, and returns its
  # value. Returns `nil` otherwise.
  def as_f? : Float64?
    @raw.as?(Float64)
  end

  # Checks that the underlying value is `Float64`, and returns
  # its value as `Float32`.
  def as_f32 : Float32
    @raw.as(Float64).to_f32
  end

  # Checks that the underlying value is `Float64`, and returns
  # its value as `Float32`. Returns `nil` otherwise.
  def as_f32? : Float32?
    @raw.as?(Float64).try(&.to_f32)
  end

  # Checks that the underlying value is `String`, and returns its value.
  def as_s : String
    @raw.as(String)
  end

  # Checks that the underlying value is `String`, and returns its
  # value. Returns `nil` otherwise.
  def as_s? : String?
    @raw.as?(String)
  end

  # Checks that the underlying value is `Time`, and returns its value.
  def as_time : Time
    @raw.as(Time)
  end

  # Checks that the underlying value is `Time`, and returns its
  # value. Returns `nil` otherwise.
  def as_time? : Time?
    @raw.as?(Time)
  end

  # Checks that the underlying value is `Array`, and returns it.
  def as_a : Array(Oyl::Any)
    @raw.as(Array)
  end

  # Checks that the underlying value is `Array`, and returns it.
  # Returns `nil` otherwise.
  def as_a? : Array(Oyl::Any)?
    @raw.as?(Array)
  end

  # Checks that the underlying value is `Hash`, and returns it.
  def as_h : Hash(Oyl::Any, Oyl::Any)
    @raw.as(Hash)
  end

  # Checks that the underlying value is `Hash`, and returns it.
  # Returns `nil` otherwise.
  def as_h? : Hash(Oyl::Any, Oyl::Any)?
    @raw.as?(Hash)
  end

  # Checks that the underlying value is `Bytes`, and returns it.
  def as_bytes : Bytes
    @raw.as(Bytes)
  end

  # Checks that the underlying value is `Bytes`, and returns it.
  # Returns `nil` otherwise.
  def as_bytes? : Bytes?
    @raw.as?(Bytes)
  end

  # Returns a new `Oyl::Any` with the `raw` value `dup`ed.
  def dup
    Oyl::Any.new(raw.dup)
  end

  # Returns a new `Oyl::Any` with the `raw` value `clone`ed.
  def clone
    Oyl::Any.new(raw.clone)
  end

  def_equals_and_hash raw

  def to_s(io : IO) : Nil
    raw.to_s(io)
  end

  def inspect(io : IO) : Nil
    raw.inspect(io)
  end
end

class Object
  def ===(other : Oyl::Any)
    self === other.raw
  end
end

struct Value
  def ==(other : Oyl::Any)
    self == other.raw
  end
end

struct Struct
  def ==(other : Oyl::Any)
    self == other.raw
  end
end

class Reference
  def ==(other : Oyl::Any)
    self == other.raw
  end
end

class Array
  def ==(other : Oyl::Any)
    self == other.raw
  end
end

class Hash
  def ==(other : Oyl::Any)
    self == other.raw
  end
end

class Regex
  def ===(other : Oyl::Any)
    value = self === other.raw
    $~ = $~
    value
  end
end
