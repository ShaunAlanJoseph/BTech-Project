# GROUP2 — Classical register allocation

Owned implementation: `src/allocators.cpp`.

Required work: Loop-weighted spill costs, naive allocation, whole-interval linear scan and optimistic Chaitin–Briggs allocation.

Read `../interfaces.md` and `../testing.md` first. Shared declarations are
coordinated interfaces. Other components are developed by their owners; they
are not assumed complete. Build and run the independent targets:

```sh
cmake -S compiler -B compiler/build
cmake --build compiler/build --target allocators-tests -j 4
ctest --test-dir compiler/build -L group2 --output-on-failure
```

Run each named executable from `build/`. Group 4's shared-utility milestone enables these isolated tests.

## Algorithm and Design Note

Group 2 implements the classical register allocation suite in `src/allocators.cpp`:

1. **Loop-Weighted Spill Costs (`spillCosts`)**:
   - Calculates a static proxy for memory access costs across the CFG:
     $$\text{cost}(v) = \sum_{\text{defs}, \text{uses}} 10^{\text{loopDepth}}$$
   - Function parameters are defined at function entry boundary at loop depth 0, contributing $+1.0$.
   - Each instruction definition in a reachable block contributes $10^{\text{depth}}$.
   - Each unique register use in `Instr::uses` in a reachable block contributes $10^{\text{depth}}$. Phi operands follow `Instr::uses` as a static proxy.
   - Values not defined or used in reachable code retain cost $0.0$.

2. **Naive Allocation (`allocNaive`)**:
   - Validates that budget $k \in [1, 25]$ (throws `std::invalid_argument` otherwise).
   - Serves as the experimental worst-case memory baseline: marks every node $v \in g.\text{nodes()}$ as spilled (`assignment[v] = kSpill = -1`).
   - Every graph node is appended to `spilled`.

3. **Whole-Interval Linear-Scan Allocation (`allocLinearScan`)**:
   - Flattens reachable basic blocks in Reverse Postorder (RPO).
   - Determines conservative live intervals $[start, end]$ by assigning linear coordinate points across block entry (`liveIn`), instruction uses/defs, and block exit (`liveOut`).
   - Sorts intervals ascending by $start$, tie-breaking by $end$ and `g.nameLess(a, b)`.
   - Sweeps left-to-right through intervals:
     - Expiring active intervals whose $end < cur.start$.
     - When $|active| < k$: assigns the lowest register index in $[0, k)$ free from active conflicts and graph neighbor conflicts.
     - When $|active| = k$: compares $cur.end$ with the active interval ending furthest in the future (`active.back()`). Evicts the furthest ending interval to memory if it extends beyond $cur.end$, or spills $cur$ otherwise.

4. **Chaitin–Briggs Optimistic Graph-Colouring Allocation (`allocChaitinBriggs`)**:
   - **Simplification**: Iteratively removes nodes with $\text{degree} < k$ from the remaining subgraph and pushes them onto `selectStack`. Tie-breaks deterministically using `g.nameLess`.
   - **Optimistic Spill Selection**: When all remaining nodes have $\text{degree} \ge k$, selects the candidate minimizing Briggs' metric:
     $$\frac{\text{cost}(v)}{\text{degree}(v)}$$
     Pushes this candidate onto `selectStack` without discarding it.
   - **Select (Coloring)**: Pops nodes from `selectStack` in reverse order. For each node, inspects colors used by all adjacent nodes in $g$. If a free color in $[0, k)$ is available, assigns the lowest valid color. If all $k$ colors are exhausted by neighbors, marks the node as an actual spill (`kSpill`).

## Limitations

- **Whole-Interval Coarseness**: Linear scan uses single convex intervals $[start, end]$ per value rather than split live ranges or live interval holes across branches. While this matches the classical Poletto & Sarkar specification, it may overestimate interference across disjoint CFG branches.
- **Static Spill Cost Proxy**: Static loop depth weighting ($10^d$) approximates dynamic execution frequency without profiling feedback (PGO) or loop trip-count estimations.
- **Physical ABI Mapping & Spill Code Insertion**: Physical register assignment to RISC-V ABI registers (`a0..a7`, `s0..s11`, `t0..t4`), spill slot allocation, stack frame expansion, and reload/store instruction generation are external integration responsibilities owned by Group 4 in `backend.cpp`.

## Validation Commands and Results

Build configuration for allocator checks:

```sh
cmake -S compiler -B compiler/build -DSSARV_TEST_ALLOCATORS=ON
cmake --build compiler/build --target allocators-tests -j 4
ctest --test-dir compiler/build -L group2 --output-on-failure
```

Direct unit test coverage in `tests/test_allocators.cpp`:
- `Allocators.EmptyAndInvalidBudgets`: Validates $k \in [1, 25]$ and checks that $k \le 0$ or $k \ge 26$ throw `std::invalid_argument`.
- `Allocators.NaiveAndHeuristicsOnManuallySpecifiedDiamond`: Validates `naive`, `linear_scan`, and `chaitin_briggs` on diamond CFG across budgets $k \in \{1, 2, 6, 25\}$. Verifies zero spills at $k=25$ and proper conflict resolution at $k=1$.
- `Allocators.UniqueUsesAndLoopWeightedCosts`: Validates exact loop-weighted costs ($10^0$ and $10^1$) on straight-line and two-latch loop fixtures.
- `Allocators.NoncliqueBipartiteGraphColoursWithBudgetTwo`: Verifies optimal 2-coloring without spills on a nonclique path graph.
- `Allocators.TightPressureOddCycleForcesSpill`: Confirms that a 5-cycle graph forces a spill under tight budget $k=2$ and colors cleanly with ample budget $k=3$.
- `Allocators.LinearScanDeterminismAndIntervals`: Validates repeatable, deterministic assignments across multiple runs.
- `Allocators.IsolatedNodesWithoutEdges`: Verifies register sharing among disconnected nodes without unnecessary spills.
- `FixtureProvider.UnknownFunctionsFailInsteadOfComputingAnalysis`: Validates strict isolation between test fixtures and production analysis.
