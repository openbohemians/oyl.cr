# yam.cr

Crystal bindings for [yam](https://github.com/trans/yam), a fast YAML 1.2
parser written in C11. SIMD-accelerated, zero-copy, arena-allocated.

## Installation

Add the dependency to your `shard.yml`:

```yaml
dependencies:
  yam:
    github: trans/yam.cr
```

Then run `shards install`. The C library is built automatically via the
`postinstall` script.

Requires a C11 compiler (GCC or Clang).

## Quick Start

```crystal
require "yam"

data = Yam.parse("name: yam\nversion: 0.1.0")
data["name"].as_s   # => "yam"
data["version"].as_s # => "0.1.0"
```

## Usage

### Parsing to Any

`Yam.parse` returns a `Yam::Any` with automatic scalar resolution
(YAML 1.2 Core schema: null, bool, int, float):

```crystal
data = Yam.parse <<-YAML
  server:
    host: localhost
    port: 8080
    debug: true
  YAML

data["server"]["host"].as_s    # => "localhost"
data["server"]["port"].as_i    # => 8080
data["server"]["debug"].as_bool # => true
```

Quoted scalars are always strings:

```crystal
Yam.parse(%("42")).as_s # => "42"
Yam.parse("42").as_i    # => 42
```

### Multi-document

```crystal
docs = Yam.parse_all("---\nfoo\n---\nbar")
docs.size        # => 2
docs[0].as_s     # => "foo"
docs[1].as_s     # => "bar"
```

### Navigating data

```crystal
data = Yam.parse("a:\n  b:\n    c: deep")

data["a"]["b"]["c"].as_s   # => "deep"
data.dig("a", "b", "c").as_s # => "deep"
data.dig?("a", "x")          # => nil

data["a"].size # => 1
```

### Anchors and aliases

```crystal
yaml = <<-YAML
  defaults: &defaults
    adapter: postgres
    host: localhost
  production:
    <<: *defaults
    database: mydb
  YAML

data = Yam.parse(yaml)
data["production"]["host"].as_s     # => "localhost"
data["production"]["database"].as_s # => "mydb"
```

### AST (Nodes API)

For full control over the parsed structure, use the Nodes API:

```crystal
doc = Yam::Nodes.parse("key: value")
map = doc[0].as(Yam::Nodes::Mapping)

key = map.nodes[0].as(Yam::Nodes::Scalar)
key.value        # => "key"
key.start_line   # => 1
key.start_column # => 1

val = map.nodes[1].as(Yam::Nodes::Scalar)
val.value        # => "value"
val.start_line   # => 1
val.start_column # => 6
```

Node types: `Scalar`, `Sequence`, `Mapping`, `Alias`.

### PullParser (low-level)

Event-based streaming parser for maximum control:

```crystal
Yam::PullParser.new("name: yam\nversion: 0.1.0") do |parser|
  parser.read_stream do
    parser.read_document do
      parser.read_mapping do
        key = parser.read_scalar   # => "name"
        value = parser.read_scalar # => "yam"
        # ...
      end
    end
  end
end
```

## API

### Yam

| Method | Returns | Description |
|--------|---------|-------------|
| `Yam.parse(data)` | `Yam::Any` | Parse a single YAML document |
| `Yam.parse_all(data)` | `Array(Yam::Any)` | Parse all documents in a stream |

### Yam::Any

| Method | Description |
|--------|-------------|
| `as_s`, `as_s?` | Access as `String` |
| `as_i`, `as_i?` | Access as `Int32` |
| `as_i64`, `as_i64?` | Access as `Int64` |
| `as_f`, `as_f?` | Access as `Float64` |
| `as_bool`, `as_bool?` | Access as `Bool` |
| `as_a`, `as_a?` | Access as `Array(Yam::Any)` |
| `as_h`, `as_h?` | Access as `Hash(Yam::Any, Yam::Any)` |
| `as_bytes`, `as_bytes?` | Access as `Bytes` |
| `as_time`, `as_time?` | Access as `Time` |
| `[index]`, `[key]` | Index by `Int` or `String` |
| `[]?` | Non-raising index access |
| `dig(keys...)` | Traverse nested structures |
| `dig?(keys...)` | Non-raising dig |
| `size` | Collection size |
| `raw` | Underlying `Type` union value |

### Yam::Nodes

| Type | Description |
|------|-------------|
| `Scalar` | Scalar value (`value`, `style`, `anchor`, `tag`) |
| `Sequence` | Ordered list (`style`, `nodes`) |
| `Mapping` | Key-value pairs (`style`, `nodes` as flat key/val/key/val) |
| `Alias` | Alias reference (`value`, `resolved`) |
| `Document` | Document container (`nodes`) |

All nodes carry `start_line`, `start_column`, `end_line`, `end_column`
(1-based source positions).

## License

MIT
