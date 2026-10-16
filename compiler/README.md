# SSA register allocation and RV64IM compiler

Team development repository for an LLVM IR subset parser, SSA analysis,
register allocation experiments, phi elimination and RISC-V emission.
Clang supplies source parsing and SSA construction.

Read [interfaces](docs/interfaces.md), [milestones](docs/milestones.md),
[testing](docs/testing.md), and the task descriptions in `docs/tasks/`.
Unimplemented components report a clear TODO until their owner completes the
agreed interface. The executable will remain incomplete until orchestration
and the remaining compiler components are integrated.

Current milestone: shared utilities and Group 1 analysis are integrated; Group
3's phi transformation and lowering implementation is developed on `task3`.
The Group 2 allocator and Group 4 parser/backend/driver integration remain
in progress.

## Build

Requires CMake >= 3.24, a C++23 compiler/library with `<expected>`, and
GoogleTest when testing is enabled. No dependencies are downloaded by CMake.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Acceptance checks for unfinished components are listed as disabled; this
means pending implementation, not a passing compiler. Run the component
executables directly while developing, or enable their CMake switches.

## Components

- Group 1: program analysis.
- Group 2: classical register allocation.
- Group 3: phi elimination and basic lowering.
- Group 4: shared utilities, parser, advanced algorithms,
  remaining backend, orchestration, integration and evaluation.

Components use common headers. Allocator unit tests use explicit analysis
fixtures instead of the production analysis library. Phi and lowering tests
construct IR directly, without requiring a parser or backend.

## Task Group 2 — Classical Register Allocation

Task Group 2 implements strategies for assigning values to registers and selecting
values to spill to memory, evaluating their behaviour under register pressure across
allocatable register budgets $k \in [1, 25]$:

- `spillCosts`: Computes static loop-weighted spill costs based on $10^{\text{loopDepth}}$
  scaling for definitions and operand reads, with $1.0$ for parameter entry definitions.
- `allocNaive`: Worst-case baseline allocator that spills every node in the interference graph.
- `allocLinearScan`: Whole-interval linear-scan allocator sweeping sorted RPO live intervals
  and evicting the furthest-ending interval on register pressure overflow.
- `allocChaitinBriggs`: Optimistic graph-colouring allocator with Briggs' simplification stack
  and cost/degree spill candidate selection.

Run its isolated acceptance tests with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DSSARV_TEST_ALLOCATORS=ON
cmake --build build --target allocators-tests -j 4
ctest --test-dir build -L group2 --output-on-failure
```

## Task Group 3 — Phi Elimination and Basic RISC-V Lowering

Task Group 3 turns executable SSA edges into ordinary copies and lowers the
integer/control-flow subset through the backend's `LoweringContext`. It is the
bridge between the shared SSA representation and Group 4's final RV64IM
emitter: it does not own register assignment, spill slots, stack frames,
memory operations, calls, or assembly-file emission.

`eliminatePhis` accepts a resolved `Function` and Group 1 reachability result.
It splits critical CFG edges, updates reciprocal CFG/phi metadata, inserts
edge-specific `mov` instructions, resolves parallel copies (using a fresh
virtual temporary for cycles), and removes reachable phi nodes. The temporary
is interned in the existing `ValuePool`, so the normal allocation pipeline can
assign it a register or spill location. The transformation must run before
allocating an executable function; the project may still allocate the original
SSA graph separately for research/evaluation.

Lowering supports integer widths `i1` through `i64`; arithmetic, bitwise,
shift, signed/unsigned divide and remainder; all integer `icmp` predicates;
`trunc`, `zext`, `sext`, integer `bitcast`; `select`; `mov`; and `br`,
`switch`, `ret`, and `unreachable`. It uses only the Group 4-reserved `t5` and
`t6` reload scratches, commits results through `storeResult`, and canonicalises
narrow values so ordinary widths are sign-extended while `i1` is `0`/`1`.
Unsupported opcodes raise a clear diagnostic instead of emitting a substitute.

For example, an `add i32 %x, 1` lowers to materialising `1` when necessary,
an `add`, then `slli`/`srai` to retain an `i32` result in an RV64 register.
An unsigned comparison first zero-extends its low-width operands before using
`sltu`. A `select` is emitted as a small branch diamond, which is safe even if
all three inputs require reload scratches.

The component design, copy-scheduling example, Group 4 boundary, test matrix,
and known integration limitations are in [the Task Group 3 design note](docs/task3.md).

Run its isolated acceptance tests with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DSSARV_TEST_PHI=ON -DSSARV_TEST_LOWERING=ON
cmake --build build --target phi-tests lowering-tests -j 4
ctest --test-dir build -L group3 --output-on-failure
```
