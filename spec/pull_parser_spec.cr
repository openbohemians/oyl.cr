require "./spec_helper"

describe Yam::PullParser do
  it "parses a simple scalar" do
    parser = Yam::PullParser.new("hello")
    parser.kind.should eq(Yam::EventKind::STREAM_START)
    parser.read_next
    parser.kind.should eq(Yam::EventKind::DOCUMENT_START)
    parser.read_next
    parser.kind.should eq(Yam::EventKind::SCALAR)
    parser.value.should eq("hello")
    parser.read_next
    parser.kind.should eq(Yam::EventKind::DOCUMENT_END)
    parser.read_next
    parser.kind.should eq(Yam::EventKind::STREAM_END)
    parser.close
  end

  it "parses a mapping" do
    yaml = "name: yam\nversion: 0.1.0"
    Yam::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.read_mapping do
            parser.read_scalar.should eq("name")
            parser.read_scalar.should eq("yam")
            parser.read_scalar.should eq("version")
            parser.read_scalar.should eq("0.1.0")
          end
        end
      end
    end
  end

  it "parses a sequence" do
    yaml = "- one\n- two\n- three"
    Yam::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.read_sequence do
            parser.read_scalar.should eq("one")
            parser.read_scalar.should eq("two")
            parser.read_scalar.should eq("three")
          end
        end
      end
    end
  end

  it "reports anchors and aliases" do
    yaml = "a: &anchor hello\nb: *anchor"
    Yam::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.read_mapping do
            parser.read_scalar # "a"
            parser.kind.should eq(Yam::EventKind::SCALAR)
            parser.anchor.should eq("anchor")
            parser.value.should eq("hello")
            parser.read_next
            parser.read_scalar # "b"
            parser.kind.should eq(Yam::EventKind::ALIAS)
            parser.read_next # consume the alias
          end
        end
      end
    end
  end

  it "reports scalar styles" do
    yaml = %("double quoted")
    Yam::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.scalar_style.should eq(Yam::ScalarStyle::DOUBLE_QUOTED)
          parser.read_scalar.should eq("double quoted")
        end
      end
    end
  end

  it "reports flow/block collection styles" do
    yaml = "[1, 2, 3]"
    Yam::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.sequence_style.should eq(Yam::SequenceStyle::FLOW)
          parser.skip
        end
      end
    end
  end

  it "skips over complex structures" do
    yaml = "a:\n  b:\n    - 1\n    - 2\nc: 3"
    Yam::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.read_mapping do
            parser.read_scalar.should eq("a")
            parser.skip # skip the nested mapping
            parser.read_scalar.should eq("c")
            parser.read_scalar.should eq("3")
          end
        end
      end
    end
  end

  it "raises on parse errors" do
    expect_raises(Yam::ParseException) do
      Yam::PullParser.new("- :\n[bad") do |parser|
        while !parser.kind.stream_end?
          parser.read_next
        end
      end
    end
  end

  it "parses empty input" do
    Yam::PullParser.new("") do |parser|
      parser.kind.should eq(Yam::EventKind::STREAM_START)
      parser.read_next
      parser.kind.should eq(Yam::EventKind::STREAM_END)
    end
  end
end
