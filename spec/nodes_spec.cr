require "./spec_helper"

describe Yam::Nodes do
  describe ".parse" do
    it "parses a scalar document" do
      doc = Yam::Nodes.parse("hello")
      doc.size.should eq(1)
      node = doc[0].as(Yam::Nodes::Scalar)
      node.value.should eq("hello")
      node.style.should eq(Yam::ScalarStyle::PLAIN)
    end

    it "parses a quoted scalar" do
      doc = Yam::Nodes.parse(%("hello world"))
      node = doc[0].as(Yam::Nodes::Scalar)
      node.value.should eq("hello world")
      node.style.should eq(Yam::ScalarStyle::DOUBLE_QUOTED)
    end

    it "parses a sequence" do
      doc = Yam::Nodes.parse("- one\n- two\n- three")
      seq = doc[0].as(Yam::Nodes::Sequence)
      seq.size.should eq(3)
      seq.style.should eq(Yam::SequenceStyle::BLOCK)
      seq[0].as(Yam::Nodes::Scalar).value.should eq("one")
      seq[1].as(Yam::Nodes::Scalar).value.should eq("two")
      seq[2].as(Yam::Nodes::Scalar).value.should eq("three")
    end

    it "parses a mapping" do
      doc = Yam::Nodes.parse("name: yam\nversion: 0.1.0")
      map = doc[0].as(Yam::Nodes::Mapping)
      map.size.should eq(4) # flat: key, val, key, val
      map.nodes[0].as(Yam::Nodes::Scalar).value.should eq("name")
      map.nodes[1].as(Yam::Nodes::Scalar).value.should eq("yam")
      map.nodes[2].as(Yam::Nodes::Scalar).value.should eq("version")
      map.nodes[3].as(Yam::Nodes::Scalar).value.should eq("0.1.0")
    end

    it "parses anchors and aliases" do
      yaml = "a: &ref hello\nb: *ref"
      doc = Yam::Nodes.parse(yaml)
      map = doc[0].as(Yam::Nodes::Mapping)

      val = map.nodes[1].as(Yam::Nodes::Scalar)
      val.value.should eq("hello")
      val.anchor.should eq("ref")

      alias_node = map.nodes[3].as(Yam::Nodes::Alias)
      alias_node.value.should eq("ref")
      alias_node.resolved.should eq(val)
    end

    it "parses nested structures" do
      yaml = "items:\n  - name: a\n    value: 1\n  - name: b\n    value: 2"
      doc = Yam::Nodes.parse(yaml)
      map = doc[0].as(Yam::Nodes::Mapping)
      seq = map.nodes[1].as(Yam::Nodes::Sequence)
      seq.size.should eq(2)
      inner = seq[0].as(Yam::Nodes::Mapping)
      inner.nodes[0].as(Yam::Nodes::Scalar).value.should eq("name")
      inner.nodes[1].as(Yam::Nodes::Scalar).value.should eq("a")
    end

    it "has correct position marks for scalars" do
      yaml = "key: value"
      doc = Yam::Nodes.parse(yaml)
      map = doc[0].as(Yam::Nodes::Mapping)
      key = map.nodes[0].as(Yam::Nodes::Scalar)
      key.start_line.should eq(1)
      key.start_column.should eq(1)
      val = map.nodes[1].as(Yam::Nodes::Scalar)
      val.start_line.should eq(1)
      val.start_column.should eq(6)
    end

    it "has correct position marks for sequences" do
      doc = Yam::Nodes.parse("- a\n- b")
      seq = doc[0].as(Yam::Nodes::Sequence)
      seq.start_line.should eq(1)
      seq.start_column.should eq(1)
      seq[0].as(Yam::Nodes::Scalar).start_line.should eq(1)
      seq[0].as(Yam::Nodes::Scalar).start_column.should eq(3)
      seq[1].as(Yam::Nodes::Scalar).start_line.should eq(2)
      seq[1].as(Yam::Nodes::Scalar).start_column.should eq(3)
    end

    it "has correct position marks for multiline mappings" do
      doc = Yam::Nodes.parse("a: 1\nb: 2")
      map = doc[0].as(Yam::Nodes::Mapping)
      map.start_line.should eq(1)
      map.start_column.should eq(1)
      map.nodes[2].as(Yam::Nodes::Scalar).start_line.should eq(2)
      map.nodes[2].as(Yam::Nodes::Scalar).start_column.should eq(1)
      map.nodes[3].as(Yam::Nodes::Scalar).start_line.should eq(2)
      map.nodes[3].as(Yam::Nodes::Scalar).start_column.should eq(4)
    end

    it "has correct position marks for flow collections" do
      doc = Yam::Nodes.parse("[a, b]")
      seq = doc[0].as(Yam::Nodes::Sequence)
      seq.start_line.should eq(1)
      seq.start_column.should eq(1)
      seq.end_line.should eq(1)
      seq.end_column.should eq(6)
    end

    it "has correct position marks with anchors" do
      doc = Yam::Nodes.parse("&ref hello")
      node = doc[0].as(Yam::Nodes::Scalar)
      node.start_line.should eq(1)
      node.start_column.should eq(1)
    end

    it "parses flow collections" do
      doc = Yam::Nodes.parse("[1, 2, 3]")
      seq = doc[0].as(Yam::Nodes::Sequence)
      seq.style.should eq(Yam::SequenceStyle::FLOW)
      seq.size.should eq(3)
    end
  end

  describe ".parse_all" do
    it "parses multiple documents" do
      yaml = "---\nfoo\n---\nbar"
      docs = Yam::Nodes.parse_all(yaml)
      docs.size.should eq(2)
      docs[0][0].as(Yam::Nodes::Scalar).value.should eq("foo")
      docs[1][0].as(Yam::Nodes::Scalar).value.should eq("bar")
    end
  end

  describe Yam::Nodes::Mapping do
    it "iterates key/value pairs" do
      doc = Yam::Nodes.parse("a: 1\nb: 2")
      map = doc[0].as(Yam::Nodes::Mapping)
      pairs = [] of {String, String}
      map.each_pair do |k, v|
        pairs << {k.as(Yam::Nodes::Scalar).value, v.as(Yam::Nodes::Scalar).value}
      end
      pairs.should eq([{"a", "1"}, {"b", "2"}])
    end
  end
end
