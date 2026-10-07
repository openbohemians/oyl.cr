require "./spec_helper"

describe Oyl do
  it "has a version" do
    Oyl::VERSION.should eq("0.1.0")
  end

  it "parses a simple mapping" do
    data = Oyl.parse("name: oyl\nversion: 0.1.0")
    data["name"].as_s.should eq("oyl")
    data["version"].as_s.should eq("0.1.0")
  end

  it "parses a shard.yml-like file" do
    yaml = <<-YAML
      name: my-app
      version: 1.0.0
      dependencies:
        oyl:
          github: openbohemians/oyl.cr
          version: ~> 0.1.0
      YAML
    data = Oyl.parse(yaml)
    data["name"].as_s.should eq("my-app")
    data["dependencies"]["oyl"]["github"].as_s.should eq("openbohemians/oyl.cr")
  end

  it "handles arrays of hashes" do
    yaml = <<-YAML
      servers:
        - host: web1
          port: 8080
        - host: web2
          port: 8081
      YAML
    data = Oyl.parse(yaml)
    servers = data["servers"]
    servers.size.should eq(2)
    servers[0]["host"].as_s.should eq("web1")
    servers[0]["port"].as_i.should eq(8080)
    servers[1]["host"].as_s.should eq("web2")
    servers[1]["port"].as_i.should eq(8081)
  end
end
