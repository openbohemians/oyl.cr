# oyl.cr

Crystal bindings for [Oyl](https://github.com/openbohemians/oyl), the
optimized YAML library: a fast YAML 1.2 parser and emitter written in C11.
SIMD-accelerated, zero-copy, arena-allocated.

## Installation

Add the dependency to your `shard.yml`:

```yaml
dependencies:
  oyl:
    github: openbohemians/oyl.cr
```

Then run `shards install`. The C library, vendored in `ext/oyl`, is built
by the `postinstall` script.

Requires a C11 compiler (GCC or Clang).

## Quick Start

```crystal
require "oyl"

data = Oyl.parse("name: oyl\nversion: 0.1.0")
data["name"].as_s   # => "oyl"
data["version"].as_s # => "0.1.0"
```

## Usage

### Parsing to Any

`Oyl.parse` returns an `Oyl::Any`, resolving plain scalars the way
Crystal's own `YAML` module does: null, booleans (including `yes`/`no` and
`on`/`off`), integers, floats and timestamps, plus `!!binary`:

```crystal
data = Oyl.parse <<-YAML
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
Oyl.parse(%("42")).as_s # => "42"
Oyl.parse("42").as_i    # => 42
```

### Multi-document

```crystal
docs = Oyl.parse_all("---\nfoo\n---\nbar")
docs.size        # => 2
docs[0].as_s     # => "foo"
docs[1].as_s     # => "bar"
```

### Navigating data

```crystal
data = Oyl.parse("a:\n  b:\n    c: deep")

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

data = Oyl.parse(yaml)
data["production"]["host"].as_s     # => "localhost"
data["production"]["database"].as_s # => "mydb"
```

### AST (Nodes API)

For full control over the parsed structure, use the Nodes API:

```crystal
doc = Oyl::Nodes.parse("key: value")
map = doc[0].as(Oyl::Nodes::Mapping)

key = map.nodes[0].as(Oyl::Nodes::Scalar)
key.value        # => "key"
key.start_line   # => 1
key.start_column # => 1

val = map.nodes[1].as(Oyl::Nodes::Scalar)
val.value        # => "value"
val.start_line   # => 1
val.start_column # => 6
```

Node types: `Scalar`, `Sequence`, `Mapping`, `Alias`.

### PullParser (low-level)

Event-based streaming parser for maximum control:

```crystal
Oyl::PullParser.new("name: oyl\nversion: 0.1.0") do |parser|
  parser.read_stream do
    parser.read_document do
      parser.read_mapping do
        key = parser.read_scalar   # => "name"
        value = parser.read_scalar # => "oyl"
        # ...
      end
    end
  end
end
```

It enforces Oyl's safety limits: 10,000 events and a nesting depth of 256
by default, raised with `max_events:` and `max_depth:` (0 turns a limit
off). Exceeding one raises `Oyl::ParseException`:

```crystal
Oyl::PullParser.new(File.read("big.yaml"), max_events: 1_000_000) do |parser|
  # ...
end
```

## API

### Oyl

| Method | Returns | Description |
|--------|---------|-------------|
| `Oyl.parse(data)` | `Oyl::Any` | Parse a single YAML document |
| `Oyl.parse_all(data)` | `Array(Oyl::Any)` | Parse all documents in a stream |

### Oyl::Any

| Method | Description |
|--------|-------------|
| `as_s`, `as_s?` | Access as `String` |
| `as_i`, `as_i?` | Access as `Int32` |
| `as_i64`, `as_i64?` | Access as `Int64` |
| `as_f`, `as_f?` | Access as `Float64` |
| `as_bool`, `as_bool?` | Access as `Bool` |
| `as_a`, `as_a?` | Access as `Array(Oyl::Any)` |
| `as_h`, `as_h?` | Access as `Hash(Oyl::Any, Oyl::Any)` |
| `as_bytes`, `as_bytes?` | Access as `Bytes` |
| `as_time`, `as_time?` | Access as `Time` |
| `[index]`, `[key]` | Index by `Int` or `String` |
| `[]?` | Non-raising index access |
| `dig(keys...)` | Traverse nested structures |
| `dig?(keys...)` | Non-raising dig |
| `size` | Collection size |
| `raw` | Underlying `Type` union value |

### Oyl::Nodes

| Type | Description |
|------|-------------|
| `Scalar` | Scalar value (`value`, `style`, `anchor`, `tag`) |
| `Sequence` | Ordered list (`style`, `nodes`) |
| `Mapping` | Key-value pairs (`style`, `nodes` as flat key/val/key/val) |
| `Alias` | Alias reference (`value`, `resolved`) |
| `Document` | Document container (`nodes`) |

All nodes carry `start_line`, `start_column`, `end_line`, `end_column`
(1-based source positions).

## Updating the C library

`ext/oyl` holds a copy of Oyl's sources, and `ext/oyl/COMMIT` names the
commit they came from. To vendor another version, from a checkout of
[Oyl](https://github.com/openbohemians/oyl):

```sh
make -C ext sync OYL=path/to/oyl   # copies the checkout's committed HEAD
make -C ext && crystal spec
```

## License

MIT; Oyl's own license is in `ext/oyl/LICENSE`.
