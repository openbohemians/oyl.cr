require "./spec_helper"

describe Oyl::PullParser do
  it "parses a simple scalar" do
    parser = Oyl::PullParser.new("hello")
    parser.kind.should eq(Oyl::EventKind::STREAM_START)
    parser.read_next
    parser.kind.should eq(Oyl::EventKind::DOCUMENT_START)
    parser.read_next
    parser.kind.should eq(Oyl::EventKind::SCALAR)
    parser.value.should eq("hello")
    parser.read_next
    parser.kind.should eq(Oyl::EventKind::DOCUMENT_END)
    parser.read_next
    parser.kind.should eq(Oyl::EventKind::STREAM_END)
    parser.close
  end

  it "parses a mapping" do
    yaml = "name: oyl\nversion: 0.1.0"
    Oyl::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.read_mapping do
            parser.read_scalar.should eq("name")
            parser.read_scalar.should eq("oyl")
            parser.read_scalar.should eq("version")
            parser.read_scalar.should eq("0.1.0")
          end
        end
      end
    end
  end

  it "parses a sequence" do
    yaml = "- one\n- two\n- three"
    Oyl::PullParser.new(yaml) do |parser|
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
    Oyl::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.read_mapping do
            parser.read_scalar # "a"
            parser.kind.should eq(Oyl::EventKind::SCALAR)
            parser.anchor.should eq("anchor")
            parser.value.should eq("hello")
            parser.read_next
            parser.read_scalar # "b"
            parser.kind.should eq(Oyl::EventKind::ALIAS)
            parser.read_next # consume the alias
          end
        end
      end
    end
  end

  it "reports scalar styles" do
    yaml = %("double quoted")
    Oyl::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.scalar_style.should eq(Oyl::ScalarStyle::DOUBLE_QUOTED)
          parser.read_scalar.should eq("double quoted")
        end
      end
    end
  end

  it "reports flow/block collection styles" do
    yaml = "[1, 2, 3]"
    Oyl::PullParser.new(yaml) do |parser|
      parser.read_stream do
        parser.read_document do
          parser.sequence_style.should eq(Oyl::SequenceStyle::FLOW)
          parser.skip
        end
      end
    end
  end

  it "skips over complex structures" do
    yaml = "a:\n  b:\n    - 1\n    - 2\nc: 3"
    Oyl::PullParser.new(yaml) do |parser|
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
    expect_raises(Oyl::ParseException) do
      Oyl::PullParser.new("- :\n[bad") do |parser|
        while !parser.kind.stream_end?
          parser.read_next
        end
      end
    end
  end

  it "parses empty input" do
    Oyl::PullParser.new("") do |parser|
      parser.kind.should eq(Oyl::EventKind::STREAM_START)
      parser.read_next
      parser.kind.should eq(Oyl::EventKind::STREAM_END)
    end
  end

  # "[a, b, c, d, e, f]" is 12 events: stream, document, sequence, 6 scalars
  it "stops at the event limit" do
    expect_raises(Oyl::ParseException, /event limit/) do
      Oyl::PullParser.new("[a, b, c, d, e, f]", max_events: 5) do |parser|
        while !parser.kind.stream_end?
          parser.read_next
        end
      end
    end
  end

  it "parses within the event limit, or with it off" do
    [12, 0].each do |max|
      Oyl::PullParser.new("[a, b, c, d, e, f]", max_events: max) do |parser|
        while !parser.kind.stream_end?
          parser.read_next
        end
      end
    end
  end

  it "stops at the depth limit" do
    expect_raises(Oyl::ParseException, /depth/) do
      Oyl::PullParser.new("[[[[x]]]]", max_depth: 3) do |parser|
        while !parser.kind.stream_end?
          parser.read_next
        end
      end
    end
    Oyl::PullParser.new("[[[[x]]]]", max_depth: 4) do |parser|
      while !parser.kind.stream_end?
        parser.read_next
      end
    end
  end
end
