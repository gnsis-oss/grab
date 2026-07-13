#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/ui.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <concepts>
#include <cstdint>
#include <variant>
// clang-format on

namespace
{

    constexpr std::uint32_t treeId                     = 1U;
    constexpr std::uint32_t generationIncrement        = 1U;
    constexpr std::uint32_t runtimeIdIncrement         = 1U;
    constexpr std::uint32_t initialEpoch               = 1U;
    constexpr std::uint64_t initialRevision            = 1U;
    constexpr std::uint64_t sequentialRevision         = 2U;
    constexpr std::uint64_t outOfOrderRevision         = 3U;
    constexpr std::uint64_t partialCommitRevision      = 4U;
    constexpr std::uint64_t simulatedOverflowDropCount = 7U;

    static_assert( std::derived_from<grab::testing::FakeRuntime,
                                     grab::spi::Runtime> );

    class FakeRuntimeTest : public ::testing::Test
    {
        protected:

            void
            SetUp() override
            {
                ASSERT_TRUE( runtime.start( context ).has_value() );
            }

            // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
            grab::OperationContext context{
                .deadline = grab::Deadline::unbounded(),
            };
            grab::testing::FakeRuntime
                runtime;    // NOLINT(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
    };

}    // namespace

TEST_F( FakeRuntimeTest,
        RestartBumpsGenerationAndRuntimeIdentity )
{
    const auto oldGeneration = runtime.generation();
    const auto oldRuntime    = runtime.runtime_id();
    const auto oldRef        = runtime.add_node();

    runtime.restart();

    EXPECT_EQ( runtime.generation(), oldGeneration + generationIncrement );
    EXPECT_EQ( runtime.runtime_id().value, oldRuntime.value + runtimeIdIncrement );

    const auto resolved = runtime.resolve( oldRef );
    ASSERT_FALSE( resolved.has_value() );
    EXPECT_EQ( resolved.error().code, grab::ErrorCode::RuntimeRestarted );
}

TEST_F( FakeRuntimeTest,
        ScriptPreservesInjectedOutOfOrderDeltaOrder )
{
    grab::UiSnapshot snapshot;
    snapshot.runtime  = runtime.runtime_id();
    snapshot.tree     = treeId;
    snapshot.epoch    = grab::TreeEpoch{ initialEpoch };
    snapshot.revision = initialRevision;
    snapshot.complete = true;
    const grab::spi::UiDelta outOfOrder{
        .runtime       = runtime.runtime_id(),
        .tree          = treeId,
        .epoch         = grab::TreeEpoch{ initialEpoch },
        .base_revision = sequentialRevision,
        .revision      = outOfOrderRevision,
        .complete      = true,
    };
    const grab::spi::UiDelta sequential{
        .runtime       = runtime.runtime_id(),
        .tree          = treeId,
        .epoch         = grab::TreeEpoch{ initialEpoch },
        .base_revision = initialRevision,
        .revision      = sequentialRevision,
        .complete      = true,
    };

    runtime.inject_snapshot( snapshot );
    runtime.inject_delta( outOfOrder );
    runtime.inject_delta( sequential );

    auto* const source = runtime.tree_source();
    ASSERT_NE( source, nullptr );

    const auto first = source->next_update( context );
    ASSERT_TRUE( first.has_value() );
    ASSERT_TRUE( first->has_value() );
    EXPECT_TRUE( std::holds_alternative<grab::UiSnapshot>( first->value().payload ) );

    const auto second = source->next_update( context );
    ASSERT_TRUE( second.has_value() );
    ASSERT_TRUE( second->has_value() );
    const auto* const secondDelta =
        std::get_if<grab::spi::UiDelta>( &second->value().payload );
    ASSERT_NE( secondDelta, nullptr );
    EXPECT_EQ( secondDelta->base_revision, sequentialRevision );
    EXPECT_EQ( secondDelta->revision, outOfOrderRevision );

    const auto third = source->next_update( context );
    ASSERT_TRUE( third.has_value() );
    ASSERT_TRUE( third->has_value() );
    const auto* const thirdDelta =
        std::get_if<grab::spi::UiDelta>( &third->value().payload );
    ASSERT_NE( thirdDelta, nullptr );
    EXPECT_EQ( thirdDelta->base_revision, initialRevision );
    EXPECT_EQ( thirdDelta->revision, sequentialRevision );
}

TEST_F( FakeRuntimeTest,
        OverflowProducesExplicitTreeGap )
{
    runtime.inject_overflow( simulatedOverflowDropCount );

    auto* const source = runtime.tree_source();
    ASSERT_NE( source, nullptr );
    const auto next = source->next_update( context );
    ASSERT_TRUE( next.has_value() );
    ASSERT_TRUE( next->has_value() );

    const auto* const gap = std::get_if<grab::spi::TreeGap>( &next->value().payload );
    ASSERT_NE( gap, nullptr );
    EXPECT_EQ( gap->dropped, simulatedOverflowDropCount );
}

TEST_F( FakeRuntimeTest,
        PartialCommitIsReportedAsTypedUncertainty )
{
    grab::UiSnapshot authoritativeAfter;
    authoritativeAfter.runtime  = runtime.runtime_id();
    authoritativeAfter.tree     = treeId;
    authoritativeAfter.epoch    = grab::TreeEpoch{ initialEpoch };
    authoritativeAfter.revision = partialCommitRevision;
    authoritativeAfter.complete = true;
    runtime.inject_partial_commit( authoritativeAfter );

    auto* const source = runtime.tree_source();
    ASSERT_NE( source, nullptr );
    const auto next = source->next_update( context );

    ASSERT_FALSE( next.has_value() );
    EXPECT_EQ( next.error().code, grab::ErrorCode::PossiblyCommitted );
}
