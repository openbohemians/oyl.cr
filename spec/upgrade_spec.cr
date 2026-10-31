require "./spec_helper"

describe "Oyl.upgrade" do
  it "rewrites what YAML 1.2 reads otherwise, by PyYAML's reading" do
    text, fixes = Oyl.upgrade("%YAML 1.1\n---\non: push  # mine\nenabled: yes\nmode: 0755\nratio: 1e3\n",
      from: :yaml11)
    text.should eq("---\n\"on\": push  # mine\nenabled: true\nmode: !!int 0o755\nratio: \"1e3\"\n")
    fixes.map(&.kind).should eq([Oyl::Fix::Kind::Directive, Oyl::Fix::Kind::Scalar,
                                 Oyl::Fix::Kind::Scalar, Oyl::Fix::Kind::Scalar, Oyl::Fix::Kind::Scalar])
    fixes.all?(&.fixed?).should be_true
    on = fixes[1]
    {on.line, on.column, on.before, on.after}.should eq({3, 1, "on", "\"on\""})
    on.message.should contain("the text kept")
  end

  it "reads the result alike in the reader and YAML 1.2" do
    text, _ = Oyl.upgrade("a: yes\nb: 0755\nc: 1:30\nd: 2001-12-14\n", from: :yaml11)
    Oyl.parse(text, dialect: :yaml11).should eq(Oyl.parse(text))
  end

  it "takes go-yaml v2's reading" do
    text, _ = Oyl.upgrade("a: y\nb: 2001-12-14\n", from: :goyaml2)
    text.should eq("a: true\nb: 2001-12-14\n")
  end

  it "reports what the readers disagree on when the reader is unknown" do
    text, fixes = Oyl.upgrade("a: yes\nb: 1e3\n")
    text.should eq("a: true\nb: 1e3\n")
    fixes.size.should eq(2)
    fixes[1].fixed?.should be_false
    fixes[1].after.should eq("")
    fixes[1].message.should contain("name the dialect")
  end

  it "flips the policy" do
    text, _ = Oyl.upgrade("on: 1\na: yes\n", from: :yaml11, keys_keep_value: true, values_keep_text: true)
    text.should eq("true: 1\na: \"yes\"\n")
  end

  it "raises for input its reader can't read" do
    expect_raises(Oyl::ParseException) { Oyl.upgrade("a: [1\n", from: :yaml11) }
  end
end
