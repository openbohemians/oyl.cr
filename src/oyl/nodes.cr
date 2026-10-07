require "./nodes/nodes"
require "./nodes/parser"

module Oyl::Nodes
  def self.parse(content : String | IO) : Document
    Parser.new(content, &.parse)
  end

  def self.parse_all(content : String | IO) : Array(Document)
    Parser.new(content, &.parse_all)
  end
end
