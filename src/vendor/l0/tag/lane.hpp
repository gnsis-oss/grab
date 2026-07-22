#pragma once
// ┌────────────────────────────────────────────────────────────────────────┐
// │  tag/lane.hpp — tag::IdLane<N> generational handle                    │
// └────────────────────────────────────────────────────────────────────────┘
//
// IdLane<N> packs an index and a generation lane into a single N-bit
// native integer.  Index occupies the lower N/2 bits, lane the upper N/2.
// Default-constructed state (index 0 + lane 0) is the nil sentinel.
//
// N must be 8, 16, 32, or 64 — mapping to uint8_t … uint64_t via
// detail::storage_t<N>.

#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <vector>

namespace tag
{

    // ── Storage type selection (shared with tag.hpp) ────────────────────────────

    namespace detail
    {

        template<unsigned N>
        struct lane_storage;

        // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        template<>
        struct lane_storage<4>
        {
                using type = uint8_t;
        };

        template<>
        struct lane_storage<8>
        {
                using type = uint8_t;
        };

        template<>
        struct lane_storage<16>
        {
                using type = uint16_t;
        };

        template<>
        struct lane_storage<32>
        {
                using type = uint32_t;
        };

        template<>
        struct lane_storage<64>
        {
                using type = uint64_t;
        };

        // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

        template<unsigned N>
        using lane_storage_t = typename lane_storage<N>::type;

    }    // namespace detail

    // ── IdLane<N> ───────────────────────────────────────────────────────────────

    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

    template<uint8_t N = 64>
    requires( N == 8 || N == 16 || N == 32 || N == 64 )
    class IdLane
    {
        public:

            using storage_type                      = detail::lane_storage_t<N>;
            using index_type                        = detail::lane_storage_t<N / 2>;
            using lane_type                         = detail::lane_storage_t<N / 2>;

            static constexpr uint8_t      half_bits = N / 2;
            static constexpr storage_type index_mask =
                // NOLINTNEXTLINE(readability-redundant-parentheses)
                static_cast<storage_type>( ( storage_type{ 1 } << half_bits ) - 1 );

        private:

            friend struct std::hash<IdLane<N>>;

            storage_type data_{};

        public:

            // ── Construction ────────────────────────────────────────────────────────

            constexpr IdLane() noexcept = default;

            constexpr IdLane( index_type index,
                              lane_type  lane ) noexcept :
                data_{ static_cast<storage_type>( static_cast<storage_type>( index ) |
                                                  static_cast<storage_type>( lane )
                                                  << half_bits ) }
            {
            }

            // ── Access ──────────────────────────────────────────────────────────────

            [[nodiscard]]
            constexpr index_type
            id() const noexcept
            {
                return static_cast<index_type>( data_ & index_mask );
            }

            [[nodiscard]]
            constexpr lane_type
            lane() const noexcept
            {
                return static_cast<lane_type>( data_ >> half_bits );
            }

            [[nodiscard]]
            constexpr bool
            nil() const noexcept
            {
                return data_ == storage_type{ 0 };
            }

            // ── Comparison ──────────────────────────────────────────────────────────

            [[nodiscard]]
            constexpr bool
            operator==( const IdLane& ) const noexcept = default;

            [[nodiscard]]
            constexpr auto
            operator<=>( const IdLane& ) const noexcept = default;
    };

    // ── Aliases ─────────────────────────────────────────────────────────────────

    using IdLane8  = IdLane<8>;
    using IdLane16 = IdLane<16>;
    using IdLane32 = IdLane<32>;
    using IdLane64 = IdLane<64>;

    // ── Size guarantees ─────────────────────────────────────────────────────────

    static_assert( sizeof( IdLane8 ) == 1 );
    static_assert( sizeof( IdLane16 ) == 2 );
    static_assert( sizeof( IdLane32 ) == 4 );
    static_assert( sizeof( IdLane64 ) == 8 );

    // ── Lanes<N> — dense slot allocator with generational recycling ───────────

    template<uint8_t N = 64>
    requires( N == 8 || N == 16 || N == 32 || N == 64 )
    class Lanes
    {
        public:

            using handle_type = IdLane<N>;
            using index_type  = typename handle_type::index_type;
            using lane_type   = typename handle_type::lane_type;

        private:

            std::vector<lane_type>
                lanes_{};    // lane counter per slot (indexed by id - 1)
            std::vector<index_type> free_{};     // stack of recycled indices
            std::vector<index_type> dense_{};    // contiguous live entity indices
            std::vector<uint32_t>
                       sparse_;       // index → position in dense_ (indexed by id - 1)
            index_type next_{ 1 };    // next fresh index (0 is reserved nil)
            uint32_t   live_{ 0 };    // count of currently live handles

        public:

            // ── Allocation ────────────────────────────────────────────────────────────

            [[nodiscard]]
            handle_type
            take()
            {
                index_type idx{};

                if( !free_.empty() )
                {
                    idx = free_.back();
                    free_.pop_back();
                }
                else
                {
                    idx = next_;
                    ++next_;
                    lanes_.emplace_back( lane_type{ 0 } );
                    sparse_.emplace_back( uint32_t{ 0 } );
                }

                auto& current_lane = lanes_[static_cast<std::size_t>( idx ) - 1];
                ++current_lane;

                // Track in dense/sparse arrays.
                sparse_[static_cast<std::size_t>( idx ) - 1] = live_;
                dense_.push_back( idx );
                ++live_;

                return handle_type{ idx, current_lane };
            }

            // ── Deallocation ──────────────────────────────────────────────────────────

            void
            rid( handle_type handle )
            {
                if( !has( handle ) )
                {
                    return;
                }

                const auto slot = static_cast<std::size_t>( handle.id() ) - 1;

                // Swap-and-pop from dense array.
                const auto dense_pos = sparse_[slot];
                const auto last_pos  = live_ - 1;

                if( dense_pos != last_pos )
                {
                    const auto moved_idx = dense_[last_pos];
                    dense_[dense_pos]    = moved_idx;
                    sparse_[static_cast<std::size_t>( moved_idx ) - 1] = dense_pos;
                }

                dense_.pop_back();

                // Bump lane to invalidate the old handle, then recycle the slot.
                ++lanes_[slot];
                free_.push_back( handle.id() );
                --live_;
            }

            // ── Queries ───────────────────────────────────────────────────────────────

            [[nodiscard]]
            bool
            has( handle_type handle ) const noexcept
            {
                if( handle.nil() )
                {
                    return false;
                }

                const auto idx = static_cast<std::size_t>( handle.id() );

                if( idx == 0 || idx >= static_cast<std::size_t>( next_ ) )
                {
                    return false;
                }

                return lanes_[idx - 1] == handle.lane();
            }

            [[nodiscard]]
            uint32_t
            size() const noexcept
            {
                return live_;
            }

            [[nodiscard]]
            bool
            is_empty() const noexcept
            {
                return live_ == 0;
            }

            // ── Iteration ─────────────────────────────────────────────────────────────

            template<typename F>
            void
            each( const F& func ) const
            {
                for( uint32_t i = 0; i < live_; ++i )
                {
                    const auto idx  = dense_[i];
                    const auto lane = lanes_[static_cast<std::size_t>( idx ) - 1];
                    func( handle_type{ idx, lane } );
                }
            }

            /// Iterate a range [begin, end) of the dense array.
            template<typename F>
            void
            each_range( uint32_t begin,
                        uint32_t end,
                        const F& func ) const
            {
                for( uint32_t i = begin; i < end && i < live_; ++i )
                {
                    const auto idx  = dense_[i];
                    const auto lane = lanes_[static_cast<std::size_t>( idx ) - 1];
                    func( handle_type{ idx, lane } );
                }
            }

            /// Expose live count for range splitting.
            [[nodiscard]]
            uint32_t
            live_tally() const noexcept
            {
                return live_;
            }

            // ── Capacity ──────────────────────────────────────────────────────────────

            void
            make_room( uint32_t capacity )
            {
                const auto cap = static_cast<std::size_t>( capacity );
                lanes_.reserve( cap );
                free_.reserve( cap );
                dense_.reserve( cap );
                sparse_.reserve( cap );
            }
    };

    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

}    // namespace tag

// ── std::hash specialization ────────────────────────────────────────────────

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)

template<uint8_t N>
requires( N == 8 || N == 16 || N == 32 || N == 64 )
struct std::hash<tag::IdLane<N>>
{
        std::size_t
        operator()( const tag::IdLane<N>& v ) const noexcept
        {
            return std::hash<typename tag::IdLane<N>::storage_type>{}( v.data_ );
        }
};

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
