require "./spec_helper"

describe Yam do
  it "has a version" do
    Yam::VERSION.should eq("0.1.0")
  end

  it "parses a simple mapping" do
    data = Yam.parse("name: yam\nversion: 0.1.0")
    data["name"].as_s.should eq("yam")
    data["version"].as_s.should eq("0.1.0")
  end

  it "parses a shard.yml-like file" do
    yaml = <<-YAML
      name: my-app
      version: 1.0.0
      dependencies:
        yam:
          github: trans/yam.cr
          version: ~> 0.1.0
      YAML
    data = Yam.parse(yaml)
    data["name"].as_s.should eq("my-app")
    data["dependencies"]["yam"]["github"].as_s.should eq("trans/yam.cr")
  end

  it "handles arrays of hashes" do
    yaml = <<-YAML
      servers:
        - host: web1
          port: 8080
        - host: web2
          port: 8081
      YAML
    data = Yam.parse(yaml)
    servers = data["servers"]
    servers.size.should eq(2)
    servers[0]["host"].as_s.should eq("web1")
    servers[0]["port"].as_i.should eq(8080)
    servers[1]["host"].as_s.should eq("web2")
    servers[1]["port"].as_i.should eq(8081)
  end
end
