#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  kin/kin.h -- kin::Kin<Union, Find> disjoint-set / union-find       │
// └──────────────────────────────────────────────────────────────────────┘
//
// Two flat arrays (parent + rank/size). Policy-based union strategy
// (ByRank or BySize) and find strategy (Halve, Full, or Split path
// compression). Near-constant amortized time per operation.
// Zero dependencies beyond the standard library.

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace walk::detail
{

    // ── Union policies ──────────────────────────────────────────────────

    struct ByRank
    {
    };    // attach smaller-rank tree under larger-rank root

    struct BySize
    {
    };    // attach smaller-count tree under larger-count root

    // ── Find policies ───────────────────────────────────────────────────

    struct Halve
    {
    };    // path halving: skip every other node

    struct Full
    {
    };    // full path compression: all nodes point to root

    struct Split
    {
    };    // path splitting: each node points to grandparent

    // ── Kin ─────────────────────────────────────────────────────────────

    template<typename Union = ByRank, typename Find = Halve>
    class Kin
    {
            static_assert( std::is_same_v<Union,
                                          ByRank> ||
                               std::is_same_v<Union,
                                              BySize>,
                           "Union policy must be kin::ByRank or kin::BySize" );
            static_assert( std::is_same_v<Find,
                                          Halve> ||
                               std::is_same_v<Find,
                                              Full> ||
                               std::is_same_v<Find,
                                              Split>,
                           "Find policy must be kin::Halve, kin::Full, or kin::Split" );

            std::vector<int> parent_;
            std::vector<int> weight_;    // rank (ByRank) or size (BySize)
            std::size_t      sets_;

            // Safe index cast from int to size_t
            static std::size_t
            ix( int i )
            {
                return static_cast<std::size_t>( i );
            }

        public:

            // ── Construction ────────────────────────────────────────────────

            explicit Kin( std::size_t n ) :
                parent_( n ),
                weight_( n,
                         std::is_same_v<Union,
                                        BySize>
                             ? 1
                             : 0 ),
                sets_{ n }
            {
                for( std::size_t i = 0; i < n; ++i )
                {
                    parent_[i] = static_cast<int>( i );
                }
            }

            ~Kin()            = default;

            Kin( const Kin& ) = default;
            Kin&
            operator=( const Kin& ) = default;
            Kin( Kin&& ) noexcept   = default;
            Kin&
            operator=( Kin&& ) noexcept = default;

            // ── Find ────────────────────────────────────────────────────────

            [[nodiscard]]
            int
            find( int x )
            {
                if constexpr( std::is_same_v<Find, Halve> )
                {
                    // Path halving: make every other node point to its grandparent
                    while( parent_[ix( x )] != x )
                    {
                        parent_[ix( x )] = parent_[ix( parent_[ix( x )] )];
                        x                = parent_[ix( x )];
                    }
                }
                else if constexpr( std::is_same_v<Find, Full> )
                {
                    // Full path compression: two passes
                    int root = x;
                    while( parent_[ix( root )] != root )
                    {
                        root = parent_[ix( root )];
                    }
                    while( x != root )
                    {
                        int next         = parent_[ix( x )];
                        parent_[ix( x )] = root;
                        x                = next;
                    }
                }
                else
                {
                    // Path splitting: each node points to its grandparent
                    while( parent_[ix( x )] != x )
                    {
                        int next         = parent_[ix( x )];
                        parent_[ix( x )] = parent_[ix( next )];
                        x                = next;
                    }
                }
                return x;
            }

            // ── Bond ────────────────────────────────────────────────────────

            void
            bond( int x,
                  int y )
            {
                int rx = find( x );
                int ry = find( y );
                if( rx == ry )
                {
                    return;
                }

                if constexpr( std::is_same_v<Union, ByRank> )
                {
                    // Attach smaller-rank tree under larger-rank root
                    if( weight_[ix( rx )] < weight_[ix( ry )] )
                    {
                        std::swap( rx, ry );
                    }
                    parent_[ix( ry )] = rx;
                    if( weight_[ix( rx )] == weight_[ix( ry )] )
                    {
                        ++weight_[ix( rx )];
                    }
                }
                else
                {
                    // Attach smaller-count tree under larger-count root
                    if( weight_[ix( rx )] < weight_[ix( ry )] )
                    {
                        std::swap( rx, ry );
                    }
                    parent_[ix( ry )]  = rx;
                    weight_[ix( rx )] += weight_[ix( ry )];
                }

                --sets_;
            }

            // ── Query ───────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            same( int x,
                  int y )
            {
                return find( x ) == find( y );
            }

            [[nodiscard]]
            std::size_t
            sets() const
            {
                return sets_;
            }

            [[nodiscard]]
            std::size_t
            size() const
            {
                return parent_.size();
            }
    };

}    // namespace walk::detail
