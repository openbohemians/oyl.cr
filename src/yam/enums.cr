module Yam
  enum EventKind
    NONE
    STREAM_START
    STREAM_END
    DOCUMENT_START
    DOCUMENT_END
    MAPPING_START
    MAPPING_END
    SEQUENCE_START
    SEQUENCE_END
    SCALAR
    ALIAS
  end

  enum ScalarStyle
    ANY
    PLAIN
    SINGLE_QUOTED
    DOUBLE_QUOTED
    LITERAL
    FOLDED
  end

  enum SequenceStyle
    ANY
    BLOCK
    FLOW
  end

  enum MappingStyle
    ANY
    BLOCK
    FLOW
  end
end
