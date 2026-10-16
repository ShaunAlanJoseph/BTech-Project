#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

#include "ssarv/allocators.hpp"
#include "support/fixtures.hpp"
#include "support/analysis_provider.hpp"

using namespace ssarv;

namespace {

// Assertions here are test invariants, independent of Group 4's production verifier.
void valid(const Function& fn, const Graph& g, const AllocResult& r, int k) {
    ASSERT_EQ(r.assignment.size(), fn.numValues());
    EXPECT_EQ(r.k, k);
    EXPECT_FALSE(r.exact);

    std::set<ValueId> spills(r.spilled.begin(), r.spilled.end());
    EXPECT_EQ(spills.size(), r.spilled.size());

    for (auto v : r.spilled) {
        ASSERT_LT(v, fn.numValues());
        EXPECT_TRUE(g.contains(v));
    }

    for (auto v : g.nodes()) {
        auto c = r.colourOf(v);
        EXPECT_TRUE(c == kSpill || (c >= 0 && c < k));
        EXPECT_EQ(spills.contains(v), c == kSpill);
        if (c >= 0) {
            g.neighbours(v).forEach([&](ValueId u) {
                EXPECT_NE(c, r.colourOf(u));
            });
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Baseline Specification Tests
// -----------------------------------------------------------------------------

TEST(Allocators, EmptyAndInvalidBudgets) {
    Function fn;
    Graph g;
    fixtures::AnalysisFixture fixture(fn, {});

    for (auto allocate : {allocNaive, allocLinearScan, allocChaitinBriggs}) {
        auto r = allocate(fn, g, 1);
        EXPECT_TRUE(r.assignment.empty());
        EXPECT_TRUE(r.spilled.empty());

        EXPECT_THROW(allocate(fn, g, 0), std::invalid_argument);
        EXPECT_THROW(allocate(fn, g, 26), std::invalid_argument);
        EXPECT_THROW(allocate(fn, g, -5), std::invalid_argument);
        EXPECT_THROW(allocate(fn, g, 100), std::invalid_argument);
    }
}

TEST(Allocators, NaiveAndHeuristicsOnManuallySpecifiedDiamond) {
    auto fn = fixtures::diamond();
    auto g = fixtures::diamondGraph(fn);
    g.setPool(&fn.values);

    fixtures::AnalysisFixture fixture(fn, fixtures::diamondAnalysis());

    for (int k : {1, 2, 6, 25}) {
        auto naive = allocNaive(fn, g, k);
        valid(fn, g, naive, k);
        EXPECT_EQ(naive.name, "naive");
        EXPECT_EQ(naive.spilled.size(), g.numNodes());

        auto linear = allocLinearScan(fn, g, k);
        auto cb = allocChaitinBriggs(fn, g, k);

        valid(fn, g, linear, k);
        valid(fn, g, cb, k);
        EXPECT_EQ(linear.name, "linear_scan");
        EXPECT_EQ(cb.name, "chaitin_briggs");

        if (k == 25) {
            EXPECT_TRUE(linear.spilled.empty());
            EXPECT_TRUE(cb.spilled.empty());
        }

        // Chaitin-Briggs must be completely deterministic.
        EXPECT_EQ(cb.assignment, allocChaitinBriggs(fn, g, k).assignment);
    }
}

TEST(Allocators, UniqueUsesAndLoopWeightedCosts) {
    auto straight = fixtures::straight();
    fixtures::AnalysisFixture a(straight, fixtures::straightAnalysis());
    EXPECT_EQ(spillCosts(straight), (std::vector<double>{2.0, 2.0}));

    auto loop = fixtures::twoLatchLoop();
    fixtures::AnalysisFixture b(loop, fixtures::loopAnalysis());
    EXPECT_EQ(spillCosts(loop), (std::vector<double>{11.0}));
}

TEST(FixtureProvider, UnknownFunctionsFailInsteadOfComputingAnalysis) {
    Function fn;
    EXPECT_THROW(computeLiveness(fn), std::logic_error);
    EXPECT_THROW(loopDepths(fn), std::logic_error);
}

// -----------------------------------------------------------------------------
// Extended Coverage: Noncliques, Tight Pressure, and Determinism
// -----------------------------------------------------------------------------

TEST(Allocators, NoncliqueBipartiteGraphColoursWithBudgetTwo) {
    // Path graph: v0 -- v1 -- v2 -- v3 (chromatic number = 2, maximum degree = 2)
    Function fn;
    fn.name = "path_fn";
    for (int i = 0; i < 4; ++i) {
        fn.values.intern("%v" + std::to_string(i));
    }

    Graph g(4);
    for (ValueId i = 0; i < 4; ++i) g.addNode(i);
    g.addEdge(0, 1);
    g.addEdge(1, 2);
    g.addEdge(2, 3);
    g.setPool(&fn.values);

    // Provide empty analysis fixture so fixture provider lookup doesn't throw.
    fixtures::AnalysisFixture fixture(fn, {});

    // With budget k = 2, Chaitin-Briggs can color the bipartite path with 0 spills.
    auto cb = allocChaitinBriggs(fn, g, 2);
    valid(fn, g, cb, 2);
    EXPECT_TRUE(cb.spilled.empty());
    EXPECT_LE(cb.coloursUsed(), 2);

    auto naive = allocNaive(fn, g, 2);
    valid(fn, g, naive, 2);
    EXPECT_EQ(naive.spilled.size(), 4u);
}

TEST(Allocators, TightPressureOddCycleForcesSpill) {
    // 5-cycle graph C5: v0 -- v1 -- v2 -- v3 -- v4 -- v0
    // Chromatic number of C5 is 3. With budget k = 2, register pressure is tight,
    // so at least one vertex must be spilled.
    Function fn;
    fn.name = "cycle_fn";
    for (int i = 0; i < 5; ++i) {
        fn.values.intern("%c" + std::to_string(i));
    }

    Graph g(5);
    for (ValueId i = 0; i < 5; ++i) g.addNode(i);
    g.addEdge(0, 1);
    g.addEdge(1, 2);
    g.addEdge(2, 3);
    g.addEdge(3, 4);
    g.addEdge(4, 0);
    g.setPool(&fn.values);

    fixtures::AnalysisFixture fixture(fn, {});

    // Budget k = 2: spilling is required.
    auto cb2 = allocChaitinBriggs(fn, g, 2);
    valid(fn, g, cb2, 2);
    EXPECT_GE(cb2.spilled.size(), 1u);

    // Budget k = 3: ample budget (3 colors suffice for an odd cycle); 0 spills.
    auto cb3 = allocChaitinBriggs(fn, g, 3);
    valid(fn, g, cb3, 3);
    EXPECT_TRUE(cb3.spilled.empty());
    EXPECT_LE(cb3.coloursUsed(), 3);
}

TEST(Allocators, LinearScanDeterminismAndIntervals) {
    auto fn = fixtures::diamond();
    auto g = fixtures::diamondGraph(fn);
    g.setPool(&fn.values);

    fixtures::AnalysisFixture fixture(fn, fixtures::diamondAnalysis());

    for (int k = 1; k <= 5; ++k) {
        auto run1 = allocLinearScan(fn, g, k);
        auto run2 = allocLinearScan(fn, g, k);
        valid(fn, g, run1, k);
        valid(fn, g, run2, k);
        EXPECT_EQ(run1.assignment, run2.assignment);
        EXPECT_EQ(run1.spilled, run2.spilled);
    }
}

TEST(Allocators, IsolatedNodesWithoutEdges) {
    // Graph containing 3 isolated nodes (no conflict edges).
    Function fn;
    fn.name = "isolated_fn";
    for (int i = 0; i < 3; ++i) {
        fn.values.intern("%i" + std::to_string(i));
    }

    Graph g(3);
    for (ValueId i = 0; i < 3; ++i) g.addNode(i);
    g.setPool(&fn.values);

    fixtures::AnalysisFixture fixture(fn, {});

    // Even with k = 1, all 3 isolated nodes can share the same register 0.
    auto cb = allocChaitinBriggs(fn, g, 1);
    valid(fn, g, cb, 1);
    EXPECT_TRUE(cb.spilled.empty());
    for (ValueId v : g.nodes()) {
        EXPECT_EQ(cb.colourOf(v), 0);
    }
}
