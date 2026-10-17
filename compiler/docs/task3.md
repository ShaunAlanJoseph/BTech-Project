# Task 3 — Phi Elimination and Basic RISC-V Lowering

## Role in the pipeline

Task3 contributes `src/phielim.cpp` and `src/lowering.cpp`. It receives the shared
SSA `Function` representation (`ir.hpp`), its resolved CFG, and—at assembly
time—the allocation/spill services exposed by `LoweringContext`. It produces a
phi-free executable CFG and RISC-V instruction sequences through that context.

The executable pipeline transforms phis before register allocation. This makes
the inserted copies and any cycle temporary visible to the existing allocator.
The allocator still receives the original SSA graph for the project's separate
register-allocation experiments. Group 3 never chooses a physical register,
creates a spill slot, changes `sp`, or emits prologue/epilogue code.

## Phi transformation

`splitCriticalEdges` snapshots the original CFG edges, then splits every edge
whose source has multiple successors and target has multiple predecessors. A
fresh bridge block contains `br label %target`; it replaces the original
predecessor in the target's predecessor list and every matching phi argument.
Terminator successors, switch-arm targets, labels, `labelToBlock`, and
reciprocal predecessor/successor lists are updated together. Snapshotting
prevents a newly created bridge edge from being reconsidered indefinitely.

`eliminatePhis` performs that split first. For every reachable phi block it
collects assignments by incoming edge, puts their moves immediately before the
edge block's terminator, and finally removes the phis. Thus a diamond such as

```llvm
left:  br label %join
right: br label %join
join:
  %p = phi i32 [ %a, %left ], [ %b, %right ]
```

becomes a `mov i32 %p, %a` on `left -> join` and a `mov i32 %p, %b` on
`right -> join`; no copy is executed on the non-selected path. A critical
incoming edge receives the copy in its bridge block instead.

`sequentialise` removes identity copies and repeatedly emits an assignment
whose destination is no longer needed as an old source. On a cycle, it emits a
fresh temporary save before continuing. For `{a <- b, b <- a}`, the schedule
is `{tmp <- b, b <- a, a <- tmp}`. Literal assignments are emitted after the
register schedule, preserving a register's old value when another assignment
reads it. Generated copies retain the phi's integer type, including cycle
temporaries.

## RISC-V lowering

The lowering target is the shared RV64IM backend contract. It supports integer
widths `i1`…`i64` and calls only these `LoweringContext` services:

- `operand` materialises an assigned register, spill reload, or literal.
- `destination` and `storeResult` handle allocated and spilled results.
- `blockLabel`, `epilogueLabel`, and `freshLabel` make branch destinations.
- `emit` records the assembly sequence.

Arithmetic (`add`, `sub`, `mul`, `sdiv`, `udiv`, `srem`, `urem`), bitwise
operations, and shifts use RV64 register instructions. Narrow results are
canonicalised with `slli`/`srai`; `i1` uses `andi ..., 1`. Unsigned division,
remainder, and comparisons zero-extend the input bit patterns first. Signed
comparisons use `slt`; unsigned comparisons use `sltu` and inversion for the
inclusive predicates. All ten LLVM integer comparison predicates produce
`0`/`1`.

`trunc`, `zext`, `sext`, and equal-width integer `bitcast` retain LLVM bit
semantics, including truncation to the low `i1` bit and sign-extending an `i1`
true value as `-1`. `select` uses a branch diamond instead of requiring a third
scratch register. `br`, `switch`, `ret`, and `unreachable` lower to explicit
labels/jumps (`ret` jumps to the Group 4 epilogue). Unsupported instructions or
malformed lowering inputs throw `std::invalid_argument` with an operation-
specific message.

## Tests and validation

The Group 3 unit tests cover parallel copy identities, random cyclic copy
assignments, long cycles, two-input diamonds, multiple loop phis, typed
generated moves, switch-edge splitting, arithmetic/bitwise/shift/division
lowering, every signed and unsigned comparison predicate, casts, selects,
branches, switches, returns, `unreachable`, destination aliasing, literals,
and unsupported opcode diagnostics.

Build and run the isolated component checks from `compiler/`:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSSARV_TEST_PHI=ON -DSSARV_TEST_LOWERING=ON
cmake --build build --target phi-tests lowering-tests -j 4
ctest --test-dir build -L group3 --output-on-failure
```

The complete end-to-end assembler/linker/emulator test remains an integration
responsibility: the current Group 4 parser, backend, driver, verifier, and
integration test are still stubs. The local development environment used for
this change also has no CMake/CTest installation and only GCC 6.3, which does
not support the project's required C++23 mode. Consequently the commands above
are documented but have not been reported as passing from this workspace.

## Group 4 boundary and limitations

Group 4 maps allocation colours to the ABI register order, reloads/stores
spills, owns memory/calls/globals/frame management, validates supported input,
and drives whole-function emission. Group 3 assumes its two reserved scratch
registers (`t5`/`t6`) are not allocated to program values, as the shared
backend contract specifies. It does not lower floating point, vectors,
integers wider than 64 bits, memory instructions, calls, exception
terminators, or unsupported casts.

Files changed for this task:

- `src/phielim.cpp` — critical-edge splitting, edge copies, parallel-copy
  scheduling, and phi removal.
- `src/lowering.cpp` — RV64IM integer/control-flow lowering through the shared
  context.
- `tests/test_phi.cpp` and `tests/test_lowering.cpp` — Group 3 unit coverage.
- `README.md` and this design note — scope, usage, integration contract, and
  verification status.
