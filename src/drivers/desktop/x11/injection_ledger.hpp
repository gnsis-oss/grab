#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace grab::drivers::desktop::x11
{

    enum class InjectionKind : std::uint8_t
    {
        ButtonPress,
        ButtonRelease,
        KeyPress,
        KeyRelease,
        Motion,
    };

    class InjectionLedger
    {
        public:

            static constexpr std::chrono::milliseconds entryLifetime{ 500 };
            static constexpr std::size_t               capacity = 64U;

            void
            record( InjectionKind kind,
                    std::uint32_t detail )
            {
                const std::scoped_lock lock( mutex_ );
                const Entry            entry{
                    .kind   = kind,
                    .detail = detail,
                    .at     = std::chrono::steady_clock::now(),
                };

                if( size_ == capacity )
                {
                    entries_.at( head_ ) = entry;
                    head_                = index_at( 1U );
                    return;
                }

                entries_.at( index_at( size_ ) ) = entry;
                ++size_;
            }

            [[nodiscard]]
            bool
            consume_match( InjectionKind kind,
                           std::uint32_t detail )
            {
                const std::scoped_lock lock( mutex_ );
                const auto             now = std::chrono::steady_clock::now();

                while( size_ > 0U && entries_.at( head_ ).at + entryLifetime <= now )
                {
                    head_ = index_at( 1U );
                    --size_;
                }

                for( std::size_t offset = 0U; offset < size_; ++offset )
                {
                    const auto& entry = entries_.at( index_at( offset ) );
                    if( entry.kind != kind || entry.detail != detail )
                    {
                        continue;
                    }

                    for( std::size_t shifted = offset; shifted + 1U < size_; ++shifted )
                    {
                        entries_.at( index_at( shifted ) ) =
                            entries_.at( index_at( shifted + 1U ) );
                    }
                    --size_;
                    return true;
                }

                return false;
            }

        private:

            struct Entry
            {
                    InjectionKind                         kind{};
                    std::uint32_t                         detail{};
                    std::chrono::steady_clock::time_point at{};
            };

            [[nodiscard]]
            std::size_t
            index_at( std::size_t offset ) const noexcept
            {
                return ( head_ + offset ) % capacity;
            }

            std::array<Entry, capacity> entries_{};
            std::size_t                 head_{};
            std::size_t                 size_{};
            mutable std::mutex          mutex_;
    };

}    // namespace grab::drivers::desktop::x11
