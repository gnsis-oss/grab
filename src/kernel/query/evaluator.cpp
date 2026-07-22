#include "grab/ids.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"
#include "kernel/query/evaluator.hpp"
#include "kernel/query/locator_plan.hpp"
#include "kernel/query/tree_nav.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <walk/delve.hpp>
#include <walk/sweep.hpp>

namespace grab::kernel::query
{
    namespace
    {

        enum class Direction : std::uint8_t
        {
            Forward,
            Reverse,
        };

        class NavGraph final
        {
            public:

                using knot_type = NodeId;
                using edge_type = void;

                NavGraph( const TreeNav& navigation,
                          Direction      direction ) :
                    navigation_{ &navigation },
                    direction_{ direction }
                {
                    std::size_t index = 0U;
                    for( const auto node : navigation.nodes() )
                    {
                        indexes_.try_emplace( node, index );
                        ++index;
                    }
                }

                [[nodiscard]]
                std::span<const NodeId>
                out( NodeId node ) const noexcept
                {
                    if( direction_ == Direction::Forward )
                    {
                        return navigation_->children( node );
                    }
                    return navigation_->parents( node );
                }

                [[nodiscard]]
                bool
                has( NodeId node ) const
                {
                    return indexes_.contains( node ) && navigation_->contains( node );
                }

                [[nodiscard]]
                std::size_t
                size() const noexcept
                {
                    return indexes_.size();
                }

                [[nodiscard]]
                std::span<const NodeId>
                knots() const noexcept
                {
                    return navigation_->nodes();
                }

                [[nodiscard]]
                std::size_t
                dense_id( NodeId node ) const
                {
                    return indexes_.at( node );
                }

                [[nodiscard]]
                std::size_t
                dense_size() const noexcept
                {
                    return navigation_->nodes().size();
                }

            private:

                const TreeNav*                navigation_;
                Direction                     direction_;
                std::map<NodeId, std::size_t> indexes_;
        };

        struct Evaluation
        {
                bool                     matched{};
                std::vector<std::string> evidence;
        };

        struct EvaluatedMatch
        {
                NodeId                   node{};
                std::vector<std::string> evidence;
                std::string              candidate_provider;
        };

        struct RelationUse
        {
                RelationId relation{};
                bool       reverse{};

                friend bool
                operator<( const RelationUse& left,
                           const RelationUse& right ) noexcept
                {
                    if( left.relation != right.relation )
                    {
                        return left.relation < right.relation;
                    }
                    return !left.reverse && right.reverse;
                }
        };

        void
        collect_relation_uses( const detail::LocatorPlan& plan,
                               std::set<RelationUse>&     uses )
        {
            if( plan.op == detail::LocatorOp::Related )
            {
                uses.insert(
                    RelationUse{ .relation = plan.relation, .reverse = false }
                );
            }
            else if( plan.op == detail::LocatorOp::RelatedReverse )
            {
                uses.insert( RelationUse{ .relation = plan.relation, .reverse = true } );
            }
            for( const auto& child : plan.children )
            {
                collect_relation_uses( *child, uses );
            }
        }

        [[nodiscard]]
        Result<void>
        validate_endpoints( std::span<const NodeId> endpoints,
                            const std::set<NodeId>& known,
                            std::string_view        edge_kind )
        {
            for( const auto endpoint : endpoints )
            {
                if( !known.contains( endpoint ) )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 std::string{ "TreeNav " } +
                                     std::string{ edge_kind } +
                                     " endpoint is absent from nodes()" );
                }
            }
            return {};
        }

        [[nodiscard]]
        Result<void>
        validate_navigation( const TreeNav&             navigation,
                             const detail::LocatorPlan& plan )
        {
            const auto             nodes = navigation.nodes();
            const std::set<NodeId> known{ nodes.begin(), nodes.end() };
            if( known.size() != nodes.size() )
            {
                return fail( ErrorCode::InvalidArgument,
                             "TreeNav nodes() contains duplicate identities" );
            }
            for( const auto node : nodes )
            {
                if( !navigation.contains( node ) )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "TreeNav nodes() contains an unavailable node" );
                }
                auto children =
                    validate_endpoints( navigation.children( node ), known, "child" );
                if( !children )
                {
                    return children;
                }
                auto parents =
                    validate_endpoints( navigation.parents( node ), known, "parent" );
                if( !parents )
                {
                    return parents;
                }
            }
            auto roots = validate_endpoints( navigation.roots(), known, "root" );
            if( !roots )
            {
                return roots;
            }

            std::set<RelationUse> uses;
            collect_relation_uses( plan, uses );
            for( const auto node : nodes )
            {
                for( const auto use : uses )
                {
                    const auto endpoints =
                        use.reverse ? navigation.related_reverse( node, use.relation )
                                    : navigation.related( node, use.relation );
                    auto validated = validate_endpoints( endpoints, known, "relation" );
                    if( !validated )
                    {
                        return validated;
                    }
                }
            }
            return {};
        }

        [[nodiscard]]
        bool
        number_equal( double left,
                      double right ) noexcept
        {
            return left == right || ( std::isnan( left ) && std::isnan( right ) );
        }

        [[nodiscard]]
        bool
        rect_equal( const SpaceRect& left,
                    const SpaceRect& right ) noexcept
        {
            return number_equal( left.x, right.x ) &&
                   number_equal( left.y, right.y ) &&
                   number_equal( left.w, right.w ) &&
                   number_equal( left.h, right.h ) &&
                   left.space == right.space;
        }

        [[nodiscard]]
        bool
        property_equal( const PropertyValue& left,
                        const PropertyValue& right )
        {
            if( left.index() != right.index() )
            {
                return false;
            }
            if( std::holds_alternative<std::monostate>( left ) )
            {
                return true;
            }
            if( const auto* value = std::get_if<bool>( &left ) )
            {
                return *value == std::get<bool>( right );
            }
            if( const auto* value = std::get_if<std::int64_t>( &left ) )
            {
                return *value == std::get<std::int64_t>( right );
            }
            if( const auto* value = std::get_if<double>( &left ) )
            {
                return number_equal( *value, std::get<double>( right ) );
            }
            if( const auto* value = std::get_if<std::string>( &left ) )
            {
                return *value == std::get<std::string>( right );
            }
            return rect_equal( std::get<SpaceRect>( left ),
                               std::get<SpaceRect>( right ) );
        }

        [[nodiscard]]
        std::string
        evidence_for( const detail::LocatorPlan& plan )
        {
            using detail::LocatorOp;
            switch( plan.op )
            {
                case LocatorOp::MatchAll :
                    return "match_all";
                case LocatorOp::MatchNone :
                    return "match_none";
                case LocatorOp::Role :
                    return std::string{ "role=" } +
                           std::string{ role_name( plan.role ) };
                case LocatorOp::State :
                    return std::string{ "state=" } +
                           std::to_string( state_mask( plan.state ) );
                case LocatorOp::Property :
                    return std::string{ "property=" } +
                           std::to_string( plan.property.value );
                case LocatorOp::AccessibleName :
                    return "accessible_name";
                case LocatorOp::Text :
                    return "text";
                case LocatorOp::All :
                    return "all";
                case LocatorOp::Any :
                    return "any";
                case LocatorOp::Not :
                    return "not";
                case LocatorOp::ChildOf :
                    return "child_of";
                case LocatorOp::DescendantOf :
                    return "descendant_of";
                case LocatorOp::AncestorOf :
                    return "ancestor_of";
                case LocatorOp::Related :
                    return std::string{ "related=" } +
                           std::string{ relation_name( plan.relation ) };
                case LocatorOp::RelatedReverse :
                    return std::string{ "related_reverse=" } +
                           std::string{ relation_name( plan.relation ) };
            }
            return "unknown";
        }

        class ExpressionEvaluator;

        class RelationVisitor final
        {
            public:

                RelationVisitor( const ExpressionEvaluator& evaluator,
                                 const detail::LocatorPlan& predicate,
                                 NodeId                     origin ) :
                    evaluator_{ &evaluator },
                    predicate_{ &predicate },
                    origin_{ origin }
                {
                }

                void
                on( NodeId node );

                [[nodiscard]]
                Evaluation
                result() const
                {
                    return result_;
                }

            private:

                const ExpressionEvaluator* evaluator_;
                const detail::LocatorPlan* predicate_;
                NodeId                     origin_{};
                Evaluation                 result_;
        };

        class ExpressionEvaluator final
        {
            public:

                explicit ExpressionEvaluator( const TreeNav& navigation ) :
                    navigation_{ &navigation },
                    forward_{
                        navigation,
                        Direction::Forward
                    },
                    reverse_{
                        navigation,
                        Direction::Reverse
                    }
                {
                }

                [[nodiscard]]
                Evaluation
                evaluate( const detail::LocatorPlan& plan,
                          NodeId                     node ) const
                {
                    using detail::LocatorOp;
                    switch( plan.op )
                    {
                        case LocatorOp::MatchAll :
                            return matched( plan );
                        case LocatorOp::MatchNone :
                            return {};
                        case LocatorOp::Role :
                            return when( navigation_->role( node ) == plan.role, plan );
                        case LocatorOp::State :
                            return when( ( navigation_->states( node ) &
                                           state_mask( plan.state ) ) ==
                                             state_mask( plan.state ),
                                         plan );
                        case LocatorOp::Property :
                            return property_matches( node,
                                                     plan.property,
                                                     plan.value,
                                                     plan );
                        case LocatorOp::AccessibleName :
                            return property_matches( node,
                                                     property::accessible_name,
                                                     PropertyValue{ plan.text },
                                                     plan );
                        case LocatorOp::Text :
                            return property_matches( node,
                                                     property::text,
                                                     PropertyValue{ plan.text },
                                                     plan );
                        case LocatorOp::All :
                            return evaluate_all( plan, node );
                        case LocatorOp::Any :
                            return evaluate_any( plan, node );
                        case LocatorOp::Not :
                            return evaluate_not( plan, node );
                        case LocatorOp::ChildOf :
                            return evaluate_neighbors( plan,
                                                       navigation_->parents( node ),
                                                       node );
                        case LocatorOp::DescendantOf :
                            return evaluate_walk( plan, reverse_, node );
                        case LocatorOp::AncestorOf :
                            return evaluate_walk( plan, forward_, node );
                        case LocatorOp::Related :
                            return evaluate_neighbors(
                                plan,
                                navigation_->related( node, plan.relation ),
                                node
                            );
                        case LocatorOp::RelatedReverse :
                            return evaluate_neighbors(
                                plan,
                                navigation_->related_reverse( node, plan.relation ),
                                node
                            );
                    }
                    return {};
                }

            private:

                friend class RelationVisitor;

                [[nodiscard]]
                static Evaluation
                matched( const detail::LocatorPlan& plan )
                {
                    return Evaluation{
                        .matched  = true,
                        .evidence = { evidence_for( plan ) },
                    };
                }

                [[nodiscard]]
                static Evaluation
                when( bool                       condition,
                      const detail::LocatorPlan& plan )
                {
                    return condition ? matched( plan ) : Evaluation{};
                }

                [[nodiscard]]
                Evaluation
                property_matches( NodeId                     node,
                                  PropertyId                 property_id,
                                  const PropertyValue&       expected,
                                  const detail::LocatorPlan& plan ) const
                {
                    const auto read = navigation_->property( node, property_id );
                    return when( read.state ==
                                     PropertyRead::State::Present &&
                                     property_equal( read.value, expected ),
                                 plan );
                }

                [[nodiscard]]
                Evaluation
                evaluate_all( const detail::LocatorPlan& plan,
                              NodeId                     node ) const
                {
                    Evaluation result{ .matched = true, .evidence = {} };
                    for( const auto& child : plan.children )
                    {
                        const auto child_result = evaluate( *child, node );
                        if( !child_result.matched )
                        {
                            return {};
                        }
                        result.evidence.insert( result.evidence.end(),
                                                child_result.evidence.begin(),
                                                child_result.evidence.end() );
                    }
                    return result;
                }

                [[nodiscard]]
                Evaluation
                evaluate_any( const detail::LocatorPlan& plan,
                              NodeId                     node ) const
                {
                    for( const auto& child : plan.children )
                    {
                        auto child_result = evaluate( *child, node );
                        if( child_result.matched )
                        {
                            return child_result;
                        }
                    }
                    return {};
                }

                [[nodiscard]]
                Evaluation
                evaluate_not( const detail::LocatorPlan& plan,
                              NodeId                     node ) const
                {
                    if( plan.children.empty() ||
                        evaluate( *plan.children.front(), node ).matched )
                    {
                        return {};
                    }
                    return matched( plan );
                }

                [[nodiscard]]
                Evaluation
                evaluate_neighbors( const detail::LocatorPlan& plan,
                                    std::span<const NodeId>    neighbors,
                                    NodeId /* origin */ ) const
                {
                    if( plan.children.empty() )
                    {
                        return {};
                    }
                    for( const auto neighbor : neighbors )
                    {
                        auto result = evaluate( *plan.children.front(), neighbor );
                        if( result.matched )
                        {
                            result.evidence.push_back( evidence_for( plan ) );
                            return result;
                        }
                    }
                    return {};
                }

                [[nodiscard]]
                Evaluation
                evaluate_walk( const detail::LocatorPlan& plan,
                               const NavGraph&            graph,
                               NodeId                     origin ) const
                {
                    if( plan.children.empty() )
                    {
                        return {};
                    }
                    RelationVisitor visitor{ *this, *plan.children.front(), origin };
                    walk::delve( graph, origin, visitor );
                    auto result = visitor.result();
                    if( result.matched )
                    {
                        result.evidence.push_back( evidence_for( plan ) );
                    }
                    return result;
                }

                const TreeNav* navigation_;
                NavGraph       forward_;
                NavGraph       reverse_;
        };

        void
        RelationVisitor::on( NodeId node )
        {
            if( node == origin_ || result_.matched )
            {
                return;
            }
            result_ = evaluator_->evaluate( *predicate_, node );
        }

        class CandidateVisitor final
        {
            public:

                CandidateVisitor( std::vector<NodeId>& order,
                                  std::set<NodeId>&    seen ) :
                    order_{ &order },
                    seen_{ &seen }
                {
                }

                void
                on( NodeId node )
                {
                    if( seen_->insert( node ).second )
                    {
                        order_->push_back( node );
                    }
                }

            private:

                std::vector<NodeId>* order_;
                std::set<NodeId>*    seen_;
        };

        [[nodiscard]]
        Result<std::vector<NodeId>>
        enumerate_scope( QueryScope scope )
        {
            const NavGraph      graph{ scope.navigation, Direction::Forward };
            std::vector<NodeId> order;
            std::set<NodeId>    seen;
            CandidateVisitor    visitor{ order, seen };

            if( scope.root.has_value() )
            {
                if( !graph.has( *scope.root ) )
                {
                    return fail( ErrorCode::NoMatch,
                                 "query scope root is not present in the tree" );
                }
                walk::sweep( graph, *scope.root, visitor );
                return order;
            }

            for( const auto root : scope.navigation.roots() )
            {
                if( graph.has( root ) )
                {
                    walk::sweep( graph, root, visitor );
                }
            }
            for( const auto node : scope.navigation.nodes() )
            {
                if( !seen.contains( node ) && graph.has( node ) )
                {
                    walk::sweep( graph, node, visitor );
                }
            }
            return order;
        }

        [[nodiscard]]
        Result<std::vector<EvaluatedMatch>>
        evaluate_matches( const Locator& locator,
                          QueryScope     scope,
                          LocatorLimits  limits )
        {
            const auto complexity = detail::plan_node_count( locator );
            if( complexity > limits.max_nodes )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ "locator complexity " } +
                                 std::to_string( complexity ) +
                                 " exceeds max_nodes=" +
                                 std::to_string( limits.max_nodes ) );
            }

            const auto& plan       = detail::plan_of( locator );
            auto        navigation = validate_navigation( scope.navigation, plan );
            if( !navigation )
            {
                return std::unexpected( std::move( navigation.error() ) );
            }

            auto candidates = enumerate_scope( scope );
            if( !candidates.has_value() )
            {
                return std::unexpected( std::move( candidates.error() ) );
            }

            std::string candidate_provider;
            if( scope.lowerer != nullptr )
            {
                auto lowered = scope.lowerer->lower( locator, scope.navigation );
                if( !lowered.has_value() )
                {
                    return std::unexpected( std::move( lowered.error() ) );
                }
                if( lowered->has_value() )
                {
                    const auto&      narrowed = **lowered;
                    std::set<NodeId> allowed{
                        narrowed.nodes.begin(),
                        narrowed.nodes.end()
                    };
                    std::erase_if( *candidates,
                                   [&allowed]( NodeId node )
                                   {
                                       return !allowed.contains( node );
                                   } );
                    if( !narrowed.provider.empty() )
                    {
                        candidate_provider = narrowed.provider;
                    }
                }
            }

            const ExpressionEvaluator   evaluator{ scope.navigation };
            std::vector<EvaluatedMatch> matches;
            for( const auto candidate : *candidates )
            {
                auto result = evaluator.evaluate( plan, candidate );
                if( result.matched )
                {
                    matches.push_back( EvaluatedMatch{
                        .node               = candidate,
                        .evidence           = std::move( result.evidence ),
                        .candidate_provider = candidate_provider,
                    } );
                }
            }
            return matches;
        }

        [[nodiscard]]
        WidgetRef
        make_ref( const TreeNav& navigation,
                  NodeId         node )
        {
            const auto metadata = navigation.metadata();
            return WidgetRef{
                .runtime    = metadata.runtime,
                .tree       = metadata.tree,
                .epoch      = metadata.epoch,
                .node       = node.value,
                .generation = navigation.generation( node ),
            };
        }

        [[nodiscard]]
        Match
        make_match( const Locator&        locator,
                    const TreeNav&        navigation,
                    const EvaluatedMatch& evaluated )
        {
            const auto metadata   = navigation.metadata();
            const auto provenance = navigation.provenance( evaluated.node );
            return Match{
                .ref                = make_ref( navigation, evaluated.node ),
                .mode               = locator.consistency(),
                .snapshot_revision  = metadata.revision,
                .matched_predicates = evaluated.evidence,
                .provenance         = ProviderProvenance{
                                                         .provider           = std::string{ metadata.provider },
                                                         .candidate_provider = evaluated.candidate_provider,
                                                         .runtime            = provenance.runtime,
                                                         .revision           = provenance.revision,
                                                         },
            };
        }

    }    // namespace

    Result<NodeSet>
    resolve_all( const Locator& locator,
                 QueryScope     scope,
                 LocatorLimits  limits )
    {
        auto matches = evaluate_matches( locator, scope, limits );
        if( !matches.has_value() )
        {
            return std::unexpected( std::move( matches.error() ) );
        }

        NodeSet nodes;
        nodes.reserve( matches->size() );
        for( const auto& match : *matches )
        {
            nodes.push_back( make_ref( scope.navigation, match.node ) );
        }
        return nodes;
    }

    Result<Match>
    resolve( const Locator& locator,
             Cardinality    cardinality,
             QueryScope     scope,
             LocatorLimits  limits )
    {
        if( cardinality == Cardinality::All )
        {
            return fail( ErrorCode::InvalidArgument,
                         "Cardinality::All returns a NodeSet through resolve_all" );
        }

        auto matches = evaluate_matches( locator, scope, limits );
        if( !matches.has_value() )
        {
            return std::unexpected( std::move( matches.error() ) );
        }
        if( matches->empty() )
        {
            return fail( ErrorCode::NoMatch, "locator matched no nodes" );
        }
        if( cardinality == Cardinality::ExactlyOne && matches->size() > 1U )
        {
            return fail( ErrorCode::AmbiguousMatch,
                         std::string{ "locator matched " } +
                             std::to_string( matches->size() ) +
                             " nodes" );
        }
        return make_match( locator, scope.navigation, matches->front() );
    }

}    // namespace grab::kernel::query
