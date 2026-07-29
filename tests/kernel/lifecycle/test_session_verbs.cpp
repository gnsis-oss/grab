#include "fake/fake_runtime.hpp"
#include "grab/capture.hpp"
#include "grab/context.hpp"
#include "grab/event.hpp"
#include "grab/ids.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/lifecycle/session_impl.hpp"
#include "kernel/tree_fixtures.hpp"
#include "spi/route.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

// NOLINTBEGIN(readability-trailing-comma)
namespace
{

    // This matches the primary tree id the session core targets:
    // X11TreeSource::firstTree; SessionCore::attach primes the same id.
    constexpr std::uint32_t        performTree = 1U;
    constexpr grab::NodeId         performNode{ 1U };
    constexpr grab::NodeGeneration performGeneration{ 1U };
    constexpr grab::TreeEpoch      performEpoch{ 1U };
    constexpr std::uint64_t        performRevision         = 17U;
    constexpr std::size_t          noRouteCommits          = 0U;
    constexpr std::size_t          oneRouteCommit          = 1U;
    constexpr std::string_view     missingOutputName       = "eDP-1";
    constexpr std::uint64_t        captureSnapshotRevision = 1U;
    constexpr std::uint64_t        captureWindowNodeId     = 1U;
    constexpr grab::SpaceRect
        describeBounds{ .x = 40.0, .y = 60.0, .w = 320.0, .h = 220.0 };
    constexpr std::string_view describeName  = "Read more";
    constexpr std::string_view describeTitle = "Wikipedia, the free encyclopedia";
    constexpr std::string_view describeText  = "The tiger is a large cat.";
    constexpr std::string_view describeUrl    = "https://en.wikipedia.org/wiki/Tiger";
    constexpr std::uint32_t
        describeFacets = grab::facet_mask( grab::Facet::Text ) | grab::Facet::Invokable;

    [[nodiscard]]
    grab::UiSnapshot
    perform_snapshot( grab::RuntimeId runtime )
    {
        const auto node = grab::UiNodeRecord{
            performNode,
            performGeneration,
            grab::role::window,
            grab::state_mask( grab::NodeState::Visible ) | grab::NodeState::Enabled,
            {                  },
            grab::UiProvenance{
                                                                                 .runtime  = runtime,.revision = performRevision,
                                                                                 },
        };
        std::vector<grab::UiNodeRecord> nodes{ node };
        std::vector<grab::NodeId>       roots{ node.id };
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtime,
                .tree     = performTree,
                .epoch    = performEpoch,
                .revision = performRevision,
                .complete = true,
            },
            std::move( nodes ),
            std::move( roots ),
            {}
        );
    }

    [[nodiscard]]
    grab::UiSnapshot
    describe_snapshot( grab::RuntimeId runtime )
    {
        const auto string_property =
            []( grab::PropertyId id, std::string_view value )
        {
            return grab::UiProperty{
                .id   = id,
                .read = grab::PropertyRead{
                    .state = grab::PropertyRead::State::Present,
                    .value = std::string{ value },
                },
            };
        };
        std::vector<grab::UiProperty> properties{
            grab::UiProperty{
                             .id   = grab::property::bounds,
                             .read = grab::PropertyRead{
                    .state = grab::PropertyRead::State::Present,
                    .value = describeBounds,
                }, },
            string_property( grab::property::accessible_name, describeName ),
            string_property( grab::property::title, describeTitle ),
            string_property( grab::property::text, describeText ),
            string_property( grab::property::url, describeUrl ),
        };
        const auto node = grab::UiNodeRecord{
            performNode,
            performGeneration,
            grab::role::window,
            grab::state_mask( grab::NodeState::Visible ) | grab::NodeState::Enabled,
            describeFacets,
            std::move( properties ),
            grab::UiProvenance{
                               .runtime  = runtime,
                               .revision = performRevision,
                               },
        };
        std::vector<grab::UiNodeRecord> nodes{ node };
        std::vector<grab::NodeId>       roots{ node.id };
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtime,
                .tree     = performTree,
                .epoch    = performEpoch,
                .revision = performRevision,
                .complete = true,
            },
            std::move( nodes ),
            std::move( roots ),
            {}
        );
    }

}    // namespace

TEST( SessionVerbs,
      ResolveFindsNodeAndWatchYieldsSubscriptionId )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 1U, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto match = core->resolve( grab::sel::role( grab::role::window ),
                                      grab::Cardinality::ExactlyOne );
    ASSERT_TRUE( match.has_value() );
    EXPECT_EQ( match->ref.node, 1U );

    auto sub = core->watch( grab::SubscriptionScope{
        .kinds  = { grab::EventKind::NodeChanged },
        .filter = {},
    } );
    ASSERT_TRUE( sub.has_value() );
    EXPECT_NE( sub->id(), grab::SubscriptionId{} );
}

TEST( SessionVerbs,
      ResolveExactlyOneOnEmptyScopeReturnsNoMatch )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        1U,
        { grab::testing::tree::node( 1U, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto match = core->resolve( grab::sel::role( grab::role::button ),
                                      grab::Cardinality::ExactlyOne );
    ASSERT_FALSE( match.has_value() );
    EXPECT_EQ( match.error().code, grab::ErrorCode::NoMatch );
}

TEST( SessionVerbs,
      CaptureWithoutDisplayRuntimeReturnsCapabilityUnavailable )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( grab::testing::tree::snapshot(
        captureSnapshotRevision,
        { grab::testing::tree::node( captureWindowNodeId, grab::role::window ) }
    ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto frame =
        core->capture( grab::CaptureTarget{ std::string{ missingOutputName } } );
    ASSERT_FALSE( frame.has_value() );
    EXPECT_EQ( frame.error().code, grab::ErrorCode::CapabilityUnavailable );
}

TEST( SessionVerbs,
      CaptureVerbOnNullCoreReturnsCapabilityUnavailable )
{
    const auto frame = grab::kernel::lifecycle::capture_verb(
        nullptr,
        grab::CaptureTarget{ std::string{ missingOutputName } },
        {}
    );

    ASSERT_FALSE( frame.has_value() );
    EXPECT_EQ( frame.error().code, grab::ErrorCode::CapabilityUnavailable );
}

TEST( SessionVerbs,
      PerformClickReturnsReceiptWithCommitStatus )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( perform_snapshot( fake.runtime_id() ) );
    auto& route = fake.add_route( "pointer.click", grab::spi::RouteKind::Physical );

    auto  core  = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    // Actionability's state_stable( 2U ) predicate needs a second observation;
    // one no-op wakeup lets the WaitEngine re-observe.
    fake.event_source()->push_wakeup(
        []
        {
        }
    );
    const auto receipt =
        core->perform( grab::Click{ .target = grab::sel::role( grab::role::window ) },
                       {} );

    ASSERT_TRUE( receipt.has_value() );
    // Fully successful transactions end Verified; Committed-only outcomes
    // always carry an error.
    EXPECT_EQ( receipt->commit, grab::CommitStatus::Verified );
    EXPECT_FALSE( receipt->routes.empty() );
    EXPECT_EQ( route.commit_count(), oneRouteCommit );
}

TEST( SessionVerbs,
      PerformHonorsCallerStopToken )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( perform_snapshot( fake.runtime_id() ) );
    auto& route = fake.add_route( "pointer.click", grab::spi::RouteKind::Physical );

    auto  core  = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    std::stop_source stop;
    stop.request_stop();
    const auto receipt =
        core->perform( grab::Click{ .target = grab::sel::role( grab::role::window ) },
                       grab::ActionOptions{ .stop = stop.get_token() } );

    ASSERT_FALSE( receipt.has_value() );
    EXPECT_EQ( receipt.error().code, grab::ErrorCode::Cancelled );
    EXPECT_EQ( route.commit_count(), noRouteCommits );
}

TEST( SessionVerbs,
      DescribeReturnsResolvedNodeGeometryStateAndText )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( describe_snapshot( fake.runtime_id() ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    const auto match = core->resolve( grab::sel::role( grab::role::window ),
                                      grab::Cardinality::ExactlyOne );
    ASSERT_TRUE( match.has_value() );

    const auto info = core->describe( *match );
    ASSERT_TRUE( info.has_value() ) << info.error().message;
    EXPECT_EQ( info->bounds.x, describeBounds.x );
    EXPECT_EQ( info->bounds.y, describeBounds.y );
    EXPECT_EQ( info->bounds.w, describeBounds.w );
    EXPECT_EQ( info->bounds.h, describeBounds.h );
    EXPECT_EQ( info->role, grab::role::window );
    EXPECT_TRUE( grab::has_state( info->states, grab::NodeState::Visible ) );
    EXPECT_TRUE( grab::has_state( info->states, grab::NodeState::Enabled ) );
    EXPECT_EQ( info->name, describeName );
    EXPECT_EQ( info->title, describeTitle );
    EXPECT_EQ( info->text, describeText );
    EXPECT_EQ( info->url, describeUrl );
    EXPECT_TRUE( grab::has_facet( info->facets, grab::Facet::Text ) );
    EXPECT_TRUE( grab::has_facet( info->facets, grab::Facet::Invokable ) );
}

TEST( SessionVerbs,
      DescribeMissingNodeReturnsNoMatch )
{
    grab::testing::FakeRuntime fake;
    fake.inject_snapshot( describe_snapshot( fake.runtime_id() ) );

    auto core = grab::kernel::lifecycle::SessionCore::open_for_test();
    ASSERT_NE( core, nullptr );
    const grab::OperationContext context{};
    ASSERT_TRUE( core->attach( fake, context ).has_value() );

    grab::Match absent{};
    absent.ref.node       = 999U;
    absent.ref.generation = performGeneration;
    const auto info       = core->describe( absent );
    ASSERT_FALSE( info.has_value() );
    EXPECT_EQ( info.error().code, grab::ErrorCode::NoMatch );
}

TEST( SessionVerbs,
      DescribeVerbOnNullCoreReturnsCapabilityUnavailable )
{
    const auto info = grab::kernel::lifecycle::describe_verb( nullptr, grab::Match{} );
    ASSERT_FALSE( info.has_value() );
    EXPECT_EQ( info.error().code, grab::ErrorCode::CapabilityUnavailable );
}

// NOLINTEND(readability-trailing-comma)
