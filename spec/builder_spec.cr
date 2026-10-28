require "./spec_helper"

record Point, x : Int32, y : Int32 do
  def to_oyl(builder : Oyl::Builder) : Nil
    builder.mapping(style: :flow) do
      builder.write "x"
      builder.write x
      builder.write "y"
      builder.write y
    end
  end
end

# Random values for round trips, from text YAML treats specially.
module RoundTrip
  PIECES = ["a", "b", "yes", "no", "on", "y", "N", "true", "null", "~", "0", "017",
            "0x1F", "0o7", "0b101", "1:30", "1_000", "2001-12-14", ".5", "-.5", "1e3",
            ".inf", "-", ":", "#", " ", "  ", "\t", "\n", "\r", "\r\n", "'", "\"",
            "\\", "&a", "*a", "!x", "|", ">", "%", "@", "`", "{", "}", "[", "]",
            ",", "?", "é", "日本", "\u0085", "\u2028", "\u0000", "\u007F", "\uFEFF",
            "<<", "=", "- x", ": y", "? z", "...", "---"]

  def self.string(r : Random) : String
    String.build { |s| r.rand(0..5).times { s << PIECES.sample(r) } }
  end

  def self.scalar(r : Random) : Oyl::Any
    Oyl::Any.new(case r.rand(9)
    when 0 then nil
    when 1 then r.next_bool
    when 2 then r.rand(Int64::MIN..Int64::MAX)
    when 3 then r.rand(Int64::MAX.to_u64 + 1..UInt64::MAX)
    when 4 then (r.next_float - 0.5) * 10.0 ** r.rand(-12..12)
    when 5 then [Float64::INFINITY, -Float64::INFINITY, 0.0, -1.0, 1e300].sample(r)
    when 6 then Time.unix_ms(r.rand(0_i64..4_000_000_000_000_i64))
    when 7 then Bytes.new(r.rand(0..9)) { r.rand(256).to_u8 }
    else        string(r)
    end)
  end

  def self.value(r : Random, depth = 0) : Oyl::Any
    return scalar(r) if depth >= 3 || r.rand(3) > 0
    if r.next_bool
      Oyl::Any.new(Array(Oyl::Any).new(r.rand(0..4)) { value(r, depth + 1) })
    else
      hash = {} of Oyl::Any => Oyl::Any
      r.rand(0..4).times do
        key = r.rand(5) == 0 ? value(r, depth + 1) : Oyl::Any.new(string(r))
        hash[key] = value(r, depth + 1)
      end
      Oyl::Any.new(hash)
    end
  end
end

describe Oyl::Builder do
  describe "#write" do
    it "quotes strings YAML 1.2 or 1.1 would read as another type" do
      %w(true null ~ 42 0x1F 1.5 .inf yes no on off y n 017 1:30 2001-12-14 << =).each do |word|
        Oyl.dump(word).should eq(%("#{word}"\n))
      end
      Oyl.dump("").should eq(%(""\n))
      Oyl.dump("hello").should eq("hello\n")
    end

    it "writes text with line breaks as a literal block" do
      Oyl.dump({"a" => "one\ntwo\n"}).should eq("a: |\n  one\n  two\n")
    end

    it "writes values in YAML 1.2's forms" do
      Oyl.dump([nil, true, -7, 1.5, 1e20, Float64::INFINITY, Float64::NAN], style: :flow)
        .should eq("[null, true, -7, 1.5, 1.0e+20, .inf, .nan]\n")
      Oyl.dump(UInt64::MAX).should eq("18446744073709551615\n")
    end

    it "tags times and bytes" do
      Oyl.dump(Time.utc(2001, 12, 14)).should eq("!!timestamp 2001-12-14\n")
      Oyl.dump("hi".to_slice).should eq("!!binary aGk=\n")
    end

    it "writes collections, tuples and symbols" do
      Oyl.dump({a: [1, 2], b: {:c, "d"}}).should eq("a:\n  - 1\n  - 2\nb:\n  - c\n  - d\n")
      Oyl.dump({1 => "one", [1, 2] => "pair"}, style: :flow).should eq("{1: one, [1, 2]: pair}\n")
    end

    it "writes a type with its to_oyl" do
      # go-yaml v2 reads a plain y as true
      Oyl.dump([Point.new(1, 2)]).should eq(%(- {x: 1, "y": 2}\n))
    end

    it "converts YAML 1.1 to YAML 1.2" do
      old = "on: push\nenabled: yes\nmode: 0755\nwhen: 2001-12-14\n"
      Oyl.parse(old, dialect: :yaml11).to_yaml
        .should eq(%(true: push\nenabled: true\nmode: 493\nwhen: !!timestamp 2001-12-14\n))
    end
  end

  it "writes events as given" do
    yaml = Oyl::Builder.build do |b|
      b.document(explicit_start: true) do
        b.mapping do
          b.scalar "base"
          b.mapping(anchor: "b") do
            b.scalar "a"
            b.scalar "1"
          end
          b.scalar "copy"
          b.alias "b"
          b.scalar "text"
          b.scalar "true", style: :single_quoted, tag: "!note"
        end
      end
    end
    yaml.should eq("---\nbase: &b\n  a: 1\ncopy: *b\ntext: !note 'true'\n")
  end

  it "writes a stream of documents" do
    yaml = Oyl.dump_all([1, "two", [3]])
    yaml.should eq("1\n---\ntwo\n---\n- 3\n")
    Oyl.parse_all(yaml).should eq([Oyl.parse("1"), Oyl.parse("two"), Oyl.parse("[3]")])
  end

  it "raises on events out of order" do
    expect_raises(Oyl::Error) do
      Oyl::Builder.build do |b|
        b.document do
          b.scalar "a"
          b.scalar "b"
        end
      end
    end
  end

  it "raises once closed" do
    b = Oyl::Builder.new
    b.close
    expect_raises(Oyl::Error, "Builder is closed") { b.document { } }
  end

  {% for style in %w(block flow minimal) %}
    it "writes random values that read back the same ({{style.id}})" do
      r = Random.new(42)
      2000.times do
        value = RoundTrip.value(r)
        yaml = Oyl.dump(value, style: :{{style.id}}, indent: r.rand(1..10))
        {Oyl::Dialect::YAML12, Oyl::Dialect::YAML11}.each do |dialect|
          back = begin
            Oyl.parse(yaml, dialect: dialect)
          rescue ex
            fail "#{value.inspect} wrote #{yaml.inspect}, which #{dialect} can't read: #{ex.message}"
          end
          fail "#{value.inspect} wrote #{yaml.inspect}, read by #{dialect} as #{back.inspect}" unless back == value
        end
      end
    end
  {% end %}
end
