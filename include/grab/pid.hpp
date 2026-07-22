#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace grab
{

    // Canonical process identifier. A single type for PIDs that used to be
    // stored inconsistently as std::string (window/event owner PIDs) or
    // std::int64_t (session supervisor PIDs). A default-constructed Pid is
    // "unknown" (not a valid PID).
    class Pid
    {
        public:

            constexpr Pid() noexcept = default;

            constexpr explicit Pid( std::int64_t value ) noexcept :
                value_( value )
            {
            }

            // Parse a decimal PID. Empty or non-numeric text yields an unknown
            // Pid (matching how absent window PIDs were represented as "").
            [[nodiscard]]
            static Pid
            from_string( std::string_view text ) noexcept
            {
                std::int64_t      value  = 0;
                const char* const begin  = text.begin();
                const char* const end    = text.end();
                const auto        parsed = std::from_chars( begin, end, value );
                if( parsed.ec != std::errc{} || parsed.ptr != end )
                {
                    return Pid{};
                }
                return Pid{ value };
            }

            [[nodiscard]]
            constexpr bool
            valid() const noexcept
            {
                return value_ > 0;
            }

            // Raw numeric value (0 when unknown); use for the session int64 form
            // and for POSIX syscalls.
            [[nodiscard]]
            constexpr std::int64_t
            value() const noexcept
            {
                return value_;
            }

            // Decimal string, or empty when unknown (the prior window/event and
            // wire/JSON string form).
            [[nodiscard]]
            std::string
            to_string() const
            {
                return valid() ? std::to_string( value_ ) : std::string{};
            }

            [[nodiscard]]
            friend constexpr bool
            operator==( Pid,
                        Pid ) noexcept = default;

        private:

            std::int64_t value_ = 0;
    };

}    // namespace grab
