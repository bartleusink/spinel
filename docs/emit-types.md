# `--emit-types`: the inferred types as JSON

```sh
spinel app.rb --emit-types            # -> app.types.json
spinel app.rb --emit-types -o out.json
```

`--emit-types` runs the whole compile (parse, analyze, codegen to a
discarded buffer) and writes what the compiler knew as JSON: a type for
every node it typed, and the diagnostics. No binary is written. The exit
status is the compile's: a program the compiler refuses exits 1 and the
JSON still carries the refusals. With `-S` as well
(`spinel app.rb --emit-types -o app.json -S`) the C of that same compile
goes to stdout, so a consumer showing both runs the compiler once. It is the surface the out-of-tree
editor tools read (rubys/spinel-ide); nothing in this tree consumes it
beyond the gate's own check.

```json
{
  "types": [
    {"file":"app.rb","line":11,"col":5,"end_line":11,"end_col":8,
     "kind":"LocalVariableReadNode","name":"pts",
     "type":"poly_array","rbs":"Array[untyped]"},
    {"file":"app.rb","line":4,"col":2,"end_line":4,"end_col":47,
     "kind":"DefNode","name":"dist2","type":"symbol","rbs":"Symbol",
     "owner":"Point","signature":"(untyped) -> Integer","widened":true},
    ...
  ],
  "diagnostics": [
    {"file":"app.rb","line":4,"col":12,"end_line":4,"end_col":13,
     "severity":"warning","method":"dist2","slot":"param","param":"o",
     "message":"Spinel: parameter `o` of `dist2` widened to untyped (boxed poly slow path)"},
    {"file":"app.rb","line":9,"col":0,"end_line":9,"end_col":19,
     "severity":"warning","method":"widen","slot":"return",
     "message":"Spinel: the return of `widen` widened to untyped (boxed poly slow path)"},
    {"file":"app.rb","line":20,"col":0,"severity":"error",
     "message":"unsupported class variable read (no class scope): node 4 (ClassVariableReadNode)"}
  ],
  "codegen": [
    {"file":"app.rb","line":11,"col":18,"end_line":11,"end_col":32,
     "kind":"CallNode","name":"dist2","dispatch":"switch"},
    {"file":"app.rb","line":11,"col":12,"end_line":11,"end_col":34,
     "kind":"BlockNode","inlined":true},
    ...
  ]
}
```

## `types`

One record per node the analyzer gave a concrete type (nodes typed
unknown or void are left out), in node order.

- `file`, `line`, `col`: the node's start, as Prism reports it (the line
  is 1-based, the column 0-based).
- `end_line`, `end_col`: Prism's exclusive end. Several expressions can
  start at one column (`pts`, `pts.map { }`, `pts.map { }.inspect`); the
  tightest span containing a position is the expression at it.
- `kind`: the Prism node type (`CallNode`, `LocalVariableReadNode`,
  `DefNode`, ...).
- `name`: present where the node names something: a call's method, a
  variable, a constant, a def, a parameter.
- `type`: spinel's internal type tag; `rbs`: the same type as RBS, which
  is the type language to read. A `DefNode`'s own type is the def
  expression's value (a `Symbol`); the method type it declares is its
  `signature`, `(Integer, Integer) -> Array[Integer]`, the text
  `--emit-rbs` writes for that method, with `"widened":true` when a slot
  of it degraded to untyped, `owner` naming the class it is defined in
  (`Object` at the top level) and `"singleton":true` for a `def self.x`.
  The signatures are all here, placed: a consumer does not need the
  `--emit-rbs` pass or a text scan for the defs.

## `diagnostics`

- `"severity":"warning"`, one per widened slot of a method whose
  signature degraded to untyped: `method` names the def, `slot` is
  `"param"` (with `param`, the parameter's name; the position is the
  parameter's) or `"return"` (the position is the def's). The
  `signature` in `types` at the def shows the whole method type.
- `"severity":"error"`, one per refusal, in the order the compile met
  them, at the refused construct: the same lines the compile prints on
  stderr ([limitations.md](limitations.md) says what a refusal is).
- A program that does not parse exits 1 too, and the JSON is still
  written: `types` and `codegen` empty, and one `"severity":"error"` per
  parse error at its span (`end_line`/`end_col` included), in the file
  it is in when a `require_relative` spliced it — the same
  `file:line:col: message` lines the compile prints on stderr.

## `codegen`

What codegen decided, one record per call it placed and per block:

- a `CallNode` carries `dispatch`: `"direct"` (one statically bound C
  call, or a builtin emitted in place: the fast path), `"switch"` (a
  switch over the classes or tags the receiver can hold, each arm a
  direct call), or `"boxed"` (the receiver is a boxed value and a runtime
  helper answers over its tag; an unresolved call is here too).
- a `BlockNode` carries `inlined`: `true` when the block was spliced into
  its caller (an iterator's body, a yielding method's block), `false`
  when it became a function of its own (a proc or lambda, a `Fiber.new`
  or `Thread.new` body).

A call the compiler never placed (unreachable, or folded into something
else) has no record. The lens is per site: the one call in a method that
took a switch or the boxed path is the one to look at.

What the JSON does not say: why a slot widened (which call site or
assignment unified it to untyped). That is an open question, not a
field.
