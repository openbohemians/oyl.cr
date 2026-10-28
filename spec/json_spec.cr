require "./spec_helper"
require "json"

# Random values JSON can hold, from text YAML treats specially.
module JSONRoundTrip
  PIECES = ["a", "yes", "on", "y", "N", "true", "null", "~", "017", "0x1F", "1:30",
            "1_000", "2001-12-14", "1e3", ".inf", "-", ":", "#", " ", "\t", "\n",
            "'", "\"", "\\", "&a", "*a", "!x", "|", "{", "[", ",", "é", "日本",
            " ", "\u0000", "\u007F", "<<", "=", "- x", "---"]

  def self.string(r : Random) : String
    String.build { |s| r.rand(0..4).times { s << PIECES.sample(r) } }
  end

  def self.value(r : Random, depth = 0) : Oyl::Any
    if depth >= 3 || r.rand(3) > 0
      return Oyl::Any.new(case r.rand(5)
      when 0 then nil
      when 1 then r.next_bool
      when 2 then r.rand(Int64::MIN..Int64::MAX)
      when 3 then (r.next_float - 0.5) * 10.0 ** r.rand(-12..12)
      else        string(r)
      end)
    end
    if r.next_bool
      Oyl::Any.new(Array(Oyl::Any).new(r.rand(0..4)) { value(r, depth + 1) })
    else
      hash = {} of Oyl::Any => Oyl::Any
      r.rand(0..4).times { hash[Oyl::Any.new(string(r))] = value(r, depth + 1) }
      Oyl::Any.new(hash)
    end
  end

  # Whether *json* holds the same data as *value*.
  def self.same?(json : JSON::Any, value : Oyl::Any) : Bool
    case raw = value.raw
    when Nil, Bool, String
      json.raw == raw
    when Int64
      json.raw == raw
    when Float64
      (j = json.raw).is_a?(Int64 | Float64) && j.to_f == raw
    when Array
      a = json.as_a? || return false
      a.size == raw.size && a.zip(raw).all? { |j, v| same?(j, v) }
    when Hash
      h = json.as_h? || return false
      h.size == raw.size && raw.all? { |k, v| (j = h[k.as_s]?) && same?(j, v) }
    else
      false
    end
  end
end

describe "Oyl.yaml_to_json" do
  it "types scalars by YAML 1.2" do
    Oyl.yaml_to_json("b: yes\na: 0777\nc: 0x1F\nd: ~\ne: 1.5\nf: 2001-12-14\n")
      .should eq(%({"a":777,"b":"yes","c":31,"d":null,"e":1.5,"f":"2001-12-14"}))
  end

  it "types scalars by a dialect" do
    Oyl.yaml_to_json("b: yes\na: 0777\n", dialect: :goyaml2).should eq(%({"a":511,"b":true}))
    Oyl.yaml_to_json("on: yes\nn: 1:30\n", dialect: :yaml11).should eq(%({"n":90,"true":true}))
    Oyl.yaml_to_json("%YAML 1.1\n---\non: 0755\n").should eq(%({"true":493}))
  end

  it "applies merges in the dialect's order" do
    yaml = "b: &b {a: 1, c: 2}\nx: {a: 9, <<: *b}\n"
    Oyl.yaml_to_json(yaml).should eq(%({"b":{"a":1,"c":2},"x":{"a":9,"c":2}}))
    # go-yaml v2 lets a merge override the keys before it
    Oyl.yaml_to_json(yaml, dialect: :goyaml2).should eq(%({"b":{"a":1,"c":2},"x":{"a":1,"c":2}}))
  end

  it "keeps a repeated key's last value, or raises with strict" do
    Oyl.yaml_to_json("a: 1\na: 2\n").should eq(%({"a":2}))
    expect_raises(Oyl::ParseException, %(key "a" already set in map at line 2, column 1)) do
      Oyl.yaml_to_json("a: 1\na: 2\n", strict: true)
    end
  end

  it "raises on invalid YAML, with where" do
    ex = expect_raises(Oyl::ParseException) { Oyl.yaml_to_json("a: [1\n") }
    ex.location.should eq({2, 1})
  end

  it "writes tagged scalars and keys as JSON has them" do
    Oyl.yaml_to_json("x: !!binary aGk=\nt: \"é\\u2028\"\n1: one\n")
      .should eq(%({"1":"one","t":"é\\u2028","x":"hi"}))
    expect_raises(Oyl::ParseException, "unsupported map key: null") { Oyl.yaml_to_json("null: z") }
  end

  it "converts the first document" do
    Oyl.yaml_to_json("1\n---\n[\n").should eq("1")
    Oyl.yaml_to_json(IO::Memory.new("a: 1")).should eq(%({"a":1}))
    Oyl.yaml_to_json("").should eq("null")
  end

  it "converts every document with yaml_to_json_all" do
    Oyl.yaml_to_json_all("1\n---\na: [x, \"y\"]\n---\n").should eq(["1", %({"a":["x","y"]}), "null"])
    Oyl.yaml_to_json_all("").should be_empty
  end

  it "agrees with Oyl.parse on random values" do
    r = Random.new(42)
    1000.times do
      value = JSONRoundTrip.value(r)
      yaml = Oyl.dump(value, style: r.next_bool ? Oyl::EmitStyle::BLOCK : Oyl::EmitStyle::FLOW)
      Oyl::Dialect.each do |dialect|
        json = Oyl.yaml_to_json(yaml, dialect: dialect)
        unless JSONRoundTrip.same?(JSON.parse(json), value)
          fail "#{value.inspect} wrote #{yaml.inspect}, which #{dialect} converts to #{json}"
        end
      end
    end
  end
end
