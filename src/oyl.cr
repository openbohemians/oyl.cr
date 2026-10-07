require "./oyl/lib_oyl"
require "./oyl/enums"
require "./oyl/error"
require "./oyl/pull_parser"
require "./oyl/nodes"
require "./oyl/parse_context"
require "./oyl/schema/core"
require "./oyl/any"

module Oyl
  VERSION = "0.1.0"

  # Parses a YAML document and returns a `Oyl::Any`.
  #
  # ```
  # data = Oyl.parse("name: oyl\nversion: 0.1.0")
  # data["name"].as_s # => "oyl"
  # ```
  def self.parse(data : String | IO) : Oyl::Any
    doc = Oyl::Nodes.parse(data)
    ctx = Oyl::ParseContext.new
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
  def self.parse_all(data : String | IO) : Array(Oyl::Any)
    documents = Oyl::Nodes.parse_all(data)
    documents.map do |doc|
      ctx = Oyl::ParseContext.new
      if doc.nodes.empty?
        Oyl::Any.new(nil)
      else
        Oyl::Any.new(ctx, doc.nodes.first)
      end
    end
  end
end
