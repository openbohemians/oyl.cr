require "./spec_helper"

describe Yam::Any do
  describe "scalar resolution" do
    it "parses null values" do
      Yam.parse("~").raw.should be_nil
      Yam.parse("null").raw.should be_nil
      Yam.parse("Null").raw.should be_nil
      Yam.parse("NULL").raw.should be_nil
      Yam.parse("").raw.should be_nil
    end

    it "parses boolean values" do
      Yam.parse("true").as_bool.should be_true
      Yam.parse("True").as_bool.should be_true
      Yam.parse("TRUE").as_bool.should be_true
      Yam.parse("yes").as_bool.should be_true
      Yam.parse("on").as_bool.should be_true
      Yam.parse("false").as_bool.should be_false
      Yam.parse("False").as_bool.should be_false
      Yam.parse("no").as_bool.should be_false
      Yam.parse("off").as_bool.should be_false
    end

    it "parses integer values" do
      Yam.parse("42").as_i.should eq(42)
      Yam.parse("-7").as_i64.should eq(-7_i64)
      Yam.parse("0").as_i64.should eq(0_i64)
      Yam.parse("0x1A").as_i64.should eq(26_i64)
      Yam.parse("0o17").as_i64.should eq(15_i64)
    end

    it "parses float values" do
      Yam.parse("3.14").as_f.should eq(3.14)
      Yam.parse("-1.5").as_f.should eq(-1.5)
      Yam.parse(".inf").as_f.should eq(Float64::INFINITY)
      Yam.parse("-.inf").as_f.should eq(-Float64::INFINITY)
      Yam.parse(".nan").as_f.nan?.should be_true
    end

    it "preserves strings when quoted" do
      Yam.parse(%("true")).as_s.should eq("true")
      Yam.parse(%("42")).as_s.should eq("42")
      Yam.parse(%("null")).as_s.should eq("null")
    end

    it "parses plain strings" do
      Yam.parse("hello").as_s.should eq("hello")
      Yam.parse("hello world").as_s.should eq("hello world")
    end
  end

  describe "collections" do
    it "parses arrays" do
      data = Yam.parse("- 1\n- 2\n- 3")
      data.as_a.size.should eq(3)
      data[0].as_i.should eq(1)
      data[1].as_i.should eq(2)
      data[2].as_i.should eq(3)
    end

    it "parses hashes" do
      data = Yam.parse("name: yam\nversion: 0.1.0")
      data["name"].as_s.should eq("yam")
      data["version"].as_s.should eq("0.1.0")
    end

    it "parses nested structures" do
      yaml = "server:\n  host: localhost\n  port: 8080"
      data = Yam.parse(yaml)
      data["server"]["host"].as_s.should eq("localhost")
      data["server"]["port"].as_i.should eq(8080)
    end

    it "parses mixed types in sequences" do
      data = Yam.parse("- hello\n- 42\n- true\n- 3.14")
      data[0].as_s.should eq("hello")
      data[1].as_i.should eq(42)
      data[2].as_bool.should be_true
      data[3].as_f.should eq(3.14)
    end
  end

  describe "#[]?" do
    it "returns nil for missing keys" do
      data = Yam.parse("a: 1")
      data["b"]?.should be_nil
    end

    it "returns nil for out of range index" do
      data = Yam.parse("- 1")
      data[5]?.should be_nil
    end
  end

  describe "#dig" do
    it "traverses nested structures" do
      yaml = "a:\n  b:\n    c: deep"
      Yam.parse(yaml).dig("a", "b", "c").as_s.should eq("deep")
    end

    it "raises on missing key" do
      expect_raises(Exception) do
        Yam.parse("a: 1").dig("b", "c")
      end
    end
  end

  describe "#dig?" do
    it "returns nil for missing path" do
      Yam.parse("a: 1").dig?("b", "c").should be_nil
    end
  end

  describe "#size" do
    it "returns array size" do
      Yam.parse("- 1\n- 2\n- 3").size.should eq(3)
    end

    it "returns hash size" do
      Yam.parse("a: 1\nb: 2").size.should eq(2)
    end
  end

  describe "anchors and aliases" do
    it "resolves aliases" do
      yaml = "defaults: &defaults\n  host: localhost\n  port: 8080\nproduction:\n  <<: *defaults\n  port: 80"
      data = Yam.parse(yaml)
      data["production"]["host"].as_s.should eq("localhost")
      data["production"]["port"].as_i.should eq(80)
    end

    it "resolves scalar aliases" do
      yaml = "a: &val hello\nb: *val"
      data = Yam.parse(yaml)
      data["a"].as_s.should eq("hello")
      data["b"].as_s.should eq("hello")
    end
  end

  describe "multi-document" do
    it "parses all documents" do
      yaml = "---\nfoo\n---\nbar"
      docs = Yam.parse_all(yaml)
      docs.size.should eq(2)
      docs[0].as_s.should eq("foo")
      docs[1].as_s.should eq("bar")
    end
  end

  describe "empty input" do
    it "returns nil for empty document" do
      Yam.parse("").raw.should be_nil
    end
  end

  describe "errors" do
    it "raises ParseException on invalid YAML" do
      expect_raises(Yam::ParseException) do
        Yam.parse("- :\n[bad")
      end
    end
  end

  describe "equality" do
    it "compares raw values extracted from Any" do
      Yam.parse("42").as_i.should eq(42)
      Yam.parse("hello").as_s.should eq("hello")
    end

    it "compares two Any values" do
      Yam.parse("42").should eq(Yam::Any.new(42_i64))
      Yam.parse("hello").should eq(Yam::Any.new("hello"))
    end
  end

  describe "#to_s" do
    it "converts to string" do
      Yam.parse("hello").to_s.should eq("hello")
      Yam.parse("42").to_s.should eq("42")
    end
  end
end
