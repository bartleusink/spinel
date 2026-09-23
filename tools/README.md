# spinel tools

Developer tools that ship with the compiler. They are written in the
spinel subset and compiled by `spinel` itself, so their only runtime
dependency is `cc` -- the same as the compiler. `make` builds them
(`build/spinel-<name>`) and `make install` places them on `PATH` beside
`spinel`, so each is invoked like a subcommand:

```
spinel-doctor app.rb
spinel-reduce app.rb
spinel-flatten app.rb
spinel diff app.rb        # = spinel-diff app.rb; the compiler dispatches it
```

They locate the compiler at run time via, in order: `$SPINEL` (an
explicit path), `$SPINEL_DIR/spinel`, then `spinel` on `PATH`.

## spinel-doctor

One health report for a program. Independent legs:

- **build** -- compiles to a binary; reports any compile / codegen /
  C-build failure (with `--line-map`, so C errors point at Ruby lines).
- **unsupported** -- codegen gaps that degrade to a stub.
- **unresolved** -- calls that silently degrade to `nil`/`0` where CRuby
  would raise (via `SPINEL_WARN_UNRESOLVED`).
- **inference** -- methods spinel widened to `untyped` (the boxed poly
  slow path).
- **advice** -- where the boxed slow path will cost: boxed operations in
  the generated C, ranked by loop depth and mapped back to source lines.
  Depth propagates across the static call graph -- a method reached only
  from a deep nest ranks at its callers' depth, not at zero. Still
  static, so it ranks by *expected* cost with no time axis -- a deep
  startup nest can outrank per-frame work of the same depth; confirm
  with a profiler before optimizing
  ([`docs/profiling.md`](../docs/profiling.md)).
- **requires** -- non-relative `require`s spinel treats as native / no-op.
- **behavior** -- optional: compiled output vs CRuby; needs `ruby` on
  `PATH` and skips cleanly otherwise.

```
spinel-doctor [--only a,b] [--skip a,b] [--behavior] [--quiet] app.rb
```

Exit `0` clean, `1` when any leg reports an error-severity finding.

## spinel-reduce

Delta-debug (ddmin) a degrading program down to a minimal input that
still reproduces a chosen failure. Re-runs `spinel` only.

```
spinel-reduce [--oracle build|unsupported|unresolved] \
              [--oracle-cmd 'CMD {}'] [-o OUT] app.rb
```

`--oracle` selects the built-in interestingness test (default `build` =
spinel exits non-zero). `--oracle-cmd` is an escape hatch: `{}` is
replaced by the candidate file and the candidate is kept when `CMD`
exits `0`. Flatten multi-file programs first so reduce has one input.

## spinel-flatten

Inline a `require_relative` graph into one self-contained file, so
`spinel-reduce` and bug reports operate on a single input.

```
spinel-flatten [-o OUT] app.rb
```

## compile_scale

`make bench-compile` (or `ruby tools/compile_scale.rb [--cc] [--check] K...`)
generates the synthetic program of `compile_scale_gen.rb` -- K units of a
model, a store, a subclass of a shared base and a driver (#4847) -- and times
spinel's analysis (`--emit-rbs`) and C emission (`-c`) at each K, printing the
growth between consecutive sizes. The program is linear in K, so a linear
phase shows x2 per doubling. `--cc` also builds the binary; `--check` compares
its output with CRuby's.

## Adding a tool

Drop `tools/<name>.rb` (subset Ruby, `require_relative "tool_common"`
for the shared helpers); `make` compiles it to `build/spinel-<name>` and
`make install` installs it. Keep it within the subset -- a tool that
stops compiling breaks the build.

## spinel diff

The same program under CRuby and under Spinel, compared mechanically:
stdout, the uncaught exception (as `Class: message`, without the
backtrace) and the exit status, after the parts no two processes share
are folded (`#<Foo:0x...>` addresses, the program's and the scratch
directory, wall-clock times) and, under a ruby older than 3.4, the
spellings Spinel writes the 3.4 way (`{"a" => 1}`, `undefined method 'x'
for nil`). A difference is confirmed against a second Spinel run before
it is reported: a program whose output changes per run is
`nondeterministic`, not a bug in either runtime. The rules live in
`diff_normalize.rb` and the labels in `diff_classify.rb`, each with a
corpus test (`test/tools_diff_*.rb`); the end-to-end leg is
`make diff-test`.

```
spinel diff FILE.rb [--no-minimize] [--emit-issue PATH] [--timeout SEC]
                    [--ruby PATH] [--keep-tmp] [-- ARGS...]
```

Exit status: 0 same, 1 a difference (`output-diff`, `exception-diff`,
`timeout`, `nondeterministic`), 2 `compile-error` / `link-error`,
3 `crash`, 4 the tool's own error. This is a contract for CI.
`--no-minimize` is accepted ahead of the minimizer, which does not exist
yet. Not normalized on purpose: Hash order (the language defines it),
`object_id` values (indistinguishable from data) and `rand` (Spinel's
generator is not CRuby's).
