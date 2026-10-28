class Oyl::ParseContext
  getter dialect : Oyl::Dialect
  @anchors = {} of String => Oyl::Any

  def initialize(@dialect : Oyl::Dialect = :yaml12)
  end

  def record_anchor(anchor : String, value : Oyl::Any)
    @anchors[anchor] = value
  end

  def read_alias(anchor : String) : Oyl::Any
    @anchors[anchor]? || raise Oyl::Error.new("Unknown alias: #{anchor}")
  end
end
