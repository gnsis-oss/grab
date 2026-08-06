// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the contract
// unit can replace this file without touching a shared build file.

#include "grab/sequence_types.hpp"
#include "kernel/graph/topological_order.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    using Graph = grab::kernel::AdjacencyGraph<grab::sequence::StepId,
                                               grab::sequence::DependencyEdge>;

    TEST( Placeholder,
          TopologicalOrderCompiles )
    {
        const Graph graph;
        const auto  order = grab::kernel::topological_order( graph );
        ASSERT_TRUE( order.has_value() );
        EXPECT_TRUE( order->empty() );
    }

}    // namespace
