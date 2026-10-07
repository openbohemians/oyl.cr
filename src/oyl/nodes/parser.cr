class Oyl::Nodes::Parser
  @anchors = {} of String => Oyl::Nodes::Node

  def initialize(@pull_parser : Oyl::PullParser)
  end

  def self.new(content : String | IO, &)
    Oyl::PullParser.new(content) do |pull|
      parser = new(pull)
      yield parser
    end
  end

  def parse : Oyl::Nodes::Document
    documents = parse_all
    documents.first? || Oyl::Nodes::Document.new
  end

  def parse_all : Array(Oyl::Nodes::Document)
    documents = [] of Oyl::Nodes::Document

    @pull_parser.read_stream do
      loop do
        case @pull_parser.kind
        when .document_start?
          documents << parse_document
        when .stream_end?
          break
        else
          @pull_parser.read_next
        end
      end
    end

    documents
  end

  private def parse_document : Oyl::Nodes::Document
    doc = Oyl::Nodes::Document.new
    doc.start_line = @pull_parser.start_line
    doc.start_column = @pull_parser.start_column

    @pull_parser.read_next # consume DOCUMENT_START

    loop do
      case @pull_parser.kind
      when .document_end?
        doc.end_line = @pull_parser.start_line
        doc.end_column = @pull_parser.start_column
        @pull_parser.read_next # consume DOCUMENT_END
        break
      else
        doc << parse_node
      end
    end

    doc
  end

  private def parse_node : Oyl::Nodes::Node
    case @pull_parser.kind
    when .scalar?
      parse_scalar
    when .sequence_start?
      parse_sequence
    when .mapping_start?
      parse_mapping
    when .alias?
      parse_alias
    else
      raise Oyl::Error.new("Unexpected event: #{@pull_parser.kind}")
    end
  end

  private def parse_scalar : Oyl::Nodes::Scalar
    node = Oyl::Nodes::Scalar.new(@pull_parser.value, @pull_parser.scalar_style)
    apply_properties(node)
    @pull_parser.read_next
    node
  end

  private def parse_sequence : Oyl::Nodes::Sequence
    node = Oyl::Nodes::Sequence.new(@pull_parser.sequence_style)
    apply_properties(node)
    @pull_parser.read_next # consume SEQUENCE_START

    until @pull_parser.kind.sequence_end?
      node << parse_node
    end

    node.end_line = @pull_parser.start_line
    node.end_column = @pull_parser.start_column
    @pull_parser.read_next # consume SEQUENCE_END
    node
  end

  private def parse_mapping : Oyl::Nodes::Mapping
    node = Oyl::Nodes::Mapping.new(@pull_parser.mapping_style)
    apply_properties(node)
    @pull_parser.read_next # consume MAPPING_START

    until @pull_parser.kind.mapping_end?
      node << parse_node # key
      node << parse_node # value
    end

    node.end_line = @pull_parser.start_line
    node.end_column = @pull_parser.start_column
    @pull_parser.read_next # consume MAPPING_END
    node
  end

  private def parse_alias : Oyl::Nodes::Alias
    value = @pull_parser.value
    node = Oyl::Nodes::Alias.new(value)
    node.resolved = @anchors[value]?
    node.start_line = @pull_parser.start_line
    node.start_column = @pull_parser.start_column
    @pull_parser.read_next
    node
  end

  private def apply_properties(node : Oyl::Nodes::Node)
    anchor = @pull_parser.anchor
    node.anchor = anchor
    node.tag = @pull_parser.tag
    node.start_line = @pull_parser.start_line
    node.start_column = @pull_parser.start_column
    if anchor
      @anchors[anchor] = node
    end
  end
end
