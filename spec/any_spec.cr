require "./spec_helper"

describe Oyl::Any do
  describe "scalar resolution" do
    it "parses null values" do
      Oyl.parse("~").raw.should be_nil
      Oyl.parse("null").raw.should be_nil
      Oyl.parse("Null").raw.should be_nil
      Oyl.parse("NULL").raw.should be_nil
      Oyl.parse("").raw.should be_nil
    end

    it "parses boolean values" do
      Oyl.parse("true").as_bool.should be_true
      Oyl.parse("True").as_bool.should be_true
      Oyl.parse("TRUE").as_bool.should be_true
      Oyl.parse("false").as_bool.should be_false
      Oyl.parse("False").as_bool.should be_false
      Oyl.parse("FALSE").as_bool.should be_false
    end

    it "reads YAML 1.1's booleans as strings" do
      %w(yes no on off Yes NO y n).each do |word|
        Oyl.parse(word).as_s.should eq(word)
      end
      Oyl.parse("on: push")["on"].as_s.should eq("push")
    end

    it "parses integer values" do
      Oyl.parse("42").as_i.should eq(42)
      Oyl.parse("-7").as_i64.should eq(-7_i64)
      Oyl.parse("0").as_i64.should eq(0_i64)
      Oyl.parse("0x1A").as_i64.should eq(26_i64)
      Oyl.parse("0o17").as_i64.should eq(15_i64)
      Oyl.parse("017").as_i64.should eq(17_i64)
    end

    it "parses integers above Int64::MAX" do
      Oyl.parse("18446744073709551615").as_u64.should eq(UInt64::MAX)
    end

    it "reads dates, base 60 and underscores as strings" do
      Oyl.parse("2001-12-14").as_s.should eq("2001-12-14")
      Oyl.parse("1:30").as_s.should eq("1:30")
      Oyl.parse("1_000").as_s.should eq("1_000")
    end

    it "reads tagged scalars by their tag" do
      Oyl.parse("!!str 42").as_s.should eq("42")
      Oyl.parse("!!float 1").as_f.should eq(1.0)
      Oyl.parse("!!binary aGVsbG8=").as_bytes.should eq("hello".to_slice)
      Oyl.parse("!!timestamp 2001-12-14").as_time.should eq(Time.utc(2001, 12, 14))
      Oyl.parse("!foo 42").as_s.should eq("42")
      Oyl.parse("! 42").as_s.should eq("42")
      expect_raises(Oyl::ParseException, "Invalid int") { Oyl.parse("!!int abc") }
    end

    it "parses float values" do
      Oyl.parse("3.14").as_f.should eq(3.14)
      Oyl.parse("-1.5").as_f.should eq(-1.5)
      Oyl.parse(".inf").as_f.should eq(Float64::INFINITY)
      Oyl.parse("-.inf").as_f.should eq(-Float64::INFINITY)
      Oyl.parse(".nan").as_f.nan?.should be_true
    end

    it "preserves strings when quoted" do
      Oyl.parse(%("true")).as_s.should eq("true")
      Oyl.parse(%("42")).as_s.should eq("42")
      Oyl.parse(%("null")).as_s.should eq("null")
    end

    it "parses plain strings" do
      Oyl.parse("hello").as_s.should eq("hello")
      Oyl.parse("hello world").as_s.should eq("hello world")
    end
  end

  describe "collections" do
    it "parses arrays" do
      data = Oyl.parse("- 1\n- 2\n- 3")
      data.as_a.size.should eq(3)
      data[0].as_i.should eq(1)
      data[1].as_i.should eq(2)
      data[2].as_i.should eq(3)
    end

    it "parses hashes" do
      data = Oyl.parse("name: oyl\nversion: 0.1.0")
      data["name"].as_s.should eq("oyl")
      data["version"].as_s.should eq("0.1.0")
    end

    it "parses nested structures" do
      yaml = "server:\n  host: localhost\n  port: 8080"
      data = Oyl.parse(yaml)
      data["server"]["host"].as_s.should eq("localhost")
      data["server"]["port"].as_i.should eq(8080)
    end

    it "parses mixed types in sequences" do
      data = Oyl.parse("- hello\n- 42\n- true\n- 3.14")
      data[0].as_s.should eq("hello")
      data[1].as_i.should eq(42)
      data[2].as_bool.should be_true
      data[3].as_f.should eq(3.14)
    end
  end

  describe "#[]?" do
    it "returns nil for missing keys" do
      data = Oyl.parse("a: 1")
      data["b"]?.should be_nil
    end

    it "returns nil for out of range index" do
      data = Oyl.parse("- 1")
      data[5]?.should be_nil
    end
  end

  describe "#dig" do
    it "traverses nested structures" do
      yaml = "a:\n  b:\n    c: deep"
      Oyl.parse(yaml).dig("a", "b", "c").as_s.should eq("deep")
    end

    it "raises on missing key" do
      expect_raises(Exception) do
        Oyl.parse("a: 1").dig("b", "c")
      end
    end
  end

  describe "#dig?" do
    it "returns nil for missing path" do
      Oyl.parse("a: 1").dig?("b", "c").should be_nil
    end
  end

  describe "#size" do
    it "returns array size" do
      Oyl.parse("- 1\n- 2\n- 3").size.should eq(3)
    end

    it "returns hash size" do
      Oyl.parse("a: 1\nb: 2").size.should eq(2)
    end
  end

  describe "anchors and aliases" do
    it "resolves aliases" do
      yaml = "defaults: &defaults\n  host: localhost\n  port: 8080\nproduction:\n  <<: *defaults\n  port: 80"
      data = Oyl.parse(yaml)
      data["production"]["host"].as_s.should eq("localhost")
      data["production"]["port"].as_i.should eq(80)
    end

    it "keeps the keys a mapping writes over a merge's" do
      yaml = "b: &b {a: 1, c: 2}\nx: {a: 9, <<: *b}\ny: {<<: *b, a: 9}"
      data = Oyl.parse(yaml)
      data["x"].should eq(Oyl.parse("{a: 9, c: 2}"))
      data["y"].should eq(Oyl.parse("{a: 9, c: 2}"))
    end

    it "resolves scalar aliases" do
      yaml = "a: &val hello\nb: *val"
      data = Oyl.parse(yaml)
      data["a"].as_s.should eq("hello")
      data["b"].as_s.should eq("hello")
    end
  end

  describe "dialects" do
    it "reads YAML 1.1 as PyYAML does" do
      data = Oyl.parse("[yes, off, 017, 0b101, '1:30', 1:30, 2001-12-14, y]", dialect: :yaml11)
      data[0].as_bool.should be_true
      data[1].as_bool.should be_false
      data[2].as_i64.should eq(15_i64)
      data[3].as_i64.should eq(5_i64)
      data[4].as_s.should eq("1:30")
      data[5].as_i64.should eq(90_i64)
      data[6].as_time.should eq(Time.utc(2001, 12, 14))
      data[7].as_s.should eq("y")
    end

    it "reads go-yaml v2's types" do
      Oyl.parse("[y, N, 0777]", dialect: :goyaml2).should eq(Oyl.parse("[true, false, 511]"))
    end

    it "reads a %YAML 1.1 document as YAML 1.1" do
      docs = Oyl.parse_all("%YAML 1.1\n---\nyes\n...\n---\nyes")
      docs[0].as_bool.should be_true
      docs[1].as_s.should eq("yes")
    end

    it "raises on a YAML 1.1 date that doesn't exist" do
      expect_raises(Oyl::ParseException, "Invalid timestamp") do
        Oyl.parse("2001-02-29", dialect: :yaml11)
      end
    end
  end

  describe "multi-document" do
    it "parses all documents" do
      yaml = "---\nfoo\n---\nbar"
      docs = Oyl.parse_all(yaml)
      docs.size.should eq(2)
      docs[0].as_s.should eq("foo")
      docs[1].as_s.should eq("bar")
    end
  end

  describe "empty input" do
    it "returns nil for empty document" do
      Oyl.parse("").raw.should be_nil
    end
  end

  describe "errors" do
    it "raises ParseException on invalid YAML" do
      expect_raises(Oyl::ParseException) do
        Oyl.parse("- :\n[bad")
      end
    end
  end

  describe "equality" do
    it "compares raw values extracted from Any" do
      Oyl.parse("42").as_i.should eq(42)
      Oyl.parse("hello").as_s.should eq("hello")
    end

    it "compares two Any values" do
      Oyl.parse("42").should eq(Oyl::Any.new(42_i64))
      Oyl.parse("hello").should eq(Oyl::Any.new("hello"))
    end
  end

  describe "#to_s" do
    it "converts to string" do
      Oyl.parse("hello").to_s.should eq("hello")
      Oyl.parse("42").to_s.should eq("42")
    end
  end
end
