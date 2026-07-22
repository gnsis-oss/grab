#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  tag/fast/pool.h -- Flat open-addressing hash set for Id<N>        │
// └──────────────────────────────────────────────────────────────────────┘
//
// Replaces unordered_set with power-of-2 open-addressing flat hash table.
// Linear probing with backward-shift deletion.
// SEED_PREFETCH on probe sequence.

#include <cstddef>
#include <functional>
#include <mutex>
#include <out/detail/platform.hpp>
#include <out/put.hpp>
#include <tag/gen.hpp>
#include <tag/rng.hpp>
#include <tag/tag.hpp>
#include <vector>

namespace tag::fast
{

    // Reuse threading policies from parent namespace
    using tag::multi_thread;
    using tag::single_thread;

    namespace detail
    {

        using tag::detail::lock_traits;

        // ── Flat open-addressing hash set ───────────────────────────────────────

        template<unsigned N>
        class FlatSet
        {
                struct Slot
                {
                        Id<N> id{};
                        bool  occupied = false;
                };

                std::vector<Slot> table_;
                std::size_t       mask_  = 0;
                std::size_t       count_ = 0;

                std::size_t
                hash( const Id<N>& id ) const
                {
                    return std::hash<Id<N>>{}( id )&mask_;
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
                        if( s.occupied )
                        {
                            insert_no_check( s.id );
                        }
                    }
                }

                void
                insert_no_check( const Id<N>& id )
                {
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( !table_[i].occupied )
                        {
                            table_[i] = { id, true };
                            ++count_;
                            return;
                        }
                        i = ( i + 1 ) & mask_;
                    }
                }

            public:

                FlatSet() = default;

                bool
                add( const Id<N>& id )
                {
                    if( count_ * 4 >= table_.size() * 3 || table_.empty() )
                    {
                        grow();
                    }
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( !table_[i].occupied )
                        {
                            table_[i] = { id, true };
                            ++count_;
                            return true;
                        }
                        if( table_[i].id == id )
                        {
                            return false;    // duplicate
                        }
                        SEED_PREFETCH( &table_[( i + 1 ) & mask_] );
                        i = ( i + 1 ) & mask_;
                    }
                }

                bool
                has( const Id<N>& id ) const
                {
                    if( table_.empty() )
                    {
                        return false;
                    }
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( !table_[i].occupied )
                        {
                            return false;
                        }
                        if( table_[i].id == id )
                        {
                            return true;
                        }
                        SEED_PREFETCH( &table_[( i + 1 ) & mask_] );
                        i = ( i + 1 ) & mask_;
                    }
                }

                bool
                erase( const Id<N>& id )
                {
                    if( table_.empty() )
                    {
                        return false;
                    }
                    std::size_t i = hash( id );
                    while( true )
                    {
                        if( !table_[i].occupied )
                        {
                            return false;
                        }
                        if( table_[i].id == id )
                        {
                            table_[i].occupied = false;
                            --count_;
                            // Backward-shift deletion
                            std::size_t j = ( i + 1 ) & mask_;
                            while( table_[j].occupied )
                            {
                                std::size_t natural = hash( table_[j].id );
                                // Check if j's natural position is at or before i
                                // (accounting for wrap-around)
                                bool displaced = ( j > i )
                                                   ? ( natural <= i || natural > j )
                                                   : ( natural <= i && natural > j );
                                if( displaced )
                                {
                                    table_[i]          = table_[j];
                                    table_[j].occupied = false;
                                    i                  = j;
                                }
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
                        s.occupied = false;
                    }
                    count_ = 0;
                }

                std::size_t
                size() const
                {
                    return count_;
                }
        };

    }    // namespace detail

    // ── Pool<N, Policy> ─────────────────────────────────────────────────────

    template<unsigned N, typename Policy = multi_thread>
    class Pool
    {
            using mutex_type = typename tag::detail::lock_traits<Policy>::mutex_type;
            using guard_type = typename tag::detail::lock_traits<Policy>::guard;

            detail::FlatSet<N> set_;
            mutable mutex_type mutex_;

        public:

            Pool()              = default;
            ~Pool()             = default;

            Pool( const Pool& ) = delete;
            Pool&
            operator=( const Pool& ) = delete;
            Pool( Pool&& )           = delete;
            Pool&
            operator=( Pool&& ) = delete;

            template<rng_source Rng>
            [[nodiscard]]
            out::Put<Id<N>,
                     out::Error>
            random( Rng& rng )
            {
                Id<N>      candidate = tag::random<N>( rng );
                guard_type lock( mutex_ );
                if( !set_.add( candidate ) )
                {
                    return out::Error::busy;
                }
                return candidate;
            }

            [[nodiscard]]
            bool
            has( const Id<N>& t ) const
            {
                guard_type lock( mutex_ );
                return set_.has( t );
            }

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            track( const Id<N>& t )
            {
                guard_type lock( mutex_ );
                if( !set_.add( t ) )
                {
                    return out::Error::busy;
                }
                return {};
            }

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            rid( const Id<N>& t )
            {
                guard_type lock( mutex_ );
                if( !set_.erase( t ) )
                {
                    return out::Error::not_found;
                }
                return {};
            }

            void
            clear()
            {
                guard_type lock( mutex_ );
                set_.clear();
            }

            [[nodiscard]]
            std::size_t
            size() const
            {
                guard_type lock( mutex_ );
                return set_.size();
            }
    };

}    // namespace tag::fast
