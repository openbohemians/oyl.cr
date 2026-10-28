require "./nodes/nodes"
require "./nodes/parser"

module Oyl::Nodes
  # Scalars keep their text, and `<<` merge keys stay as written unless
  # *merge* is given; *dialect* is as for `Oyl::PullParser`.
  def self.parse(content : String | IO, *, dialect : Oyl::Dialect = :yaml12, merge : Bool = false) : Document
    Parser.new(content, dialect: dialect, merge: merge, &.parse)
  end

  def self.parse_all(content : String | IO, *, dialect : Oyl::Dialect = :yaml12, merge : Bool = false) : Array(Document)
    Parser.new(content, dialect: dialect, merge: merge, &.parse_all)
  end
end
