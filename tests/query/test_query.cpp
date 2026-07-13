#include "grab/ids.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "kernel/query/evaluator.hpp"
#include "kernel/query/snapshot_tree_nav.hpp"
#include "kernel/query/tree_nav.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr grab::RuntimeId      runtimeId{ 17U };
    constexpr std::uint32_t        treeId = 4U;
    constexpr grab::TreeEpoch      treeEpoch{ 9U };
    constexpr std::uint64_t        revision        = 23U;
    constexpr std::uint64_t        secondControlId = 5U;
    constexpr grab::NodeId         fakeParentId{ 41U };
    constexpr grab::NodeId         fakeChildId{ 42U };
    constexpr grab::PropertyId     fakeProperty{ 77U };
    constexpr std::int64_t         fakePropertyValue = 12;
    constexpr grab::NodeGeneration fakeGeneration{ 6U };
    constexpr grab::NodeId         missingFakeNode{ 99U };

    [[nodiscard]]
    grab::UiNodeRecord
    node( std::uint64_t                 id,
          grab::RoleId                  role,
          std::uint32_t                 states     = 0U,
          std::vector<grab::UiProperty> properties = {} )
    {
        return grab::UiNodeRecord{
            grab::NodeId{id                          },
            grab::NodeGeneration{                  2U },
            role,
            states,
            std::move( properties ),
            grab::UiProvenance{
                         .runtime  = runtimeId,.revision = revision,
                         },
        };
    }

    [[nodiscard]]
    grab::UiProperty
    text_property( grab::PropertyId property,
                   std::string      value )
    {
        return grab::UiProperty{
            .id   = property,
            .read = grab::PropertyRead{
                                       .state = grab::PropertyRead::State::Present,
                                       .value = std::move( value ),
                                       },
        };
    }

    [[nodiscard]]
    grab::UiSnapshot
    query_snapshot( bool second_control = false )
    {
        std::vector<grab::UiNodeRecord> nodes{
            node( 1U, grab::role::application ),
            node( 2U, grab::role::panel ),
            node( 3U,
                  grab::role::control,
                  grab::NodeState::Visible | grab::NodeState::Enabled,
                  {
                      text_property( grab::property::accessible_name, "Save" ),
                      text_property( grab::property::text, "Save document" ),
                  } ),
            node( 4U,
                  grab::role::text,
                  0U,
                  { text_property( grab::property::accessible_name, "Save label" ) } ),
        };
        if( second_control )
        {
            nodes.push_back( node( secondControlId, grab::role::control ) );
        }

        std::vector<grab::UiRelation> relations{
            grab::UiRelation{
                             .source   = grab::NodeId{ 1U },
                             .target   = grab::NodeId{ 2U },
                             .relation = grab::relation::contains,
                             },
            grab::UiRelation{
                             .source   = grab::NodeId{ 2U },
                             .target   = grab::NodeId{ 3U },
                             .relation = grab::relation::contains,
                             },
            grab::UiRelation{
                             .source   = grab::NodeId{ 3U },
                             .target   = grab::NodeId{ 4U },
                             .relation = grab::relation::labelled_by,
                             },
        };
        if( second_control )
        {
            relations.push_back( grab::UiRelation{
                .source   = grab::NodeId{ 2U },
                .target   = grab::NodeId{ secondControlId },
                .relation = grab::relation::contains,
            } );
        }

        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtimeId,
                .tree     = treeId,
                .epoch    = treeEpoch,
                .revision = revision,
                .complete = true,
            },
            std::move( nodes ),
            { grab::NodeId{ 1U } },
            std::move( relations )
        );
    }

    class FakeTreeNav final : public grab::kernel::query::TreeNav
    {
        public:

            FakeTreeNav()
            {
                nodes_                    = { fakeParentId, fakeChildId };
                roots_                    = { nodes_.front() };
                children_[nodes_.front()] = { nodes_.back() };
                parents_[nodes_.back()]   = { nodes_.front() };
                roles_[nodes_.front()]    = grab::role::application;
                roles_[nodes_.back()]     = grab::role::control;
                states_[nodes_.back()]    = grab::state_mask( grab::NodeState::Enabled );
                properties_[{ nodes_.back(), fakeProperty }] = grab::PropertyRead{
                    .state = grab::PropertyRead::State::Present,
                    .value = fakePropertyValue,
                };
            }

            void
            add_dangling_child()
            {
                children_[nodes_.front()].push_back( missingFakeNode );
            }

            [[nodiscard]]
            grab::kernel::query::TreeNavMetadata
            metadata() const noexcept override
            {
                return grab::kernel::query::TreeNavMetadata{
                    .runtime  = runtimeId,
                    .tree     = treeId,
                    .epoch    = treeEpoch,
                    .revision = revision,
                    .provider = "fake-nav",
                };
            }

            [[nodiscard]]
            std::span<const grab::NodeId>
            nodes() const noexcept override
            {
                return nodes_;
            }

            [[nodiscard]]
            std::span<const grab::NodeId>
            roots() const noexcept override
            {
                return roots_;
            }

            [[nodiscard]]
            bool
            contains( grab::NodeId id ) const noexcept override
            {
                return roles_.contains( id );
            }

            [[nodiscard]]
            grab::RoleId
            role( grab::NodeId id ) const override
            {
                return roles_.at( id );
            }

            [[nodiscard]]
            std::uint32_t
            states( grab::NodeId id ) const override
            {
                const auto found = states_.find( id );
                return found == states_.end() ? 0U : found->second;
            }

            [[nodiscard]]
            grab::PropertyRead
            property( grab::NodeId     id,
                      grab::PropertyId property_id ) const override
            {
                const auto found = properties_.find( { id, property_id } );
                return found == properties_.end() ? grab::PropertyRead{} : found->second;
            }

            [[nodiscard]]
            std::span<const grab::NodeId>
            children( grab::NodeId id ) const noexcept override
            {
                return values( children_, id );
            }

            [[nodiscard]]
            std::span<const grab::NodeId>
            parents( grab::NodeId id ) const noexcept override
            {
                return values( parents_, id );
            }

            [[nodiscard]]
            std::span<const grab::NodeId>
            related(
                [[maybe_unused]] grab::NodeId     id,
                [[maybe_unused]] grab::RelationId relation_id
            ) const noexcept override
            {
                return {};
            }

            [[nodiscard]]
            std::span<const grab::NodeId>
            related_reverse(
                [[maybe_unused]] grab::NodeId     id,
                [[maybe_unused]] grab::RelationId relation_id
            ) const noexcept override
            {
                return {};
            }

            [[nodiscard]]
            grab::NodeGeneration
            generation( [[maybe_unused]] grab::NodeId id ) const override
            {
                return fakeGeneration;
            }

            [[nodiscard]]
            grab::UiProvenance
            provenance( [[maybe_unused]] grab::NodeId id ) const override
            {
                return grab::UiProvenance{
                    .runtime  = runtimeId,
                    .revision = revision,
                };
            }

        private:

            using Edges = std::map<grab::NodeId, std::vector<grab::NodeId>>;

            [[nodiscard]]
            static std::span<const grab::NodeId>
            values( const Edges& edges,
                    grab::NodeId id ) noexcept
            {
                const auto found = edges.find( id );
                return found == edges.end()
                         ? std::span<const grab::NodeId>{}
                         : std::span<const grab::NodeId>{ found->second };
            }

            std::vector<grab::NodeId>             nodes_;
            std::vector<grab::NodeId>             roots_;
            Edges                                 children_;
            Edges                                 parents_;
            std::map<grab::NodeId, grab::RoleId>  roles_;
            std::map<grab::NodeId, std::uint32_t> states_;
            std::map<std::pair<grab::NodeId, grab::PropertyId>, grab::PropertyRead>
                properties_;
    };

}    // namespace

TEST( Locator,
      IsImmutableHashableAndCanonicallySerializable )
{
    const auto base = grab::sel::role( grab::role::control );
    const auto locator =
        base.and_( grab::sel::state( grab::NodeState::Enabled ) )
            .and_( grab::sel::property( grab::PropertyId{ 19U }, std::int64_t{ 7 } ) )
            .and_( grab::sel::any( {
                grab::sel::accessible_name( "Save \"as\"" ),
                grab::sel::not_( grab::sel::text( "Discard" ) ),
            } ) )
            .and_( grab::sel::related( grab::relation::labelled_by,
                                       grab::sel::accessible_name( "Save label" ) ) )
            .with_boundary( grab::BoundaryPolicy::CrossEmbeds )
            .with_consistency( grab::ConsistencyMode::Pinned );

    EXPECT_EQ(
        base.to_string(),
        R"({"boundary":"same_tree","consistency":"live","expr":{"op":"role","value":10},"version":1})"
    );
    EXPECT_NE( locator, base );
    EXPECT_EQ( base, grab::sel::role( grab::role::control ) );
    EXPECT_EQ( std::hash<grab::Locator>{}( base ),
               std::hash<grab::Locator>{}( grab::sel::role( grab::role::control ) ) );

    const auto restored = grab::Locator::from_string( locator.to_string() );
    ASSERT_TRUE( restored.has_value() ) << restored.error().message;
    EXPECT_EQ( *restored, locator );
    EXPECT_EQ( restored->boundary(), grab::BoundaryPolicy::CrossEmbeds );
    EXPECT_EQ( restored->consistency(), grab::ConsistencyMode::Pinned );

    grab::QueryValue set_value  = grab::NodeSet{};
    grab::QueryValue node_value = grab::WidgetRef{};
    EXPECT_TRUE( std::holds_alternative<grab::NodeSet>( set_value ) );
    EXPECT_TRUE( std::holds_alternative<grab::WidgetRef>( node_value ) );

    auto       moving = locator;
    const auto moved  = std::move( moving );
    EXPECT_EQ( moved, locator );
    EXPECT_EQ( moving, locator );    // NOLINT(bugprone-use-after-move)

    const auto composed_options = grab::sel::all( {
        base.with_boundary( grab::BoundaryPolicy::SameProcess ),
        grab::sel::state( grab::NodeState::Visible )
            .with_consistency( grab::ConsistencyMode::Pinned ),
    } );
    EXPECT_EQ( composed_options.boundary(), grab::BoundaryPolicy::SameProcess );
    EXPECT_EQ( composed_options.consistency(), grab::ConsistencyMode::Pinned );
}

TEST( Locator,
      RoundTripsNonFiniteNumbersAndRejectsMalformedNumericInput )
{
    const auto non_finite = grab::sel::all( {
        grab::sel::property( grab::PropertyId{ 30U },
                             std::numeric_limits<double>::quiet_NaN() ),
        grab::sel::property( grab::PropertyId{ 31U },
                             std::numeric_limits<double>::infinity() ),
        grab::sel::property( grab::PropertyId{ 32U },
                             -std::numeric_limits<double>::infinity() ),
    } );
    const auto restored   = grab::Locator::from_string( non_finite.to_string() );
    ASSERT_TRUE( restored.has_value() ) << restored.error().message;
    EXPECT_EQ( *restored, non_finite );

    const auto integer_overflow = grab::Locator::from_string(
        R"({"boundary":"same_tree","consistency":"live","expr":{"op":"property","property":1,"type":"integer","value":18446744073709551615},"version":1})"
    );
    ASSERT_FALSE( integer_overflow.has_value() );
    EXPECT_EQ( integer_overflow.error().code, grab::ErrorCode::InvalidArgument );

    const auto number_overflow = grab::Locator::from_string(
        R"({"boundary":"same_tree","consistency":"live","expr":{"op":"property","property":1,"type":"number","value":1e400},"version":1})"
    );
    ASSERT_FALSE( number_overflow.has_value() );
    EXPECT_EQ( number_overflow.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( Locator,
      SyntaxErrorsCarrySafeCaretContext )
{
    const std::string malformed =
        "{\"boundary\":\"same\n"
        "tree\",\"consistency\":\"live\",\"expr\":{},\"version\":1}";
    const auto result = grab::Locator::from_string( malformed );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( result.error().message.find( '^' ), std::string::npos );
}

TEST( Query,
      ResolveExactlyOneReturnsMatchAndEvidence )
{
    const auto                                 snapshot = query_snapshot();
    const grab::kernel::query::SnapshotTreeNav navigation{ snapshot };
    const auto                                 locator =
        grab::sel::all( {
                            grab::sel::role( grab::role::control ),
                            grab::sel::state( grab::NodeState::Visible |
                                              grab::NodeState::Enabled ),
                            grab::sel::accessible_name( "Save" ),
                        } )
            .with_consistency( grab::ConsistencyMode::Revisioned );

    const auto result = grab::kernel::query::resolve(
        locator,
        grab::Cardinality::ExactlyOne,
        grab::kernel::query::QueryScope{ .navigation = navigation }
    );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->ref.runtime, runtimeId );
    EXPECT_EQ( result->ref.tree, treeId );
    EXPECT_EQ( result->ref.epoch, treeEpoch );
    EXPECT_EQ( result->ref.node, 3U );
    EXPECT_EQ( result->ref.generation, grab::NodeGeneration{ 2U } );
    EXPECT_EQ( result->mode, grab::ConsistencyMode::Revisioned );
    EXPECT_EQ( result->snapshot_revision, revision );
    EXPECT_FALSE( result->matched_predicates.empty() );
    EXPECT_EQ( result->provenance.provider, "snapshot" );
    EXPECT_EQ( result->provenance.runtime, runtimeId );
}

TEST( Query,
      ExactlyOneRejectsAmbiguousAndMissingMatches )
{
    const auto ambiguous_snapshot = query_snapshot( true );
    const grab::kernel::query::SnapshotTreeNav ambiguous_nav{ ambiguous_snapshot };
    const auto                                 ambiguous = grab::kernel::query::resolve(
        grab::sel::role( grab::role::control ),
        grab::Cardinality::ExactlyOne,
        grab::kernel::query::QueryScope{ .navigation = ambiguous_nav }
    );
    ASSERT_FALSE( ambiguous.has_value() );
    EXPECT_EQ( ambiguous.error().code, grab::ErrorCode::AmbiguousMatch );

    const auto                                 missing_snapshot = query_snapshot();
    const grab::kernel::query::SnapshotTreeNav missing_nav{ missing_snapshot };
    const auto                                 missing = grab::kernel::query::resolve(
        grab::sel::role( grab::role::dialog ),
        grab::Cardinality::ExactlyOne,
        grab::kernel::query::QueryScope{ .navigation = missing_nav }
    );
    ASSERT_FALSE( missing.has_value() );
    EXPECT_EQ( missing.error().code, grab::ErrorCode::NoMatch );
}

TEST( Query,
      DescendantAndRelationPredicatesResolveThroughSnapshotNavigation )
{
    const auto                                 snapshot = query_snapshot();
    const grab::kernel::query::SnapshotTreeNav navigation{ snapshot };
    const auto                                 locator = grab::sel::all( {
        grab::sel::role( grab::role::control ),
        grab::sel::descendant_of( grab::sel::role( grab::role::application ) ),
        grab::sel::related( grab::relation::labelled_by,
                            grab::sel::accessible_name( "Save label" ) ),
    } );

    const auto                                 result  = grab::kernel::query::resolve(
        locator,
        grab::Cardinality::ExactlyOne,
        grab::kernel::query::QueryScope{ .navigation = navigation }
    );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->ref.node, 3U );
}

TEST( Query,
      RejectsLocatorOverComplexityBudget )
{
    const auto                                 snapshot = query_snapshot();
    const grab::kernel::query::SnapshotTreeNav navigation{ snapshot };
    const auto                                 locator = grab::sel::all( {
        grab::sel::role( grab::role::control ),
        grab::sel::state( grab::NodeState::Visible ),
        grab::sel::state( grab::NodeState::Enabled ),
    } );

    const auto                                 result = grab::kernel::query::resolve_all(
        locator,
        grab::kernel::query::QueryScope{ .navigation = navigation },
        grab::LocatorLimits{ .max_nodes = 2U }
    );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( TreeNav,
      FakeImplementationProvesEvaluatorHasNoConcreteNodeDependency )
{
    const FakeTreeNav navigation;
    const auto        locator = grab::sel::all( {
        grab::sel::role( grab::role::control ),
        grab::sel::state( grab::NodeState::Enabled ),
        grab::sel::property( fakeProperty, fakePropertyValue ),
        grab::sel::child_of( grab::sel::role( grab::role::application ) ),
    } );

    const auto        result  = grab::kernel::query::resolve(
        locator,
        grab::Cardinality::ExactlyOne,
        grab::kernel::query::QueryScope{ .navigation = navigation }
    );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( result->ref.node, fakeChildId.value );
    EXPECT_EQ( result->ref.generation, fakeGeneration );
    EXPECT_EQ( result->provenance.provider, "fake-nav" );
}

TEST( TreeNav,
      RejectsDanglingInjectedTopologyWithTypedError )
{
    FakeTreeNav navigation;
    navigation.add_dangling_child();

    const auto result = grab::kernel::query::resolve_all(
        grab::sel::role( grab::role::control ),
        grab::kernel::query::QueryScope{ .navigation = navigation }
    );

    ASSERT_FALSE( result.has_value() );
    EXPECT_EQ( result.error().code, grab::ErrorCode::InvalidArgument );
}
