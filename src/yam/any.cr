require "base64"

# `Yam::Any` is a convenient wrapper around all possible YAML core
# schema types (`Yam::Any::Type`) and can be used for traversing
# dynamic or unknown YAML structures.
#
# ```
# require "yam"
#
# data = Yam.parse("---\nname: yam\nversion: 0.1.0")
# data["name"].as_s   # => "yam"
# data["version"].as_s # => "0.1.0"
# ```
struct Yam::Any
  # All possible YAML core schema types.
  alias Type = Nil | Bool | Int64 | Float64 | String | Time | Bytes |
               Array(Yam::Any) | Hash(Yam::Any, Yam::Any)

  # Returns the raw underlying value.
  getter raw : Type

  # Creates a `Yam::Any` that wraps the given value.
  def initialize(@raw : Type)
  end

  # Creates a `Yam::Any` from a `Yam::Nodes::Node` using the
  # core schema for scalar resolution.
  def self.new(ctx : Yam::ParseContext, node : Yam::Nodes::Node) : Yam::Any
    case node
    when Yam::Nodes::Scalar
      any = new(Yam::Schema::Core.parse_scalar(node))
      if anchor = node.anchor
        ctx.record_anchor(anchor, any)
      end
      any
    when Yam::Nodes::Sequence
      ary = Array(Yam::Any).new(node.size)
      node.each do |child|
        ary << new(ctx, child)
      end
      any = new(ary)
      if anchor = node.anchor
        ctx.record_anchor(anchor, any)
      end
      any
    when Yam::Nodes::Mapping
      hash = Hash(Yam::Any, Yam::Any).new
      Yam::Schema::Core.each(node) do |key_node, value_node|
        key = new(ctx, key_node)
        val = new(ctx, value_node)
        hash[key] = val
      end
      any = new(hash)
      if anchor = node.anchor
        ctx.record_anchor(anchor, any)
      end
      any
    when Yam::Nodes::Alias
      anchor = node.value
      ctx.read_alias(anchor)
    else
      raise Yam::Error.new("Unknown node type: #{node.class}")
    end
  end

  # Assumes the underlying value is an `Array` and returns the
  # element at the given *index*.
  def [](index : Int) : Yam::Any
    case object = @raw
    when Array
      object[index]
    when Hash
      object[Yam::Any.new(index.to_i64)]
    else
      raise Yam::Error.new("Expected Array or Hash, not #{object.class}")
    end
  end

  # Assumes the underlying value is a `Hash` and returns the
  # value for the given *key*.
  def [](key : String) : Yam::Any
    case object = @raw
    when Hash
      object[Yam::Any.new(key)]
    else
      raise Yam::Error.new("Expected Hash, not #{object.class}")
    end
  end

  # Returns the element at the given *index*, or `nil`.
  def []?(index : Int) : Yam::Any?
    case object = @raw
    when Array
      object[index]?
    when Hash
      object[Yam::Any.new(index.to_i64)]?
    else
      nil
    end
  end

  # Returns the value for the given *key*, or `nil`.
  def []?(key : String) : Yam::Any?
    case object = @raw
    when Hash
      object[Yam::Any.new(key)]?
    else
      nil
    end
  end

  # Traverses the depth of a structure and returns the value,
  # otherwise raises.
  def dig(index_or_key, *subkeys) : Yam::Any
    self[index_or_key].dig(*subkeys)
  end

  # :ditto:
  def dig(index_or_key) : Yam::Any
    self[index_or_key]
  end

  # Traverses the depth of a structure and returns the value,
  # or `nil`.
  def dig?(index_or_key, *subkeys) : Yam::Any?
    self[index_or_key]?.try &.dig?(*subkeys)
  end

  # :ditto:
  def dig?(index_or_key) : Yam::Any?
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
      raise Yam::Error.new("Expected Array or Hash for #size, not #{object.class}")
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
  def as_a : Array(Yam::Any)
    @raw.as(Array)
  end

  # Checks that the underlying value is `Array`, and returns it.
  # Returns `nil` otherwise.
  def as_a? : Array(Yam::Any)?
    @raw.as?(Array)
  end

  # Checks that the underlying value is `Hash`, and returns it.
  def as_h : Hash(Yam::Any, Yam::Any)
    @raw.as(Hash)
  end

  # Checks that the underlying value is `Hash`, and returns it.
  # Returns `nil` otherwise.
  def as_h? : Hash(Yam::Any, Yam::Any)?
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

  # Returns a new `Yam::Any` with the `raw` value `dup`ed.
  def dup
    Yam::Any.new(raw.dup)
  end

  # Returns a new `Yam::Any` with the `raw` value `clone`ed.
  def clone
    Yam::Any.new(raw.clone)
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
  def ===(other : Yam::Any)
    self === other.raw
  end
end

struct Value
  def ==(other : Yam::Any)
    self == other.raw
  end
end

struct Struct
  def ==(other : Yam::Any)
    self == other.raw
  end
end

class Reference
  def ==(other : Yam::Any)
    self == other.raw
  end
end

class Array
  def ==(other : Yam::Any)
    self == other.raw
  end
end

class Hash
  def ==(other : Yam::Any)
    self == other.raw
  end
end

class Regex
  def ===(other : Yam::Any)
    value = self === other.raw
    $~ = $~
    value
  end
end
