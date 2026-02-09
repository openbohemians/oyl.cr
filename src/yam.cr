require "./yam/lib_yam"
require "./yam/enums"
require "./yam/error"
require "./yam/pull_parser"
require "./yam/nodes"
require "./yam/parse_context"
require "./yam/schema/core"
require "./yam/any"

module Yam
  VERSION = "0.1.0"

  # Parses a YAML document and returns a `Yam::Any`.
  #
  # ```
  # data = Yam.parse("name: yam\nversion: 0.1.0")
  # data["name"].as_s # => "yam"
  # ```
  def self.parse(data : String | IO) : Yam::Any
    doc = Yam::Nodes.parse(data)
    ctx = Yam::ParseContext.new
    if doc.nodes.empty?
      Yam::Any.new(nil)
    else
      Yam::Any.new(ctx, doc.nodes.first)
    end
  end

  # Parses all YAML documents and returns an `Array(Yam::Any)`.
  #
  # ```
  # docs = Yam.parse_all("---\nfoo\n---\nbar")
  # docs.size # => 2
  # ```
  def self.parse_all(data : String | IO) : Array(Yam::Any)
    documents = Yam::Nodes.parse_all(data)
    documents.map do |doc|
      ctx = Yam::ParseContext.new
      if doc.nodes.empty?
        Yam::Any.new(nil)
      else
        Yam::Any.new(ctx, doc.nodes.first)
      end
    end
  end
end
