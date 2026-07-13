#pragma once
// ┌────────────────────────────────────────────────────────────────────────┐
// │  tag/context.h — opt-in uniqueness tracking for tag::Id<N>            │
// └────────────────────────────────────────────────────────────────────────┘
//
// tag::Pool<N, Policy> tracks a set of live ids and enforces uniqueness.
//
//   Policy = tag::multi_thread   (default) — std::mutex protects the set
//   Policy = tag::single_thread            — no locking overhead
//
// Generator methods call the underlying generator OUTSIDE the lock, then
// acquire the lock only for check-and-insert. This keeps the critical
// section minimal and avoids holding a lock across potentially slow calls.
//
// Methods:
//   random(rng)          — generate Id<N>, track it, return out::Put
//   timed(rng)           — generate Id<128> v7, track it  [requires N==128]
//   named(ns, name)      — generate Id<128> v5, track it  [requires N==128]
//   has(Id<N>)           — bool query, const
//   track(Id<N>)         — register external id; busy on duplicate
//   rid(Id<N>)           — deregister; not_found if absent
//   clear()              — rid all
//   size()               — number of tracked ids, const

#include <mutex>
#include <out/put.hpp>
#include <string_view>
#include <tag/gen.hpp>
#include <tag/rng.hpp>
#include <tag/tag.hpp>
#include <unordered_set>

namespace tag
{

    // ── Threading policy tags ─────────────────────────────────────────────────────

    struct multi_thread
    {
    };

    struct single_thread
    {
    };

    // ── detail: mutex/guard selection ─────────────────────────────────────────────

    namespace detail
    {

        template<typename Policy>
        struct lock_traits;

        template<>
        struct lock_traits<multi_thread>
        {
                using mutex_type = std::mutex;

                struct guard
                {
                        explicit guard( std::mutex& m ) :
                            lk( m )
                        {
                        }

                        std::unique_lock<std::mutex> lk;
                };
        };

        // Noop mutex and guard for single-threaded use.
        struct noop_mutex
        {
                void
                lock() noexcept
                {
                }

                void
                unlock() noexcept
                {
                }
        };

        template<>
        struct lock_traits<single_thread>
        {
                using mutex_type = noop_mutex;

                struct guard
                {
                        explicit guard( noop_mutex& /*unused*/ ) noexcept
                        {
                        }
                };
        };

    }    // namespace detail

    // ── Pool<N, Policy> ────────────────────────────────────────────────────────

    template<unsigned N, typename Policy = multi_thread>
    class Pool
    {
            using mutex_type = typename detail::lock_traits<Policy>::mutex_type;
            using guard_type = typename detail::lock_traits<Policy>::guard;

            std::unordered_set<Id<N>> set_;
            mutable mutex_type        mutex_;

        public:

            Pool()  = default;
            ~Pool() = default;

            // Non-copyable, non-movable (mutex and set make copy semantics awkward;
            // movability would require careful design; omit until needed).
            Pool( const Pool& ) = delete;
            Pool&
            operator=( const Pool& ) = delete;
            Pool( Pool&& )           = delete;
            Pool&
            operator=( Pool&& ) = delete;

            // ── random(rng) ──────────────────────────────────────────────────────────
            //
            // Generates outside the lock, then locks to check-and-insert.
            // Returns out::Error::busy on collision (astronomically rare but correct).

            template<rng_source Rng>
            [[nodiscard]]
            out::Put<Id<N>,
                     out::Error>
            random( Rng& rng )
            {
                Id<N>      candidate = tag::random<N>( rng );
                guard_type lock( mutex_ );
                auto [it, inserted] = set_.insert( candidate );
                if( !inserted )
                {
                    return out::Error::busy;
                }
                return candidate;
            }

            // ── timed(rng) ───────────────────────────────────────────────────────────

            template<rng_source Rng>
            [[nodiscard]]
            out::Put<Id<128>,
                     out::Error>
            timed( Rng& rng )
            requires( N == 128 )
            {
                Id<128>    candidate = tag::timed( rng );
                guard_type lock( mutex_ );
                auto [it, inserted] = set_.insert( candidate );
                if( !inserted )
                {
                    return out::Error::busy;
                }
                return candidate;
            }

            // ── named(ns, name) ──────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<Id<128>,
                     out::Error>
            named( const Id<128>&   ns,
                   std::string_view name )
            requires( N == 128 )
            {
                Id<128>    candidate = tag::named( ns, name );
                guard_type lock( mutex_ );
                auto [it, inserted] = set_.insert( candidate );
                if( !inserted )
                {
                    return out::Error::busy;
                }
                return candidate;
            }

            // ── has ─────────────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            has( const Id<N>& t ) const
            {
                guard_type lock( mutex_ );
                return set_.contains( t );
            }

            // ── track ────────────────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            track( const Id<N>& t )
            {
                guard_type lock( mutex_ );
                auto [it, inserted] = set_.insert( t );
                if( !inserted )
                {
                    return out::Error::busy;
                }
                return {};
            }

            // ── rid ───────────────────────────────────────────────────────────────

            [[nodiscard]]
            out::Put<void,
                     out::Error>
            rid( const Id<N>& t )
            {
                guard_type lock( mutex_ );
                auto       erased = set_.erase( t );
                if( erased == 0 )
                {
                    return out::Error::not_found;
                }
                return {};
            }

            // ── clear ────────────────────────────────────────────────────────────────

            void
            clear()
            {
                guard_type lock( mutex_ );
                set_.clear();
            }

            // ── size ─────────────────────────────────────────────────────────────────

            [[nodiscard]]
            size_t
            size() const
            {
                guard_type lock( mutex_ );
                return set_.size();
            }
    };

}    // namespace tag
