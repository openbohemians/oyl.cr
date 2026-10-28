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

`Oyl.parse` returns an `Oyl::Any`, typing plain scalars by YAML 1.2's Core
schema, as Oyl does: `null`, `~` and empty are null; `true` and `false`
are booleans; decimal, `0o` and `0x` integers; floats, `.inf` and `.nan`.
Everything else is a string, including `yes`, `on` and dates:

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

A tag reads its scalar by the tag's rules: `!!str 42` is a string,
`!!binary` gives `Bytes`, `!!timestamp` a `Time`, and a scalar its tag
doesn't accept (`!!int abc`) raises `Oyl::ParseException`.

### YAML 1.1 and other dialects

Files written for YAML 1.1 loaders (PyYAML, Ruby's Psych, go-yaml v2) mean
some scalars differently. Choose how to read them with `dialect:`:

```crystal
Oyl.parse("[yes, 017, 1:30, 2001-12-14]")                    # => ["yes", 17, "1:30", "2001-12-14"]
Oyl.parse("[yes, 017, 1:30, 2001-12-14]", dialect: :yaml11) # => [true, 15, 90, 2001-12-14 00:00:00Z]
```

| Dialect | Types |
|---------|-------|
| `:yaml12` | YAML 1.2's Core schema (the default) |
| `:yaml11` | YAML 1.1 as PyYAML reads it: also `yes`/`no`/`on`/`off`, 0-octal, `0b`, base 60, timestamps |
| `:goyaml2` | go-yaml v2's, as Kubernetes reads YAML: YAML 1.1's and `y`/`n` |

A document that starts with `%YAML 1.1` is read as YAML 1.1 whatever the
dialect (as go-yaml v2 with `:goyaml2`).

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

`Oyl.parse` applies `<<` merge keys, and the keys a mapping writes win
over the ones it merges, wherever the `<<` is. (go-yaml v2 lets a merge
override the keys written before it; with `:goyaml2`, `Oyl.yaml_to_json`
copies that, `Oyl.parse` doesn't yet.)

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

Node types: `Scalar`, `Sequence`, `Mapping`, `Alias`. Scalars keep their
text, and `<<` merge keys stay as written unless you pass `merge: true`.
`Oyl::Nodes.parse` takes `dialect:` too, and each `Document` records the
dialect that types its scalars (`Oyl::Dialect#value` gives a scalar's).

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

### Writing YAML

`Oyl.dump` writes a value as a YAML 1.2 document, and `Oyl::Any#to_yaml`
writes what `Oyl.parse` read:

```crystal
Oyl.dump({"name" => "oyl", "tags" => ["fast", "yaml"], "port" => 8080})
# name: oyl
# tags:
#   - fast
#   - yaml
# port: 8080
```

What it writes reads back as the same value, in YAML 1.2 and in YAML 1.1
readers alike. A string that either would read as another type is quoted:
`"yes"`, `"on"`, `"n"` (a boolean to go-yaml v2), `"017"`, `"1:30"`,
`""`. Text with line breaks is written as a literal block (`|`). YAML 1.2
has no plain form for times and bytes, so a `Time` is written
`!!timestamp 2001-12-14` and `Bytes` as `!!binary`.

Reading a YAML 1.1 file and writing it back gives YAML 1.2, with the
values YAML 1.1 gave them (comments and layout aren't kept):

```crystal
Oyl.parse("enabled: yes\nmode: 0755\n", dialect: :yaml11).to_yaml
# enabled: true
# mode: 493
```

`style:` chooses `:block` (the default), `:flow` or `:minimal`, and
`indent:` the spaces per level (1 to 10, default 2). `Oyl.dump_all` writes
several documents.

Nil, booleans, numbers, strings, symbols, `Time`, `Bytes`, arrays,
tuples, sets, hashes and named tuples write themselves. A type of your own
does with `to_oyl`:

```crystal
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

Oyl.dump([Point.new(1, 2)]) # => "- {x: 1, \"y\": 2}\n"
```

`Oyl::Builder` writes events as given, with anchors, aliases, tags and
styles. `scalar` writes text, quoting it only where plain text wouldn't
read back as the same text, so use `write` for values:

```crystal
yaml = Oyl::Builder.build do |b|
  b.document do
    b.mapping do
      b.scalar "base"
      b.mapping(anchor: "b") do
        b.scalar "a"
        b.write 1
      end
      b.scalar "copy"
      b.alias "b"
    end
  end
end
# base: &b
#   a: 1
# copy: *b
```

### YAML to JSON

`Oyl.yaml_to_json` converts a YAML document to JSON in Oyl's C, with no
Crystal values in between: scalars typed by the dialect, aliases
expanded, `<<` merge keys applied, keys sorted. With `dialect: :goyaml2`
the output is byte for byte what sigs.k8s.io/yaml's `YAMLToJSON` writes,
as Kubernetes reads YAML:

```crystal
Oyl.yaml_to_json("b: yes\na: 0777\n")                    # => {"a":777,"b":"yes"}
Oyl.yaml_to_json("b: yes\na: 0777\n", dialect: :goyaml2) # => {"a":511,"b":true}
```

A key set twice keeps its last value, or with `strict: true` raises
`Oyl::ParseException`. `Oyl.yaml_to_json_all` converts every document of
a stream.

## API

### Oyl

| Method | Returns | Description |
|--------|---------|-------------|
| `Oyl.parse(data, dialect: :yaml12)` | `Oyl::Any` | Parse a single YAML document |
| `Oyl.parse_all(data, dialect: :yaml12)` | `Array(Oyl::Any)` | Parse all documents in a stream |
| `Oyl.dump(value, style: :block, indent: 2)` | `String` | Write a value as a YAML document (also to an `IO`) |
| `Oyl.dump_all(values, style: :block, indent: 2)` | `String` | Write each value as a document of one stream |
| `Oyl.yaml_to_json(data, dialect: :yaml12, strict: false)` | `String` | Convert the first document to JSON |
| `Oyl.yaml_to_json_all(data, dialect: :yaml12, strict: false)` | `Array(String)` | Convert each document to JSON |

### Oyl::Any

| Method | Description |
|--------|-------------|
| `as_s`, `as_s?` | Access as `String` |
| `as_i`, `as_i?` | Access as `Int32` |
| `as_i64`, `as_i64?` | Access as `Int64` |
| `as_u64`, `as_u64?` | Access as `UInt64` (integers above `Int64::MAX`) |
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
| `to_yaml` | Write as a YAML document (also to an `IO`) |

### Oyl::Builder

| Method | Description |
|--------|-------------|
| `Builder.build(style:, indent:) { \|b\| }` | Yield a builder, return the YAML |
| `document(explicit_start:, explicit_end:) { }` | A document around what the block writes |
| `write(value)` | A value, written to read back the same |
| `scalar(text, style:, tag:, anchor:)` | Text as given |
| `sequence(style:, tag:, anchor:) { }` | A sequence of what the block writes |
| `mapping(style:, tag:, anchor:) { }` | A mapping: a key, then its value |
| `alias(anchor)` | An alias |

### Oyl::Nodes

| Type | Description |
|------|-------------|
| `Scalar` | Scalar value (`value`, `style`, `anchor`, `tag`) |
| `Sequence` | Ordered list (`style`, `nodes`) |
| `Mapping` | Key-value pairs (`style`, `nodes` as flat key/val/key/val) |
| `Alias` | Alias reference (`value`, `resolved`) |
| `Document` | Document container (`nodes`, `dialect`) |

All nodes carry `start_line`, `start_column`, `end_line`, `end_column`
(1-based source positions).

## Updating the C library

`ext/oyl` holds a copy of Oyl's sources, and `ext/oyl/COMMIT` names the
commit they came from. To vendor another version, from a checkout of
[Oyl](https://github.com/openbohemians/oyl):

```sh
make -C ext sync OYL=path/to/oyl              # the checkout's committed HEAD
make -C ext sync OYL=path/to/oyl REF=v1.2.0   # or a release tag
make -C ext && crystal spec
```

## License

MIT; Oyl's own license is in `ext/oyl/LICENSE`.
