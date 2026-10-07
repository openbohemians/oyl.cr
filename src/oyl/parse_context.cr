class Oyl::ParseContext
  @anchors = {} of String => Oyl::Any

  def record_anchor(anchor : String, value : Oyl::Any)
    @anchors[anchor] = value
  end

  def read_alias(anchor : String) : Oyl::Any
    @anchors[anchor]? || raise Oyl::Error.new("Unknown alias: #{anchor}")
  end
end
