#include "ssarv/allocators.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace ssarv {

// -----------------------------------------------------------------------------
// 1. Loop-Weighted Spill Costs
// -----------------------------------------------------------------------------

/// Computes static loop-weighted spill costs for all values in the function.
///
/// In register allocation, spilling a value inside a deeply nested loop incurs
/// orders of magnitude more runtime memory traffic than spilling a value in
/// straight-line code. We approximate execution frequency using 10^(loopDepth):
/// - Function parameters receive 1.0 (defined at function entry, depth 0: 10^0 = 1).
/// - Every instruction definition in a reachable block contributes 10^(depth).
/// - Every unique register use in Instr::uses contributes 10^(depth).
/// - Phi uses follow Instr::uses as a static proxy.
std::vector<double> spillCosts(const Function& fn) {
    std::vector<double> costs(fn.numValues(), 0.0);
    if (fn.numValues() == 0) {
        return costs;
    }

    // Function parameters are defined once at the function entry boundary.
    for (ValueId p : fn.params) {
        if (p < costs.size()) {
            costs[p] += 1.0;
        }
    }

    // If there are no basic blocks, there are no instructions to traverse.
    if (fn.blocks.empty()) {
        return costs;
    }

    // Query analysis fixtures or production analysis for loop depths and reachability.
    const auto depths = loopDepths(fn);
    const auto reachable = reachableBlocks(fn);

    for (BlockId b : reachable) {
        if (b >= fn.blocks.size()) {
            continue;
        }

        // Depth 0 corresponds to a 10^0 = 1x execution factor.
        int depth = (b < depths.size()) ? depths[b] : 0;
        double weight = std::pow(10.0, std::max(0, depth));

        const auto& blk = fn.blocks[b];
        for (const auto& in : blk.instrs) {
            // Instruction definition cost: writing a new value to a register/stack slot.
            if (in.dst != kNoValue && in.dst < costs.size()) {
                costs[in.dst] += weight;
            }

            // Instruction operand uses: reading values. Instr::uses already contains
            // unique register operands for each instruction (e.g. `add %x, %x` lists %x once).
            for (ValueId u : in.uses) {
                if (u != kNoValue && u < costs.size()) {
                    costs[u] += weight;
                }
            }
        }
    }

    return costs;
}

// -----------------------------------------------------------------------------
// 2. Naive Allocation
// -----------------------------------------------------------------------------

/// Naive register allocator:
/// Spills every value that is part of the interference graph. No hardware
/// registers are assigned. This establishes a baseline representing the maximum
/// spill cost and memory traffic for comparison in evaluation benchmarks.
AllocResult allocNaive(const Function& fn, const Graph& g, int k) {
    if (k < 1 || k > 25) {
        throw std::invalid_argument("Register budget k must be in [1, 25]");
    }

    AllocResult res;
    res.name = "naive";
    res.k = k;
    res.exact = false;
    res.ops = 0;
    res.assignment.assign(fn.numValues(), kUnassigned);

    // Every node present in the interference graph is spilled to memory.
    for (ValueId v : g.nodes()) {
        res.assignment[v] = kSpill;
        res.spilled.push_back(v);
        res.ops++;
    }

    return res;
}

// -----------------------------------------------------------------------------
// 3. Whole-Interval Linear-Scan Allocation
// -----------------------------------------------------------------------------

namespace {

struct LiveInterval {
    ValueId v = kNoValue;
    int start = 0;
    int end = 0;
    double cost = 0.0;
};

} // namespace

/// Classical Linear-Scan Register Allocator (Poletto & Sarkar 1999).
///
/// Flattens the control flow graph into a 1-dimensional order using Reverse
/// Postorder (RPO). Computes a single conservative [start, end] live interval
/// for each value based on RPO instruction positions and block liveness.
/// Sweeps left-to-right through start points:
/// 1. Expire intervals whose end point has passed.
/// 2. If available registers exist, assign the smallest free register index.
/// 3. If pressure exceeds budget k, spill the interval that extends furthest into
///    the future (or current interval if it ends furthest).
AllocResult allocLinearScan(const Function& fn, const Graph& g, int k) {
    if (k < 1 || k > 25) {
        throw std::invalid_argument("Register budget k must be in [1, 25]");
    }

    AllocResult res;
    res.name = "linear_scan";
    res.k = k;
    res.exact = false;
    res.ops = 0;
    res.assignment.assign(fn.numValues(), kUnassigned);

    if (g.numNodes() == 0 || fn.numValues() == 0) {
        return res;
    }

    // Step 1: Compute live intervals across reachable blocks in reverse postorder.
    std::vector<int> firstPoint(fn.numValues(), std::numeric_limits<int>::max());
    std::vector<int> lastPoint(fn.numValues(), std::numeric_limits<int>::min());

    // Parameters are defined at the very beginning of the function (point 0).
    for (ValueId p : fn.params) {
        if (p < fn.numValues()) {
            firstPoint[p] = std::min(firstPoint[p], 0);
            lastPoint[p] = std::max(lastPoint[p], 0);
        }
    }

    if (!fn.blocks.empty()) {
        const auto rpo = reversePostorder(fn);
        const auto liveness = computeLiveness(fn);

        int pointCounter = 1;
        for (BlockId b : rpo) {
            if (b >= fn.blocks.size()) {
                continue;
            }

            // Values live at the entry of the block span from this block's start.
            int blockStart = pointCounter++;
            if (b < liveness.liveIn.size()) {
                liveness.liveIn[b].forEach([&](ValueId v) {
                    if (v < fn.numValues()) {
                        firstPoint[v] = std::min(firstPoint[v], blockStart);
                        lastPoint[v] = std::max(lastPoint[v], blockStart);
                    }
                });
            }

            // Walk instructions in the block. We separate read and def sub-points
            // so an operand dying at this instruction does not conflict with its result.
            for (const auto& in : fn.blocks[b].instrs) {
                int usePoint = pointCounter++;
                for (ValueId u : in.uses) {
                    if (u < fn.numValues()) {
                        firstPoint[u] = std::min(firstPoint[u], usePoint);
                        lastPoint[u] = std::max(lastPoint[u], usePoint);
                    }
                }

                int defPoint = pointCounter++;
                if (in.dst != kNoValue && in.dst < fn.numValues()) {
                    firstPoint[in.dst] = std::min(firstPoint[in.dst], defPoint);
                    lastPoint[in.dst] = std::max(lastPoint[in.dst], defPoint);
                }
            }

            // Values live at the exit of the block span to this block's end.
            int blockEnd = pointCounter++;
            if (b < liveness.liveOut.size()) {
                liveness.liveOut[b].forEach([&](ValueId v) {
                    if (v < fn.numValues()) {
                        firstPoint[v] = std::min(firstPoint[v], blockStart);
                        lastPoint[v] = std::max(lastPoint[v], blockEnd);
                    }
                });
            }
        }
    }

    const auto costs = spillCosts(fn);

    // Assemble intervals for all nodes present in the interference graph.
    std::vector<LiveInterval> intervals;
    intervals.reserve(g.numNodes());
    for (ValueId v : g.nodes()) {
        int s = firstPoint[v];
        int e = lastPoint[v];
        if (s > e) {
            // Unused or disconnected node defaults to a point-interval.
            s = 0;
            e = 0;
        }
        intervals.push_back({v, s, e, v < costs.size() ? costs[v] : 1.0});
    }

    // Sort intervals by start point ascending. Tie-break deterministically using
    // end point, followed by the graph's name/id comparison rule.
    std::sort(intervals.begin(), intervals.end(), [&](const LiveInterval& a, const LiveInterval& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.end != b.end) return a.end < b.end;
        return g.nameLess(a.v, b.v);
    });

    // Step 2: Linear sweep.
    // `active` holds currently assigned intervals, ordered by end point ascending.
    std::vector<LiveInterval> active;
    active.reserve(k);

    for (const auto& cur : intervals) {
        res.ops++;

        // 2a. Expire old intervals that have ended before `cur.start`.
        auto expireIt = std::remove_if(active.begin(), active.end(), [&](const LiveInterval& act) {
            return act.end < cur.start;
        });
        active.erase(expireIt, active.end());

        // 2b. Check if we have capacity in the active set.
        if (static_cast<int>(active.size()) < k) {
            // Find the lowest free register in [0, k) not used by active intervals,
            // while additionally ensuring no conflict with already-colored graph neighbors.
            std::vector<bool> regUsed(k, false);
            for (const auto& act : active) {
                int r = res.assignment[act.v];
                if (r >= 0 && r < k) regUsed[r] = true;
            }
            g.neighbours(cur.v).forEach([&](ValueId nbr) {
                int r = res.colourOf(nbr);
                if (r >= 0 && r < k) regUsed[r] = true;
            });

            int chosenReg = -1;
            for (int r = 0; r < k; ++r) {
                if (!regUsed[r]) {
                    chosenReg = r;
                    break;
                }
            }

            if (chosenReg != -1) {
                res.assignment[cur.v] = chosenReg;
                active.push_back(cur);
                std::sort(active.begin(), active.end(), [](const LiveInterval& a, const LiveInterval& b) {
                    return a.end < b.end;
                });
            } else {
                // Rare case where graph interference exceeds linear interval capacity.
                res.assignment[cur.v] = kSpill;
                res.spilled.push_back(cur.v);
            }
        } else {
            // 2c. Spill handling: all k registers are currently occupied.
            // Classical Poletto & Sarkar heuristic: compare current with the active
            // interval that ends furthest in the future (`active.back()`).
            auto& furthestActive = active.back();
            if (furthestActive.end > cur.end) {
                // Evict `furthestActive` to memory and assign its register to `cur`.
                ValueId victim = furthestActive.v;
                active.pop_back();

                std::vector<bool> regUsed(k, false);
                for (const auto& act : active) {
                    int r = res.assignment[act.v];
                    if (r >= 0 && r < k) regUsed[r] = true;
                }
                g.neighbours(cur.v).forEach([&](ValueId nbr) {
                    int r = res.colourOf(nbr);
                    if (r >= 0 && r < k) regUsed[r] = true;
                });

                int chosenReg = -1;
                for (int r = 0; r < k; ++r) {
                    if (!regUsed[r]) {
                        chosenReg = r;
                        break;
                    }
                }

                if (chosenReg != -1) {
                    res.assignment[victim] = kSpill;
                    res.spilled.push_back(victim);

                    res.assignment[cur.v] = chosenReg;
                    active.push_back(cur);
                    std::sort(active.begin(), active.end(), [](const LiveInterval& a, const LiveInterval& b) {
                        return a.end < b.end;
                    });
                } else {
                    // Cannot assign cur; spill cur instead.
                    res.assignment[cur.v] = kSpill;
                    res.spilled.push_back(cur.v);
                }
            } else {
                // `cur` ends furthest; spill `cur`.
                res.assignment[cur.v] = kSpill;
                res.spilled.push_back(cur.v);
            }
        }
    }

    // Sort spilled list for determinism.
    std::sort(res.spilled.begin(), res.spilled.end());
    return res;
}

// -----------------------------------------------------------------------------
// 4. Chaitin–Briggs Optimistic Graph-Colouring Allocation
// -----------------------------------------------------------------------------

/// Chaitin–Briggs Optimistic Graph-Colouring Allocator.
///
/// Briggs et al. improved on Chaitin's allocator by delaying the spill decision.
/// Rather than immediately discarding high-degree nodes as spills during simplification,
/// we push them onto the stack optimistically. When popped during the coloring
/// phase, if their conflicting neighbors happened to receive overlapping colors or
/// were themselves spilled, an actual color may still be found for the node!
///
/// Phases:
/// 1. Simplify:
///    Repeatedly remove nodes with degree < k from the interference graph and push
///    to the coloring stack.
/// 2. Optimistic Spill Selection:
///    When all remaining nodes have degree >= k, choose a spill candidate that
///    minimizes (cost / degree) and push it optimistically onto the stack.
/// 3. Select (Coloring):
///    Pop nodes from the stack in reverse order and assign the lowest valid color
///    in [0, k) that is not used by any neighbor. If no color is free, the node
///    becomes an actual spill.
AllocResult allocChaitinBriggs(const Function& fn, const Graph& g, int k) {
    if (k < 1 || k > 25) {
        throw std::invalid_argument("Register budget k must be in [1, 25]");
    }

    AllocResult res;
    res.name = "chaitin_briggs";
    res.k = k;
    res.exact = false;
    res.ops = 0;
    res.assignment.assign(fn.numValues(), kUnassigned);

    if (g.numNodes() == 0 || fn.numValues() == 0) {
        return res;
    }

    const auto costs = spillCosts(fn);

    // Track active nodes in the subgraph during simplification.
    BitSet activeNodes(g.capacity());
    std::vector<int> currentDegree(g.capacity(), 0);

    for (ValueId v : g.nodes()) {
        activeNodes.set(v);
    }

    // Compute initial degrees within the active node set.
    for (ValueId v : g.nodes()) {
        int d = 0;
        g.neighbours(v).forEach([&](ValueId nbr) {
            if (activeNodes.test(nbr)) d++;
        });
        currentDegree[v] = d;
    }

    std::vector<ValueId> selectStack;
    selectStack.reserve(g.numNodes());
    std::size_t remainingNodes = g.numNodes();

    // -------------------------------------------------------------------------
    // Phase 1 & 2: Simplify and Optimistic Spill Selection
    // -------------------------------------------------------------------------
    while (remainingNodes > 0) {
        res.ops++;

        // Find a node with degree < k.
        // We pick the smallest according to g.nameLess to maintain strict determinism.
        ValueId simplifyCand = kNoValue;
        activeNodes.forEach([&](ValueId v) {
            if (currentDegree[v] < k) {
                if (simplifyCand == kNoValue || g.nameLess(v, simplifyCand)) {
                    simplifyCand = v;
                }
            }
        });

        if (simplifyCand != kNoValue) {
            // Simplify: degree < k, guaranteed to be colorable once neighbors are colored.
            ValueId v = simplifyCand;
            activeNodes.reset(v);
            remainingNodes--;
            selectStack.push_back(v);

            g.neighbours(v).forEach([&](ValueId nbr) {
                if (activeNodes.test(nbr)) {
                    currentDegree[nbr]--;
                }
            });
        } else {
            // Optimistic Spill: all active nodes have degree >= k.
            // Briggs' heuristic: minimize (spill_cost / degree).
            ValueId bestSpill = kNoValue;
            double bestMetric = std::numeric_limits<double>::infinity();

            activeNodes.forEach([&](ValueId v) {
                double c = (v < costs.size()) ? costs[v] : 1.0;
                double deg = static_cast<double>(std::max(1, currentDegree[v]));
                double metric = c / deg;

                if (metric < bestMetric) {
                    bestMetric = metric;
                    bestSpill = v;
                } else if (std::abs(metric - bestMetric) < 1e-9) {
                    // Deterministic tie-breaking on identical metrics.
                    if (bestSpill == kNoValue || g.nameLess(v, bestSpill)) {
                        bestSpill = v;
                    }
                }
            });

            ValueId v = bestSpill;
            activeNodes.reset(v);
            remainingNodes--;
            selectStack.push_back(v);

            g.neighbours(v).forEach([&](ValueId nbr) {
                if (activeNodes.test(nbr)) {
                    currentDegree[nbr]--;
                }
            });
        }
    }

    // -------------------------------------------------------------------------
    // Phase 3: Select (Coloring)
    // -------------------------------------------------------------------------
    // Reconstruct the graph by popping nodes from the stack in reverse order.
    while (!selectStack.empty()) {
        ValueId v = selectStack.back();
        selectStack.pop_back();
        res.ops++;

        // Identify which colors are already taken by colored neighbors in the original graph.
        std::vector<bool> colorUsed(k, false);
        g.neighbours(v).forEach([&](ValueId nbr) {
            int c = res.colourOf(nbr);
            if (c >= 0 && c < k) {
                colorUsed[c] = true;
            }
        });

        // Find the lowest available color in [0, k).
        int assignedColor = -1;
        for (int c = 0; c < k; ++c) {
            if (!colorUsed[c]) {
                assignedColor = c;
                break;
            }
        }

        if (assignedColor != -1) {
            res.assignment[v] = assignedColor;
        } else {
            // No color available in budget k: this is an actual spill.
            res.assignment[v] = kSpill;
            res.spilled.push_back(v);
        }
    }

    // Sort spilled list for deterministic output order.
    std::sort(res.spilled.begin(), res.spilled.end());
    return res;
}

} // namespace ssarv
