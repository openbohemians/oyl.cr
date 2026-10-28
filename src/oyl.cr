require "./oyl/lib_oyl"
require "./oyl/enums"
require "./oyl/error"
require "./oyl/dialect"
require "./oyl/pull_parser"
require "./oyl/nodes"
require "./oyl/parse_context"
require "./oyl/any"

module Oyl
  VERSION = "0.3.0"

  # Parses a YAML document and returns a `Oyl::Any`. Scalars are typed
  # by YAML 1.2's Core schema, or by *dialect*, and a document saying
  # `%YAML 1.1` by YAML 1.1's; `<<` merge keys are applied.
  #
  # ```
  # data = Oyl.parse("name: oyl\nversion: 0.1.0")
  # data["name"].as_s # => "oyl"
  # ```
  def self.parse(data : String | IO, *, dialect : Dialect = :yaml12) : Oyl::Any
    doc = Oyl::Nodes.parse(data, dialect: dialect, merge: true)
    ctx = Oyl::ParseContext.new(doc.dialect)
    if doc.nodes.empty?
      Oyl::Any.new(nil)
    else
      Oyl::Any.new(ctx, doc.nodes.first)
    end
  end

  # Parses all YAML documents and returns an `Array(Oyl::Any)`.
  #
  # ```
  # docs = Oyl.parse_all("---\nfoo\n---\nbar")
  # docs.size # => 2
  # ```
  def self.parse_all(data : String | IO, *, dialect : Dialect = :yaml12) : Array(Oyl::Any)
    documents = Oyl::Nodes.parse_all(data, dialect: dialect, merge: true)
    documents.map do |doc|
      ctx = Oyl::ParseContext.new(doc.dialect)
      if doc.nodes.empty?
        Oyl::Any.new(nil)
      else
        Oyl::Any.new(ctx, doc.nodes.first)
      end
    end
  end
end
