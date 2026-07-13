#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  web/web.h -- web::Web<Policy, Edge>, OneWay, AnyWay                │
// └──────────────────────────────────────────────────────────────────────┘
//
// A flat, policy-driven adjacency list. OneWay is directed (dual storage),
// AnyWay is undirected (symmetric single storage). Edge = void for
// unweighted, any type for weighted with Neighbor<Edge>.

#include <algorithm>
#include <cstddef>
#include <log/writer.hpp>
#include <map>
#include <out/put.hpp>
#include <set>
#include <span>
#include <type_traits>
#include <vector>
#include <web/knot.hpp>
#include <web/trait.hpp>

namespace web
{

    // ── Direction policies ──────────────────────────────────────────────────

    struct OneWay
    {
    };    // directed: tie(a, b) means a -> b only

    struct AnyWay
    {
    };    // undirected: tie(a, b) means a <-> b

    // ── Neighbor (weighted edges) ───────────────────────────────────────────

    template<typename Edge>
    struct Neighbor
    {
            Knot target;
            Edge data;
    };

    // ── Storage type selection ──────────────────────────────────────────────

    namespace detail
    {

        template<typename Edge>
        using AdjValue = std::conditional_t<std::is_void_v<Edge>, Knot, Neighbor<Edge>>;

        template<typename Edge>
        using AdjList = std::vector<AdjValue<Edge>>;

        template<typename Edge>
        using AdjMap = std::map<Knot, AdjList<Edge>>;

        // Extract target knot from adjacency entry
        template<typename Edge>
        Knot
        target_of( const AdjValue<Edge>& entry )
        {
            if constexpr( std::is_void_v<Edge> )
            {
                return entry;
            }
            else
            {
                return entry.target;
            }
        }

        template<typename Edge>
        bool
        has_target( const AdjList<Edge>& list,
                    Knot                 target )
        {
            auto it = std::lower_bound( list.begin(),
                                        list.end(),
                                        target,
                                        []( const AdjValue<Edge>& e, Knot t )
                                        {
                                            return target_of<Edge>( e ) < t;
                                        } );
            return it != list.end() && target_of<Edge>( *it ) == target;
        }

        template<typename Edge>
        void
        remove_target( AdjList<Edge>& list,
                       Knot           target )
        {
            auto it = std::lower_bound( list.begin(),
                                        list.end(),
                                        target,
                                        []( const AdjValue<Edge>& e, Knot t )
                                        {
                                            return target_of<Edge>( e ) < t;
                                        } );
            if( it != list.end() && target_of<Edge>( *it ) == target )
            {
                list.erase( it );
            }
        }

        template<typename Edge>
        void
        sorted_insert( AdjList<Edge>& list,
                       AdjValue<Edge> entry )
        {
            auto target = target_of<Edge>( entry );
            auto pos    = std::lower_bound( list.begin(),
                                            list.end(),
                                            target,
                                            []( const AdjValue<Edge>& e, Knot t )
                                            {
                                             return target_of<Edge>( e ) < t;
                                            } );
            list.insert( pos, std::move( entry ) );
        }

    }    // namespace detail

    // ── Web<Policy, Edge> ───────────────────────────────────────────────────

    template<typename Policy, typename Edge = void>
    class Web
    {
            static_assert( std::is_same_v<Policy,
                                          OneWay> ||
                               std::is_same_v<Policy,
                                              AnyWay>,
                           "Policy must be web::OneWay or web::AnyWay" );

            detail::AdjMap<Edge> adj_;

            // OneWay keeps a reverse adjacency map; AnyWay does not
            struct NoRev
            {
            };

            using RevType = std::conditional_t<std::is_same_v<Policy, OneWay>,
                                               detail::AdjMap<Edge>,
                                               NoRev>;
            [[no_unique_address]]
            RevType rev_adj_{};

            // ── Span helpers ────────────────────────────────────────────────

            template<typename Map>
            static std::span<const detail::AdjValue<Edge>>
            span_of( const Map& map,
                     Knot       knot )
            {
                auto it = map.find( knot );
                if( it == map.end() )
                {
                    return {};
                }
                return std::span<const detail::AdjValue<Edge>>{ it->second };
            }

        public:

            // ── Concept conformance ───────────────────────────────────────
            using knot_type   = Knot;
            using edge_type   = Edge;

            Web()             = default;
            ~Web()            = default;

            Web( const Web& ) = delete;
            Web&
            operator=( const Web& ) = delete;
            Web( Web&& ) noexcept   = default;
            Web&
            operator=( Web&& ) noexcept = default;

            // ── Add knot ────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            add( Knot knot )
            {
                logger::trace( logger::tag( "web.graph" ), "add() adding knot" );
                if( adj_.contains( knot ) )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "add() — knot already exists, busy" );
                    return out::Error::busy;
                }
                adj_[knot] = {};
                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    rev_adj_[knot] = {};
                }
                return out::Put<void, out::Error>{};
            }

            // ── Remove knot ─────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            rid( Knot knot )
            {
                logger::trace( logger::tag( "web.graph" ), "rid() removing knot" );
                auto it = adj_.find( knot );
                if( it == adj_.end() )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "rid() — knot not found" );
                    return out::Error::not_found;
                }

                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    // Remove knot from forward neighbors' reverse lists
                    for( const auto& entry : it->second )
                    {
                        auto target = detail::target_of<Edge>( entry );
                        auto rev_it = rev_adj_.find( target );
                        if( rev_it != rev_adj_.end() )
                        {
                            detail::remove_target<Edge>( rev_it->second, knot );
                        }
                    }
                    // Remove knot from reverse neighbors' forward lists
                    auto rev_it = rev_adj_.find( knot );
                    if( rev_it != rev_adj_.end() )
                    {
                        for( const auto& entry : rev_it->second )
                        {
                            auto source = detail::target_of<Edge>( entry );
                            auto fwd_it = adj_.find( source );
                            if( fwd_it != adj_.end() )
                            {
                                detail::remove_target<Edge>( fwd_it->second, knot );
                            }
                        }
                        rev_adj_.erase( rev_it );
                    }
                }
                else
                {
                    // AnyWay: remove knot from all neighbors' lists
                    for( const auto& entry : it->second )
                    {
                        auto neighbor = detail::target_of<Edge>( entry );
                        auto nb_it    = adj_.find( neighbor );
                        if( nb_it != adj_.end() )
                        {
                            detail::remove_target<Edge>( nb_it->second, knot );
                        }
                    }
                }

                adj_.erase( it );
                return out::Put<void, out::Error>{};
            }

            // ── Add edge (unweighted) ───────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            tie( Knot from,
                 Knot to )
            requires( std::is_void_v<Edge> )
            {
                logger::trace( logger::tag( "web.graph" ), "tie() adding edge" );
                if( from == to )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — self-loop rejected" );
                    return out::Error::wrong;
                }
                auto from_it = adj_.find( from );
                if( from_it == adj_.end() )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — source knot not found" );
                    return out::Error::not_found;
                }
                if( !adj_.contains( to ) )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — target knot not found" );
                    return out::Error::not_found;
                }
                if( detail::has_target<Edge>( from_it->second, to ) )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — edge already exists, busy" );
                    return out::Error::busy;
                }

                detail::sorted_insert<Edge>( from_it->second, to );

                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    detail::sorted_insert<Edge>( rev_adj_[to], from );
                }
                else
                {
                    detail::sorted_insert<Edge>( adj_[to], from );
                }
                return out::Put<void, out::Error>{};
            }

            // ── Add edge (weighted) ─────────────────────────────────────────

            template<typename E = Edge>
            [[nodiscard]]
            out::Put<void,
                     out::Error>
            tie( Knot     from,
                 Knot     to,
                 const E& data )
            requires( !std::is_void_v<Edge> ) && std::is_same_v<E,
                                                                Edge>
            {
                logger::trace( logger::tag( "web.graph" ),
                               "tie() adding weighted edge" );
                if( from == to )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — self-loop rejected" );
                    return out::Error::wrong;
                }
                auto from_it = adj_.find( from );
                if( from_it == adj_.end() )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — source knot not found" );
                    return out::Error::not_found;
                }
                if( !adj_.contains( to ) )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — target knot not found" );
                    return out::Error::not_found;
                }
                if( detail::has_target<Edge>( from_it->second, to ) )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "tie() — edge already exists, busy" );
                    return out::Error::busy;
                }

                detail::sorted_insert<Edge>( from_it->second,
                                             Neighbor<Edge>{ to, data } );

                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    detail::sorted_insert<Edge>( rev_adj_[to],
                                                 Neighbor<Edge>{ from, data } );
                }
                else
                {
                    detail::sorted_insert<Edge>( adj_[to],
                                                 Neighbor<Edge>{ from, data } );
                }
                return out::Put<void, out::Error>{};
            }

            // ── Remove edge ─────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            cut( Knot from,
                 Knot to )
            {
                logger::trace( logger::tag( "web.graph" ), "cut() removing edge" );
                auto from_it = adj_.find( from );
                if( from_it == adj_.end() )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "cut() — source knot not found" );
                    return out::Error::not_found;
                }
                if( !detail::has_target<Edge>( from_it->second, to ) )
                {
                    logger::error( logger::tag( "web.graph" ),
                                   "cut() — edge not found" );
                    return out::Error::not_found;
                }

                detail::remove_target<Edge>( from_it->second, to );

                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    auto rev_it = rev_adj_.find( to );
                    if( rev_it != rev_adj_.end() )
                    {
                        detail::remove_target<Edge>( rev_it->second, from );
                    }
                }
                else
                {
                    auto to_it = adj_.find( to );
                    if( to_it != adj_.end() )
                    {
                        detail::remove_target<Edge>( to_it->second, from );
                    }
                }
                return out::Put<void, out::Error>{};
            }

            // ── Outgoing neighbors (a -> ?) ─────────────────────────────────

            [[nodiscard]]
            auto
            out( Knot knot ) const -> std::span<const detail::AdjValue<Edge>>
            {
                return span_of( adj_, knot );
            }

            // ── Incoming neighbors (? -> a) ─────────────────────────────────

            [[nodiscard]]
            auto
            in( Knot knot ) const -> std::span<const detail::AdjValue<Edge>>
            {
                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    return span_of( rev_adj_, knot );
                }
                else
                {
                    return span_of( adj_, knot );
                }
            }

            // ── All neighbors (both directions) ─────────────────────────────

            [[nodiscard]]
            std::vector<Knot>
            kin( Knot knot ) const
            requires( std::is_void_v<Edge> )
            {
                if constexpr( std::is_same_v<Policy, AnyWay> )
                {
                    auto span = out( knot );
                    return std::vector<Knot>( span.begin(), span.end() );
                }
                else
                {
                    std::set<Knot>    seen;
                    std::vector<Knot> result;
                    for( auto k : out( knot ) )
                    {
                        if( seen.insert( k ).second )
                        {
                            result.push_back( k );
                        }
                    }
                    for( auto k : in( knot ) )
                    {
                        if( seen.insert( k ).second )
                        {
                            result.push_back( k );
                        }
                    }
                    return result;
                }
            }

            // ── Iteration ───────────────────────────────────────────────────

            [[nodiscard]]
            std::vector<Knot>
            knots() const
            {
                std::vector<Knot> result;
                result.reserve( adj_.size() );
                for( const auto& [k, _] : adj_ )
                {
                    result.push_back( k );
                }
                return result;
            }

            // ── Query ───────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            has( Knot knot ) const
            {
                return adj_.contains( knot );
            }

            [[nodiscard]]
            bool
            has( Knot from,
                 Knot to ) const
            {
                auto it = adj_.find( from );
                if( it == adj_.end() )
                {
                    return false;
                }
                return detail::has_target<Edge>( it->second, to );
            }

            [[nodiscard]]
            std::size_t
            size() const
            {
                return adj_.size();
            }

            [[nodiscard]]
            std::size_t
            ties() const
            {
                std::size_t total = 0;
                for( const auto& [_, list] : adj_ )
                {
                    total += list.size();
                }
                if constexpr( std::is_same_v<Policy, AnyWay> )
                {
                    return total / 2;
                }
                else
                {
                    return total;
                }
            }

            [[nodiscard]]
            bool
            is_empty() const
            {
                return adj_.empty();
            }

            // ── Modify ──────────────────────────────────────────────────────

            void
            empty()
            {
                logger::trace( logger::tag( "web.graph" ), "empty() clearing graph" );
                adj_.clear();
                if constexpr( std::is_same_v<Policy, OneWay> )
                {
                    rev_adj_.clear();
                }
            }
    };

}    // namespace web
