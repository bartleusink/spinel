# `--emit-types`: the inferred types as JSON

```sh
spinel app.rb --emit-types            # -> app.types.json
spinel app.rb --emit-types -o out.json
```

`--emit-types` runs the whole compile (parse, analyze, codegen to a
discarded buffer) and writes what the compiler knew as JSON: a type for
every node it typed, and the diagnostics. No binary is written. The exit
status is the compile's: a program the compiler refuses exits 1 and the
JSON still carries the refusals. It is the surface the out-of-tree
editor tools read (rubys/spinel-ide); nothing in this tree consumes it
beyond the gate's own check.

```json
{
  "types": [
    {"file":"app.rb","line":11,"col":5,"end_line":11,"end_col":8,
     "kind":"LocalVariableReadNode","name":"pts",
     "type":"poly_array","rbs":"Array[untyped]"},
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
  is the type language to read.

## `diagnostics`

- `"severity":"warning"`, one per widened slot of a method whose
  signature degraded to untyped: `method` names the def, `slot` is
  `"param"` (with `param`, the parameter's name; the position is the
  parameter's) or `"return"` (the position is the def's). The RBS in
  `types` at the def shows the whole signature.
- `"severity":"error"`, one per refusal, in the order the compile met
  them, at the refused construct: the same lines the compile prints on
  stderr ([limitations.md](limitations.md) says what a refusal is).

What the JSON does not say: why a slot widened (which call site or
assignment unified it to untyped), and what codegen decided at a call
(a direct call, a class switch, a boxed send). Both are open questions,
neither is a field yet.
