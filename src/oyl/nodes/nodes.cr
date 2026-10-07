module Oyl::Nodes
  abstract class Node
    property anchor : String?
    property tag : String?
    property start_line : Int32 = 0
    property start_column : Int32 = 0
    property end_line : Int32 = 0
    property end_column : Int32 = 0
  end

  class Scalar < Node
    property value : String
    property style : Oyl::ScalarStyle

    def initialize(@value : String = "", @style : Oyl::ScalarStyle = Oyl::ScalarStyle::ANY)
    end
  end

  class Sequence < Node
    property nodes : Array(Node)
    property style : Oyl::SequenceStyle

    def initialize(@style : Oyl::SequenceStyle = Oyl::SequenceStyle::ANY)
      @nodes = [] of Node
    end

    delegate :<<, :each, :size, to: @nodes

    def [](index : Int) : Node
      @nodes[index]
    end
  end

  class Mapping < Node
    property nodes : Array(Node)
    property style : Oyl::MappingStyle

    def initialize(@style : Oyl::MappingStyle = Oyl::MappingStyle::ANY)
      @nodes = [] of Node
    end

    delegate :<<, :size, to: @nodes

    def each_pair(&)
      i = 0
      while i < @nodes.size - 1
        yield @nodes[i], @nodes[i + 1]
        i += 2
      end
    end
  end

  class Alias < Node
    property value : String
    property resolved : Node?

    def initialize(@value : String)
    end
  end

  class Document < Node
    property nodes : Array(Node)

    def initialize
      @nodes = [] of Node
    end

    delegate :<<, :each, :size, to: @nodes

    def [](index : Int) : Node
      @nodes[index]
    end
  end
end
