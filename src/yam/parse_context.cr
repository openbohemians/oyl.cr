class Yam::ParseContext
  @anchors = {} of String => Yam::Any

  def record_anchor(anchor : String, value : Yam::Any)
    @anchors[anchor] = value
  end

  def read_alias(anchor : String) : Yam::Any
    @anchors[anchor]? || raise Yam::Error.new("Unknown alias: #{anchor}")
  end
end
