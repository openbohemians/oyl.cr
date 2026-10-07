require "./spec_helper"

describe Oyl::Nodes do
  describe ".parse" do
    it "parses a scalar document" do
      doc = Oyl::Nodes.parse("hello")
      doc.size.should eq(1)
      node = doc[0].as(Oyl::Nodes::Scalar)
      node.value.should eq("hello")
      node.style.should eq(Oyl::ScalarStyle::PLAIN)
    end

    it "parses a quoted scalar" do
      doc = Oyl::Nodes.parse(%("hello world"))
      node = doc[0].as(Oyl::Nodes::Scalar)
      node.value.should eq("hello world")
      node.style.should eq(Oyl::ScalarStyle::DOUBLE_QUOTED)
    end

    it "parses a sequence" do
      doc = Oyl::Nodes.parse("- one\n- two\n- three")
      seq = doc[0].as(Oyl::Nodes::Sequence)
      seq.size.should eq(3)
      seq.style.should eq(Oyl::SequenceStyle::BLOCK)
      seq[0].as(Oyl::Nodes::Scalar).value.should eq("one")
      seq[1].as(Oyl::Nodes::Scalar).value.should eq("two")
      seq[2].as(Oyl::Nodes::Scalar).value.should eq("three")
    end

    it "parses a mapping" do
      doc = Oyl::Nodes.parse("name: oyl\nversion: 0.1.0")
      map = doc[0].as(Oyl::Nodes::Mapping)
      map.size.should eq(4) # flat: key, val, key, val
      map.nodes[0].as(Oyl::Nodes::Scalar).value.should eq("name")
      map.nodes[1].as(Oyl::Nodes::Scalar).value.should eq("oyl")
      map.nodes[2].as(Oyl::Nodes::Scalar).value.should eq("version")
      map.nodes[3].as(Oyl::Nodes::Scalar).value.should eq("0.1.0")
    end

    it "parses anchors and aliases" do
      yaml = "a: &ref hello\nb: *ref"
      doc = Oyl::Nodes.parse(yaml)
      map = doc[0].as(Oyl::Nodes::Mapping)

      val = map.nodes[1].as(Oyl::Nodes::Scalar)
      val.value.should eq("hello")
      val.anchor.should eq("ref")

      alias_node = map.nodes[3].as(Oyl::Nodes::Alias)
      alias_node.value.should eq("ref")
      alias_node.resolved.should eq(val)
    end

    it "parses nested structures" do
      yaml = "items:\n  - name: a\n    value: 1\n  - name: b\n    value: 2"
      doc = Oyl::Nodes.parse(yaml)
      map = doc[0].as(Oyl::Nodes::Mapping)
      seq = map.nodes[1].as(Oyl::Nodes::Sequence)
      seq.size.should eq(2)
      inner = seq[0].as(Oyl::Nodes::Mapping)
      inner.nodes[0].as(Oyl::Nodes::Scalar).value.should eq("name")
      inner.nodes[1].as(Oyl::Nodes::Scalar).value.should eq("a")
    end

    it "has correct position marks for scalars" do
      yaml = "key: value"
      doc = Oyl::Nodes.parse(yaml)
      map = doc[0].as(Oyl::Nodes::Mapping)
      key = map.nodes[0].as(Oyl::Nodes::Scalar)
      key.start_line.should eq(1)
      key.start_column.should eq(1)
      val = map.nodes[1].as(Oyl::Nodes::Scalar)
      val.start_line.should eq(1)
      val.start_column.should eq(6)
    end

    it "has correct position marks for sequences" do
      doc = Oyl::Nodes.parse("- a\n- b")
      seq = doc[0].as(Oyl::Nodes::Sequence)
      seq.start_line.should eq(1)
      seq.start_column.should eq(1)
      seq[0].as(Oyl::Nodes::Scalar).start_line.should eq(1)
      seq[0].as(Oyl::Nodes::Scalar).start_column.should eq(3)
      seq[1].as(Oyl::Nodes::Scalar).start_line.should eq(2)
      seq[1].as(Oyl::Nodes::Scalar).start_column.should eq(3)
    end

    it "has correct position marks for multiline mappings" do
      doc = Oyl::Nodes.parse("a: 1\nb: 2")
      map = doc[0].as(Oyl::Nodes::Mapping)
      map.start_line.should eq(1)
      map.start_column.should eq(1)
      map.nodes[2].as(Oyl::Nodes::Scalar).start_line.should eq(2)
      map.nodes[2].as(Oyl::Nodes::Scalar).start_column.should eq(1)
      map.nodes[3].as(Oyl::Nodes::Scalar).start_line.should eq(2)
      map.nodes[3].as(Oyl::Nodes::Scalar).start_column.should eq(4)
    end

    it "has correct position marks for flow collections" do
      doc = Oyl::Nodes.parse("[a, b]")
      seq = doc[0].as(Oyl::Nodes::Sequence)
      seq.start_line.should eq(1)
      seq.start_column.should eq(1)
      seq.end_line.should eq(1)
      seq.end_column.should eq(6)
    end

    it "has correct position marks with anchors" do
      doc = Oyl::Nodes.parse("&ref hello")
      node = doc[0].as(Oyl::Nodes::Scalar)
      node.start_line.should eq(1)
      node.start_column.should eq(1)
    end

    it "parses flow collections" do
      doc = Oyl::Nodes.parse("[1, 2, 3]")
      seq = doc[0].as(Oyl::Nodes::Sequence)
      seq.style.should eq(Oyl::SequenceStyle::FLOW)
      seq.size.should eq(3)
    end
  end

  describe ".parse_all" do
    it "parses multiple documents" do
      yaml = "---\nfoo\n---\nbar"
      docs = Oyl::Nodes.parse_all(yaml)
      docs.size.should eq(2)
      docs[0][0].as(Oyl::Nodes::Scalar).value.should eq("foo")
      docs[1][0].as(Oyl::Nodes::Scalar).value.should eq("bar")
    end
  end

  describe Oyl::Nodes::Mapping do
    it "iterates key/value pairs" do
      doc = Oyl::Nodes.parse("a: 1\nb: 2")
      map = doc[0].as(Oyl::Nodes::Mapping)
      pairs = [] of {String, String}
      map.each_pair do |k, v|
        pairs << {k.as(Oyl::Nodes::Scalar).value, v.as(Oyl::Nodes::Scalar).value}
      end
      pairs.should eq([{"a", "1"}, {"b", "2"}])
    end
  end
end
