# Provides utility methods for YAML core schema scalar resolution.
#
# Handles null, bool, int, float, infinity/NaN, timestamps,
# and base64-encoded binary. Compatible with Crystal's
# `YAML::Schema::Core` conventions (YAML 1.1 extended booleans:
# yes/no/on/off).
module Oyl::Schema::Core
  # Parses a scalar from a PullParser, taking style and tag into
  # account, then advances the pull parser.
  def self.parse_scalar(pull_parser : Oyl::PullParser) : Nil | Bool | Int64 | Float64 | String | Time | Bytes
    string = pull_parser.value

    process_scalar_tag(string, pull_parser.tag, pull_parser.location) do |value|
      return value
    end

    unless pull_parser.scalar_style.plain?
      return string
    end

    parse_scalar(string)
  end

  # Parses a scalar from a Nodes::Scalar, taking style and tag
  # into account.
  def self.parse_scalar(node : Oyl::Nodes::Scalar) : Nil | Bool | Int64 | Float64 | String | Time | Bytes
    string = node.value

    process_scalar_tag(string, node.tag, {node.start_line, node.start_column}) do |value|
      return value
    end

    unless node.style.plain?
      return string
    end

    parse_scalar(string)
  end

  # Parses a plain scalar string according to the core schema.
  def self.parse_scalar(string : String) : Nil | Bool | Int64 | Float64 | String | Time | Bytes
    if parse_null?(string)
      return nil
    end

    value = parse_bool?(string)
    return value unless value.nil?

    value = parse_float_infinity_and_nan?(string)
    return value if value

    case string
    when .starts_with?("0x"),
         .starts_with?("+0x"),
         .starts_with?("-0x")
      value = string.to_i64?(base: 16, prefix: true)
      value || string
    when .starts_with?("0."),
         .starts_with?('.')
      value = parse_float?(string)
      value || string
    when .starts_with?('0')
      return 0_i64 if string.size == 1
      value = string.to_i64?(base: 8, prefix: true, leading_zero_is_octal: true)
      value || string
    when .starts_with?('-'),
         .starts_with?('+')
      value = parse_number?(string)
      value || string
    else
      if string[0]?.try(&.ascii_number?)
        value = parse_number?(string)
        return value if value

        value = parse_time?(string)
        return value if value
      end

      string
    end
  end

  # Returns whether a string is reserved and must not be output
  # with a plain style.
  def self.reserved_string?(string) : Bool
    !parse_scalar(string).is_a?(String)
  end

  # If `node` parses to a null value, returns `nil`, otherwise
  # invokes the given block.
  def self.parse_null_or(node : Oyl::Nodes::Node, &)
    unless parse_null?(node)
      yield
    end
  end

  # Returns `true` if *node* parses to a null value.
  def self.parse_null?(node : Oyl::Nodes::Node)
    if node.is_a?(Oyl::Nodes::Scalar)
      parse_null?(node.value) && node.style.plain?
    else
      false
    end
  end

  # Iterates a mapping's keys and values, resolving merge keys
  # (`<<`) recursively with cycle detection.
  def self.each(node : Oyl::Nodes::Mapping, &)
    stack = [{node, 0}]
    visited = Set(Oyl::Nodes::Mapping).new

    until stack.empty?
      mapping, index = stack.pop

      visited << mapping

      while index < mapping.nodes.size
        key = mapping.nodes[index]
        index += 1

        value = mapping.nodes[index]
        index += 1

        if key.is_a?(Oyl::Nodes::Scalar) &&
           key.value == "<<" &&
           key.tag != "tag:yaml.org,2002:str" &&
           solve_merge(stack, mapping, index, value, visited)
          break
        else
          yield({key, value})
        end
      end
    end
  end

  private def self.solve_merge(stack, mapping, index, value, visited)
    value = value.resolved if value.is_a?(Oyl::Nodes::Alias)

    case value
    when Oyl::Nodes::Mapping
      stack.push({mapping, index})

      unless visited.includes?(value)
        stack.push({value, 0})
      end

      true
    when Oyl::Nodes::Sequence
      all_mappings = value.nodes.all? do |elem|
        elem = elem.resolved if elem.is_a?(Oyl::Nodes::Alias)
        elem.is_a?(Oyl::Nodes::Mapping)
      end

      if all_mappings
        stack.push({mapping, index})

        value.each do |elem|
          elem = elem.resolved if elem.is_a?(Oyl::Nodes::Alias)
          mapping = elem.as(Oyl::Nodes::Mapping)

          unless visited.includes?(mapping)
            stack.push({mapping, 0})
          end
        end

        true
      else
        false
      end
    else
      false
    end
  end

  protected def self.parse_binary(string, location) : Bytes
    Base64.decode(string)
  rescue ex : Base64::Error
    raise Oyl::ParseException.new("Error decoding Base64: #{ex.message}", *location)
  end

  protected def self.parse_bool(string, location) : Bool
    value = parse_bool?(string)
    unless value.nil?
      return value
    end

    raise Oyl::ParseException.new("Invalid bool", *location)
  end

  protected def self.parse_int(string : String, location) : Int64
    return 0_i64 if string == "0"

    string.to_i64?(underscore: true, prefix: true, leading_zero_is_octal: true) ||
      raise(Oyl::ParseException.new("Invalid int", *location))
  end

  protected def self.parse_float(string, location) : Float64
    parse_float_infinity_and_nan?(string) ||
      parse_float?(string) ||
      raise(Oyl::ParseException.new("Invalid float", *location))
  end

  protected def self.parse_null(string, location) : Nil
    if parse_null?(string)
      return nil
    end

    raise Oyl::ParseException.new("Invalid null", *location)
  end

  protected def self.parse_time(string, location) : Time
    parse_time?(string) ||
      raise(Oyl::ParseException.new("Invalid timestamp", *location))
  end

  protected def self.process_scalar_tag(string, tag, location, &)
    case tag
    when "tag:yaml.org,2002:binary"
      yield parse_binary(string, location)
    when "tag:yaml.org,2002:bool"
      yield parse_bool(string, location)
    when "tag:yaml.org,2002:float"
      yield parse_float(string, location)
    when "tag:yaml.org,2002:int"
      yield parse_int(string, location)
    when "tag:yaml.org,2002:null"
      yield parse_null(string, location)
    when "tag:yaml.org,2002:str"
      yield string
    when "tag:yaml.org,2002:timestamp"
      yield parse_time(string, location)
    else
      # not a tag we support
    end
  end

  private def self.parse_null?(string)
    case string
    when .empty?, "~", "null", "Null", "NULL"
      true
    else
      false
    end
  end

  private def self.parse_bool?(string)
    case string
    when "yes", "Yes", "YES", "true", "True", "TRUE", "on", "On", "ON"
      true
    when "no", "No", "NO", "false", "False", "FALSE", "off", "Off", "OFF"
      false
    else
      nil
    end
  end

  private def self.parse_number?(string)
    parse_int?(string) || parse_float?(string)
  end

  private def self.parse_int?(string)
    string.to_i64?(underscore: true, leading_zero_is_octal: true)
  end

  private def self.parse_float?(string)
    string = string.delete('_') if string.includes?('_')
    string.to_f64?
  end

  private def self.parse_float_infinity_and_nan?(string)
    case string
    when ".inf", ".Inf", ".INF", "+.inf", "+.Inf", "+.INF"
      Float64::INFINITY
    when "-.inf", "-.Inf", "-.INF"
      -Float64::INFINITY
    when ".nan", ".NaN", ".NAN"
      Float64::NAN
    else
      nil
    end
  end

  private def self.parse_time?(string)
    return nil if string.size < 8
    Time::Format::YAML_DATE.parse?(string)
  end
end
