#include "fake/fake_runtime.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "kernel/action/transaction.hpp"
#include "spi/route.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t treeId           = 1U;
    constexpr std::uint64_t snapshotRevision = 17U;

    [[nodiscard]]
    grab::UiNodeRecord
    control( std::uint64_t   id,
             grab::RuntimeId runtime,
             bool            enabled = true )
    {
        std::uint32_t states = grab::state_mask( grab::NodeState::Visible );
        if( enabled )
        {
            states |= grab::NodeState::Enabled;
        }
        return grab::UiNodeRecord{
            grab::NodeId{id                          },
            grab::NodeGeneration{                1U },
            grab::role::control,
            states,
            {                  },
            grab::UiProvenance{
                         .runtime  = runtime,.revision = snapshotRevision,
                         },
        };
    }

    [[nodiscard]]
    grab::UiSnapshot
    snapshot( grab::RuntimeId runtime,
              std::size_t     control_count = 1U,
              bool            enabled       = true )
    {
        std::vector<grab::UiNodeRecord> nodes;
        std::vector<grab::NodeId>       roots;
        nodes.reserve( control_count );
        roots.reserve( control_count );
        for( std::size_t index = 0U; index < control_count; ++index )
        {
            const auto node = static_cast<std::uint64_t>( index + 1U );
            nodes.push_back( control( node, runtime, enabled ) );
            roots.push_back( grab::NodeId{ node } );
        }
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtime,
                .tree     = treeId,
                .epoch    = grab::TreeEpoch{ 1U },
                .revision = snapshotRevision,
                .complete = true,
            },
            std::move( nodes ),
            std::move( roots ),
            {}
        );
    }

    [[nodiscard]]
    grab::UiSnapshot
    window_snapshot( grab::RuntimeId runtime )
    {
        std::vector<grab::UiNodeRecord> nodes;
        nodes.push_back( grab::UiNodeRecord{
            grab::NodeId{1U                          },
            grab::NodeGeneration{                1U },
            grab::role::window,
            grab::state_mask( grab::NodeState::Visible ) | grab::NodeState::Enabled,
            {                  },
            grab::UiProvenance{
                         .runtime  = runtime,.revision = snapshotRevision,
                         },
        } );
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtime,
                .tree     = treeId,
                .epoch    = grab::TreeEpoch{ 1U },
                .revision = snapshotRevision,
                .complete = true,
            },
            std::move( nodes ),
            std::vector<grab::NodeId>{ grab::NodeId{ 1U } },
            {}
        );
    }

    [[nodiscard]]
    grab::Action
    click_control()
    {
        return grab::Click{
            .target = grab::sel::role( grab::role::control ),
        };
    }

    [[nodiscard]]
    grab::ActionOptions
    forced_options()
    {
        return grab::ActionOptions{
            .deadline = std::chrono::seconds{ 1 },
            .force    = true,
        };
    }

}    // namespace

TEST( Receipt,
      UsesCanonicalFailureAndNeutralizationDefaults )
{
    const grab::Receipt receipt;

    EXPECT_EQ( receipt.commit, grab::CommitStatus::FailedBeforeCommit );
    EXPECT_EQ( receipt.neutralization, grab::NeutralizationOutcome::NotAttempted );
    EXPECT_FALSE( receipt.forced );
    EXPECT_FALSE( receipt.fallback_used );
    EXPECT_EQ( receipt.retry_class, grab::RetryClass::Never );
}

TEST( ActionTransaction,
      ExactlyOneFailureShortCircuitsBeforeCommitAndNeutralizes )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id(), 2U ) );
    auto& route = runtime.add_route( "semantic.invoke", grab::spi::RouteKind::Semantic );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const auto outcome = transaction.perform( click_control(), forced_options() );

    ASSERT_TRUE( outcome.error.has_value() );
    EXPECT_EQ( outcome.error->code, grab::ErrorCode::AmbiguousMatch );
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::FailedBeforeCommit );
    EXPECT_EQ( route.commit_count(), 0U );
    EXPECT_EQ( runtime.seat().neutralize_count(), 1U );
    EXPECT_EQ( outcome.receipt.neutralization,
               grab::NeutralizationOutcome::NothingHeld );
}

TEST( ActionTransaction,
      PossiblyCommittedActionIsNeverRetried )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id() ) );
    auto& route = runtime.add_route( "semantic.invoke", grab::spi::RouteKind::Semantic );
    route.set_commit_error( grab::ErrorCode::PossiblyCommitted,
                            "fake transport lost the commit acknowledgement" );
    auto options  = forced_options();
    options.retry = grab::RetryClass::Compensated;
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const auto outcome = transaction.perform( click_control(), options );

    ASSERT_TRUE( outcome.error.has_value() );
    EXPECT_EQ( outcome.error->code, grab::ErrorCode::PossiblyCommitted );
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::PossiblyCommitted );
    EXPECT_EQ( route.commit_count(), 1U );
    EXPECT_EQ( outcome.receipt.resolve_retries, 0U );
    EXPECT_EQ( outcome.receipt.neutralization,
               grab::NeutralizationOutcome::NothingHeld );
}

TEST( ActionTransaction,
      ArmsAndRecordsBarriersBeforeTheSingleCommit )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id() ) );
    auto& route =
        runtime.add_route( "physical.pointer", grab::spi::RouteKind::Physical );
    route.add_barrier( "focus_enters_target", true, false );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const auto outcome = transaction.perform( click_control(), forced_options() );

    ASSERT_FALSE( outcome.error.has_value() ) << outcome.error->message;
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::Verified );
    ASSERT_EQ( outcome.receipt.barriers.size(), 1U );
    EXPECT_EQ( outcome.receipt.barriers.front().barrier, "focus_enters_target" );
    EXPECT_TRUE( outcome.receipt.barriers.front().satisfied );
    EXPECT_FALSE( outcome.receipt.barriers.front().timed_out );
    const auto arm =
        std::ranges::find( runtime.action_log(), "arm:focus_enters_target" );
    const auto commit = std::ranges::find( runtime.action_log(), "commit" );
    ASSERT_NE( arm, runtime.action_log().end() );
    ASSERT_NE( commit, runtime.action_log().end() );
    EXPECT_LT( arm, commit );
    EXPECT_EQ( route.commit_count(), 1U );
}

TEST( ActionTransaction,
      ForceBypassesActionabilityAndIsRecorded )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id(), 1U, false ) );
    runtime.add_route( "semantic.invoke", grab::spi::RouteKind::Semantic );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const auto outcome = transaction.perform( click_control(), forced_options() );

    ASSERT_FALSE( outcome.error.has_value() ) << outcome.error->message;
    EXPECT_TRUE( outcome.receipt.forced );
    EXPECT_EQ( runtime.event_source()->wait_count(), 0U );
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::Verified );
}

TEST( ActionTransaction,
      PreferSemanticRecordsExplicitPhysicalFallback )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id() ) );
    auto& semantic =
        runtime.add_route( "semantic.invoke", grab::spi::RouteKind::Semantic );
    semantic.set_reserve_error( grab::ErrorCode::RouteUnavailable,
                                "semantic action is unsupported" );
    auto& physical =
        runtime.add_route( "physical.pointer", grab::spi::RouteKind::Physical );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const auto outcome = transaction.perform( click_control(), forced_options() );

    ASSERT_FALSE( outcome.error.has_value() ) << outcome.error->message;
    EXPECT_TRUE( outcome.receipt.fallback_used );
    ASSERT_EQ( outcome.receipt.routes.size(), 2U );
    EXPECT_FALSE( outcome.receipt.routes.front().selected );
    EXPECT_EQ( outcome.receipt.routes.front().rejection,
               grab::ErrorCode::RouteUnavailable );
    EXPECT_TRUE( outcome.receipt.routes.back().selected );
    EXPECT_EQ( semantic.commit_count(), 0U );
    EXPECT_EQ( physical.commit_count(), 1U );
}

TEST( ActionTransaction,
      DragVerbReservesRouteAndReturnsVerifiedReceipt )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id() ) );
    auto& route =
        runtime.add_route( "physical.pointer", grab::spi::RouteKind::Physical );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const grab::Action                drag = grab::Drag{
        .target  = grab::sel::role( grab::role::control ),
        .from    = grab::geometry::Point{ .x = 10, .y = 10 },
        .to      = grab::geometry::Point{ .x = 40, .y = 40 },
        .options = grab::input::DragOptions{},
    };
    const auto outcome = transaction.perform( drag, forced_options() );

    EXPECT_FALSE( outcome.error.has_value() );
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::Verified );
    EXPECT_EQ( route.commit_count(), 1U );
    EXPECT_EQ( runtime.seat().neutralize_count(), 1U );
}

TEST( ActionTransaction,
      PressKeyVerbReservesRouteAndReturnsVerifiedReceipt )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( snapshot( runtime.runtime_id() ) );
    auto& route =
        runtime.add_route( "physical.keyboard", grab::spi::RouteKind::Physical );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const grab::Action                press = grab::PressKey{
        .target   = grab::sel::role( grab::role::control ),
        .key_name = "Return",
    };
    const auto outcome = transaction.perform( press, forced_options() );

    EXPECT_FALSE( outcome.error.has_value() );
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::Verified );
    EXPECT_EQ( route.commit_count(), 1U );
    EXPECT_EQ( runtime.seat().neutralize_count(), 1U );
}

TEST( ActionTransaction,
      ActivateVerbReservesRouteAndReturnsVerifiedReceipt )
{
    grab::testing::FakeRuntime runtime;
    runtime.inject_snapshot( window_snapshot( runtime.runtime_id() ) );
    auto& route =
        runtime.add_route( "physical.activate", grab::spi::RouteKind::Physical );
    grab::kernel::action::Transaction transaction{ runtime, treeId };

    const grab::Action                activate = grab::Activate{
        .target = grab::sel::role( grab::role::window ),
    };
    const auto outcome = transaction.perform( activate, forced_options() );

    ASSERT_FALSE( outcome.error.has_value() ) << outcome.error->message;
    EXPECT_EQ( outcome.receipt.commit, grab::CommitStatus::Verified );
    EXPECT_EQ( route.commit_count(), 1U );
}
