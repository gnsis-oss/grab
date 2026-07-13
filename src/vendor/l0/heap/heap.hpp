#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  heap/heap.h -- heap::Heap<T, Order, Arity> indexed d-ary heap      │
// └──────────────────────────────────────────────────────────────────────┘
//
// A flat, cache-friendly priority queue with O(1) ID lookup, O(log n)
// add/pop/rid/rank. Uses Trait<T> for id/key extraction and out::Put
// for fallible operations. Min puts smallest key on top; Max puts
// largest key on top.
//
// Arity controls branching factor: Binary (d=2, default), Quad (d=4),
// Oct (d=8). Higher arity means shallower tree (faster sift-up / add)
// but more comparisons per sift-down level (slower pop).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <heap/trait.hpp>
#include <log/writer.hpp>
#include <out/put.hpp>
#include <tag/tag.hpp>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heap
{

    // ── Order tags ──────────────────────────────────────────────────────────

    struct Min
    {
    };    // smallest key on top

    struct Max
    {
    };    // largest key on top

    // ── Arity tags ──────────────────────────────────────────────────────────

    struct Binary
    {
    };    // branching factor 2 (default)

    struct Quad
    {
    };    // branching factor 4 (cache-friendly, better for add-heavy)

    struct Oct
    {
    };    // branching factor 8

    // ── Heap ────────────────────────────────────────────────────────────────

    template<typename T, typename Order = Min, typename Arity = Binary>
    requires Heapable<T>
    class Heap
    {
            // NOLINTBEGIN(readability-identifier-naming): D is the canonical
            // single-letter math symbol for d-ary heap branching factor.
            static constexpr int32_t D = []
            {
                if constexpr( std::is_same_v<Arity, Binary> )
                {
                    return 2;
                }
                else if constexpr( std::is_same_v<Arity, Quad> )
                {
                    return 4;
                }
                else
                {
                    return 8;
                }
            }();
            // NOLINTEND(readability-identifier-naming)

            static_assert( std::is_same_v<Arity,
                                          Binary> ||
                               std::is_same_v<Arity,
                                              Quad> ||
                               std::is_same_v<Arity,
                                              Oct>,
                           "Arity must be Binary, Quad, or Oct" );

            std::vector<T>                               items_;
            std::unordered_map<tag::Id<64>, std::size_t> index_;

            // ── Comparison ──────────────────────────────────────────────────────

            bool
            less( std::size_t a,
                  std::size_t b ) const
            {
                auto ka = Trait<T>::key( items_[a] );
                auto kb = Trait<T>::key( items_[b] );
                if constexpr( std::is_same_v<Order, Min> )
                {
                    return ka < kb;
                }
                else
                {
                    return ka > kb;
                }
            }

            // ── Swap with index update ──────────────────────────────────────────

            void
            swap_entries( std::size_t a,
                          std::size_t b )
            {
                std::swap( items_[a], items_[b] );
                index_[Trait<T>::id( items_[a] )] = a;
                index_[Trait<T>::id( items_[b] )] = b;
            }

            // ── Sift operations ─────────────────────────────────────────────────

            void
            sift_up( std::size_t i )
            {
                while( i > 0 )
                {
                    std::size_t parent = ( i - 1 ) / D;
                    if( less( i, parent ) )
                    {
                        swap_entries( i, parent );
                        i = parent;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            void
            sift_down( std::size_t i )
            {
                while( true )
                {
                    std::size_t best        = i;
                    std::size_t first_child = ( D * i ) + 1;

                    // Check all D children
                    std::size_t limit =
                        std::min( first_child + static_cast<std::size_t>( D ),
                                  items_.size() );
                    for( std::size_t c = first_child; c < limit; ++c )
                    {
                        if( less( c, best ) )
                        {
                            best = c;
                        }
                    }

                    if( best == i )
                    {
                        break;
                    }
                    swap_entries( i, best );
                    i = best;
                }
            }

        public:

            Heap() = default;

            explicit Heap( std::size_t initial_capacity )
            {
                items_.reserve( initial_capacity );
                index_.reserve( initial_capacity );
            }

            ~Heap()             = default;

            Heap( const Heap& ) = delete;
            Heap&
            operator=( const Heap& ) = delete;
            Heap( Heap&& ) noexcept  = default;
            Heap&
            operator=( Heap&& ) noexcept = default;

            // ── Insert ──────────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            add( const T& item )
            {
                logger::trace( logger::tag( "heap.heap" ), "add() entering" );
                auto id = Trait<T>::id( item );
                if( index_.contains( id ) )
                {
                    logger::error( logger::tag( "heap.heap" ),
                                   "add() — duplicate id, busy" );
                    return out::Error::busy;
                }
                items_.push_back( item );
                std::size_t pos = items_.size() - 1;
                index_[id]      = pos;
                sift_up( pos );
                logger::trace( logger::tag( "heap.heap" ), "add() returning ok" );
                return out::Put<void, out::Error>{};
            }

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            add( T&& item )
            {
                logger::trace( logger::tag( "heap.heap" ), "add() entering" );
                auto id = Trait<T>::id( item );
                if( index_.contains( id ) )
                {
                    logger::error( logger::tag( "heap.heap" ),
                                   "add() — duplicate id, busy" );
                    return out::Error::busy;
                }
                items_.push_back( std::move( item ) );
                std::size_t pos = items_.size() - 1;
                index_[id]      = pos;
                sift_up( pos );
                logger::trace( logger::tag( "heap.heap" ), "add() returning ok" );
                return out::Put<void, out::Error>{};
            }

            // ── Remove top ──────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<T,
                     out::Error>
            pop()
            {
                logger::trace( logger::tag( "heap.heap" ), "pop() entering" );
                if( items_.empty() )
                {
                    logger::error( logger::tag( "heap.heap" ),
                                   "pop() — empty heap, not_found" );
                    return out::Error::not_found;
                }
                T    result = std::move( items_[0] );
                auto id     = Trait<T>::id( result );
                index_.erase( id );

                if( items_.size() > 1 )
                {
                    items_[0] = std::move( items_.back() );
                    items_.pop_back();
                    index_[Trait<T>::id( items_[0] )] = 0;
                    sift_down( 0 );
                }
                else
                {
                    items_.pop_back();
                }
                logger::trace( logger::tag( "heap.heap" ), "pop() returning item" );
                return result;
            }

            // ── Peek ────────────────────────────────────────────────────────────

            [[nodiscard]]
            const T*
            top() const
            {
                if( items_.empty() )
                {
                    return nullptr;
                }
                return items_.data();
            }

            // ── Remove by ID ────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<T,
                     out::Error>
            rid( tag::Id<64> id )
            {
                logger::trace( logger::tag( "heap.heap" ), "rid() entering" );
                auto it = index_.find( id );
                if( it == index_.end() )
                {
                    logger::error( logger::tag( "heap.heap" ), "rid() — id not found" );
                    return out::Error::not_found;
                }

                std::size_t pos    = it->second;
                T           result = std::move( items_[pos] );
                index_.erase( it );

                if( pos < items_.size() - 1 )
                {
                    items_[pos] = std::move( items_.back() );
                    items_.pop_back();
                    index_[Trait<T>::id( items_[pos] )] = pos;
                    sift_up( pos );
                    sift_down( pos );
                }
                else
                {
                    items_.pop_back();
                }
                logger::trace( logger::tag( "heap.heap" ), "rid() returning item" );
                return result;
            }

            // ── Rank (replace item, re-heapify) ─────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            rank( const T& item )
            {
                logger::trace( logger::tag( "heap.heap" ), "rank() entering" );
                auto id = Trait<T>::id( item );
                auto it = index_.find( id );
                if( it == index_.end() )
                {
                    logger::error( logger::tag( "heap.heap" ), "rank() — id not found" );
                    return out::Error::not_found;
                }

                std::size_t pos = it->second;
                items_[pos]     = item;
                sift_up( pos );
                sift_down( pos );
                logger::trace( logger::tag( "heap.heap" ), "rank() returning ok" );
                return out::Put<void, out::Error>{};
            }

            // ── Query ───────────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            has( tag::Id<64> id ) const
            {
                return index_.contains( id );
            }

            [[nodiscard]]
            std::size_t
            size() const
            {
                return items_.size();
            }

            [[nodiscard]]
            bool
            is_empty() const
            {
                return items_.empty();
            }

            // ── Modify ──────────────────────────────────────────────────────────

            void
            empty()
            {
                items_.clear();
                index_.clear();
            }

            // ── Capacity ────────────────────────────────────────────────────────

            void
            make_room( std::size_t capacity )
            {
                items_.reserve( capacity );
                index_.reserve( capacity );
            }
    };

}    // namespace heap
