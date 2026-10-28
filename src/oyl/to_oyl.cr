require "base64"

# How each type writes itself with `Oyl::Builder#write`.

struct Nil
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.scalar "null"
  end
end

struct Bool
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.scalar to_s
  end
end

struct Int
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.scalar to_s
  end
end

struct Float
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.scalar(if nan?
      ".nan"
    elsif infinite?
      self > 0 ? ".inf" : "-.inf"
    else
      to_s
    end)
  end
end

class String
  def to_oyl(builder : Oyl::Builder) : Nil
    style = if includes?('\n')
              Oyl::ScalarStyle::LITERAL
            elsif Oyl::Builder.typed?(self)
              Oyl::ScalarStyle::DOUBLE_QUOTED
            else
              Oyl::ScalarStyle::ANY
            end
    builder.scalar self, style: style
  end
end

struct Symbol
  def to_oyl(builder : Oyl::Builder) : Nil
    to_s.to_oyl(builder)
  end
end

struct Time
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.scalar Time::Format::YAML_DATE.format(self), tag: "tag:yaml.org,2002:timestamp"
  end
end

struct Slice(T)
  def to_oyl(builder : Oyl::Builder) : Nil
    {% if T == UInt8 %}
      builder.scalar Base64.strict_encode(self), tag: "tag:yaml.org,2002:binary"
    {% else %}
      builder.sequence do
        each { |item| builder.write item }
      end
    {% end %}
  end
end

module Enumerable(T)
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.sequence do
      each { |item| builder.write item }
    end
  end
end

class Hash(K, V)
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.mapping do
      each do |key, value|
        builder.write key
        builder.write value
      end
    end
  end
end

struct NamedTuple
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.mapping do
      each do |key, value|
        builder.write key
        builder.write value
      end
    end
  end
end
