#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  heap/fast/heap.h -- SoA, prefetch, branchless sift d-ary heap     │
// └──────────────────────────────────────────────────────────────────────┘
//
// Round 2 optimizations:
//   - SoA layout: separate keys_ vector for cache-hot sift comparisons
//   - Prefetch grandchildren during sift-down
//   - Hole-based sift: single item save, slide path, place once
//   - Branchless min/max child selection via sequential cmov
//   - Indexed: flat open-addressing hash table (no unordered_map)
//   - Unindexed: no index at all (closest to std::priority_queue)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <heap/heap.hpp>
#include <heap/trait.hpp>
#include <out/detail/platform.hpp>
#include <out/put.hpp>
#include <tag/tag.hpp>
#include <type_traits>
#include <utility>
#include <vector>

namespace heap::fast
{

    // Reuse order/arity tags from parent namespace
    using heap::Binary;
    using heap::Max;
    using heap::Min;
    using heap::Oct;
    using heap::Quad;

    // ── Mode tags ────────────────────────────────────────────────────────────

    struct Indexed
    {
    };    // maintains ID->position map (supports rid/has)

    struct Unindexed
    {
    };    // no map -- faster push/pop, no rid/has

    // ── Flat open-addressing hash table for ID->position ─────────────────────

    namespace detail
    {

        class FlatIndex
        {
                // Robin Hood-free open addressing with linear probing
                // Sentinels: position == SIZE_MAX means empty
                struct Slot
                {
                        tag::Id<64> id{};
                        std::size_t pos = kEmpty;
                };

                static constexpr std::size_t kEmpty = ~std::size_t{ 0 };
                std::vector<Slot>            table_;
                std::size_t                  mask_  = 0;
                std::size_t                  count_ = 0;

                std::size_t
                hash( tag::Id<64> id ) const
                {
                    return std::hash<tag::Id<64>>{}( id )&mask_;
                }

                void
                grow()
                {
                    std::size_t       new_cap = table_.empty() ? 64 : table_.size() * 2;
                    std::vector<Slot> old;
                    old.swap( table_ );
                    table_.resize( new_cap, Slot{} );
                    mask_  = new_cap - 1;
                    count_ = 0;
                    for( auto& s : old )
                    {
                        if( s.pos != kEmpty )
                        {
                            set( s.id, s.pos );
                        }
                    }
                }

            public:

                FlatIndex() = default;

                void
                reserve( std::size_t n )
                {
                    std::size_t cap = 64;
                    while( cap < n * 2 )
                    {
                        cap <<= 1;
                    }
                    table_.resize( cap, Slot{} );
                    mask_ = cap - 1;
                }

                void
                set( tag::Id<64> id,
                     std::size_t pos )
                {
                    if( count_ * 4 >= table_.size() * 3 )
                    {
                        grow();    // load factor 0.75
                    }
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( table_[i].pos == kEmpty )
                        {
                            table_[i] = { id, pos };
                            ++count_;
                            return;
                        }
                        if( table_[i].id == id )
                        {
                            table_[i].pos = pos;
                            return;
                        }
                        i = ( i + 1 ) & mask_;
                    }
                }

                [[nodiscard]]
                bool
                has( tag::Id<64> id ) const
                {
                    if( table_.empty() )
                    {
                        return false;
                    }
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( table_[i].pos == kEmpty )
                        {
                            return false;
                        }
                        if( table_[i].id == id )
                        {
                            return true;
                        }
                        i = ( i + 1 ) & mask_;
                    }
                }

                [[nodiscard]]
                std::size_t
                get( tag::Id<64> id ) const
                {
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( table_[i].id == id )
                        {
                            return table_[i].pos;
                        }
                        i = ( i + 1 ) & mask_;
                    }
                }

                // Returns true if found and erased
                bool
                erase( tag::Id<64> id )
                {
                    if( table_.empty() )
                    {
                        return false;
                    }
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( table_[i].pos == kEmpty )
                        {
                            return false;
                        }
                        if( table_[i].id == id )
                        {
                            // Backward-shift deletion
                            table_[i].pos = kEmpty;
                            --count_;
                            // Rehash subsequent entries
                            std::size_t j = ( i + 1 ) & mask_;
                            while( table_[j].pos != kEmpty )
                            {
                                auto slot     = table_[j];
                                table_[j].pos = kEmpty;
                                --count_;
                                set( slot.id, slot.pos );
                                j = ( j + 1 ) & mask_;
                            }
                            return true;
                        }
                        i = ( i + 1 ) & mask_;
                    }
                }

                void
                clear()
                {
                    for( auto& s : table_ )
                    {
                        s.pos = kEmpty;
                    }
                    count_ = 0;
                }
        };

        struct EmptyIndex
        {
                void
                reserve( std::size_t )
                {
                }

                void
                set( tag::Id<64>,
                     std::size_t )
                {
                }

                void
                clear()
                {
                }
        };

    }    // namespace detail

    // ── Heap ────────────────────────────────────────────────────────────────

    template<typename T,
             typename Order = Min,
             typename Arity = Binary,
             typename Mode  = Indexed>
    requires Heapable<T>
    class Heap
    {
            static constexpr int32_t D = std::is_same_v<Arity, Binary> ? 2
                                       : std::is_same_v<Arity, Quad>   ? 4
                                                                       : 8;

            static_assert( std::is_same_v<Arity,
                                          Binary> ||
                               std::is_same_v<Arity,
                                              Quad> ||
                               std::is_same_v<Arity,
                                              Oct>,
                           "Arity must be Binary, Quad, or Oct" );

            static constexpr bool indexed = std::is_same_v<Mode, Indexed>;

            using KeyType = decltype( Trait<T>::key( std::declval<const T&>() ) );

            // SoA layout: keys hot, items cold
            std::vector<KeyType> keys_;     // hot: compared during sift
            std::vector<T>       items_;    // cold: moved only on swap

            // Conditional ID index
            [[no_unique_address]]
            std::conditional_t<indexed, detail::FlatIndex, detail::EmptyIndex> index_;

            // ── Comparison (operates on keys_ array) ─────────────────────────────

            [[gnu::always_inline]]
            bool
            key_less( std::size_t a,
                      std::size_t b ) const
            {
                if constexpr( std::is_same_v<Order, Min> )
                {
                    return keys_[a] < keys_[b];
                }
                else
                {
                    return keys_[a] > keys_[b];
                }
            }

            [[gnu::always_inline]]
            bool
            key_better( KeyType a,
                        KeyType b ) const
            {
                if constexpr( std::is_same_v<Order, Min> )
                {
                    return a < b;
                }
                else
                {
                    return a > b;
                }
            }

            // ── Index update helper ─────────────────────────────────────────────

            void
            index_set( std::size_t pos )
            {
                if constexpr( indexed )
                {
                    index_.set( Trait<T>::id( items_[pos] ), pos );
                }
            }

            // ── Swap (SoA: swap both keys and items) ────────────────────────────

            [[gnu::always_inline]]
            void
            swap_at( std::size_t a,
                     std::size_t b )
            {
                std::swap( keys_[a], keys_[b] );
                std::swap( items_[a], items_[b] );
            }

            // ── Hole-based sift-up: save item, slide parents down, place once ───

            SEED_ALWAYS_INLINE void
            sift_up( std::size_t i )
            {
                T       item     = std::move( items_[i] );
                KeyType item_key = keys_[i];

                while( i > 0 )
                {
                    std::size_t parent = ( i - 1 ) / D;
                    if( key_better( item_key, keys_[parent] ) )
                    {
                        keys_[i]  = keys_[parent];
                        items_[i] = std::move( items_[parent] );
                        index_set( i );
                        i = parent;
                    }
                    else
                    {
                        break;
                    }
                }
                keys_[i]  = item_key;
                items_[i] = std::move( item );
                index_set( i );
            }

            // ── Hole-based sift-down with prefetch ──────────────────────────────

            SEED_ALWAYS_INLINE void
            sift_down( std::size_t i )
            {
                auto    n        = keys_.size();
                T       item     = std::move( items_[i] );
                KeyType item_key = keys_[i];

                while( true )
                {
                    std::size_t first_child = D * i + 1;
                    if( first_child >= n )
                    {
                        break;
                    }

                    // Prefetch grandchildren -- hide memory latency
                    std::size_t first_grandchild = D * first_child + 1;
                    if( first_grandchild < n )
                    {
                        SEED_PREFETCH( &keys_[first_grandchild] );
                    }

                    std::size_t limit =
                        std::min( first_child + static_cast<std::size_t>( D ), n );

                    // Find best child via sequential comparison (cmov-friendly)
                    std::size_t best = first_child;
                    for( std::size_t c = first_child + 1; c < limit; ++c )
                    {
                        best = key_less( c, best ) ? c : best;
                    }

                    // Compare best child against the saved item's key
                    if( !key_better( keys_[best], item_key ) )
                    {
                        break;
                    }

                    // Prefetch best child's children for next iteration
                    SEED_PREFETCH( &keys_[D * best + 1] );

                    keys_[i]  = keys_[best];
                    items_[i] = std::move( items_[best] );
                    index_set( i );
                    i = best;
                }

                keys_[i]  = item_key;
                items_[i] = std::move( item );
                index_set( i );
            }

        public:

            Heap() = default;

            explicit Heap( std::size_t initial_capacity )
            {
                keys_.reserve( initial_capacity );
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
                auto id = Trait<T>::id( item );
                if constexpr( indexed )
                {
                    if( index_.has( id ) )
                    {
                        return out::Error::busy;
                    }
                }
                keys_.push_back( Trait<T>::key( item ) );
                items_.push_back( item );
                std::size_t pos = items_.size() - 1;
                index_set( pos );
                sift_up( pos );
                return out::Put<void, out::Error>{};
            }

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            add( T&& item )
            {
                auto id = Trait<T>::id( item );
                if constexpr( indexed )
                {
                    if( index_.has( id ) )
                    {
                        return out::Error::busy;
                    }
                }
                keys_.push_back( Trait<T>::key( item ) );
                items_.push_back( std::move( item ) );
                std::size_t pos = items_.size() - 1;
                index_set( pos );
                sift_up( pos );
                return out::Put<void, out::Error>{};
            }

            // ── Remove top ──────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<T,
                     out::Error>
            pop()
            {
                if( items_.empty() )
                {
                    return out::Error::not_found;
                }
                T result = std::move( items_[0] );
                if constexpr( indexed )
                {
                    index_.erase( Trait<T>::id( result ) );
                }

                if( items_.size() > 1 )
                {
                    keys_[0]  = keys_.back();
                    items_[0] = std::move( items_.back() );
                    keys_.pop_back();
                    items_.pop_back();
                    index_set( 0 );
                    sift_down( 0 );
                }
                else
                {
                    keys_.pop_back();
                    items_.pop_back();
                }
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
                return &items_[0];
            }

            // ── Remove by ID (Indexed mode only) ────────────────────────────────

            [[nodiscard]]
            out::Put<T,
                     out::Error>
            rid( tag::Id<64> id )
            requires( indexed )
            {
                if( !index_.has( id ) )
                {
                    return out::Error::not_found;
                }
                std::size_t pos    = index_.get( id );
                T           result = std::move( items_[pos] );
                index_.erase( id );

                if( pos < items_.size() - 1 )
                {
                    keys_[pos]  = keys_.back();
                    items_[pos] = std::move( items_.back() );
                    keys_.pop_back();
                    items_.pop_back();
                    index_set( pos );
                    sift_up( pos );
                    sift_down( pos );
                }
                else
                {
                    keys_.pop_back();
                    items_.pop_back();
                }
                return result;
            }

            // ── Rank (replace item, re-heapify) (Indexed mode only) ─────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            rank( const T& item )
            requires( indexed )
            {
                auto id = Trait<T>::id( item );
                if( !index_.has( id ) )
                {
                    return out::Error::not_found;
                }
                std::size_t pos = index_.get( id );
                keys_[pos]      = Trait<T>::key( item );
                items_[pos]     = item;
                sift_up( pos );
                sift_down( pos );
                return out::Put<void, out::Error>{};
            }

            // ── Query ───────────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            has( tag::Id<64> id ) const
            requires( indexed )
            {
                return index_.has( id );
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
                keys_.clear();
                items_.clear();
                index_.clear();
            }

            // ── Capacity ────────────────────────────────────────────────────────

            void
            make_room( std::size_t capacity )
            {
                keys_.reserve( capacity );
                items_.reserve( capacity );
                index_.reserve( capacity );
            }
    };

}    // namespace heap::fast
