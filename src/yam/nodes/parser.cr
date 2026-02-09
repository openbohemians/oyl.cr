class Yam::Nodes::Parser
  @anchors = {} of String => Yam::Nodes::Node

  def initialize(@pull_parser : Yam::PullParser)
  end

  def self.new(content : String | IO, &)
    Yam::PullParser.new(content) do |pull|
      parser = new(pull)
      yield parser
    end
  end

  def parse : Yam::Nodes::Document
    documents = parse_all
    documents.first? || Yam::Nodes::Document.new
  end

  def parse_all : Array(Yam::Nodes::Document)
    documents = [] of Yam::Nodes::Document

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

  private def parse_document : Yam::Nodes::Document
    doc = Yam::Nodes::Document.new
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

  private def parse_node : Yam::Nodes::Node
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
      raise Yam::Error.new("Unexpected event: #{@pull_parser.kind}")
    end
  end

  private def parse_scalar : Yam::Nodes::Scalar
    node = Yam::Nodes::Scalar.new(@pull_parser.value, @pull_parser.scalar_style)
    apply_properties(node)
    @pull_parser.read_next
    node
  end

  private def parse_sequence : Yam::Nodes::Sequence
    node = Yam::Nodes::Sequence.new(@pull_parser.sequence_style)
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

  private def parse_mapping : Yam::Nodes::Mapping
    node = Yam::Nodes::Mapping.new(@pull_parser.mapping_style)
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

  private def parse_alias : Yam::Nodes::Alias
    value = @pull_parser.value
    node = Yam::Nodes::Alias.new(value)
    node.resolved = @anchors[value]?
    node.start_line = @pull_parser.start_line
    node.start_column = @pull_parser.start_column
    @pull_parser.read_next
    node
  end

  private def apply_properties(node : Yam::Nodes::Node)
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
