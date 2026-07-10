#pragma once

#include "grab/result.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>
#include <system_error>

namespace grab::cli
{

    struct FractionPair
    {
            double first  = 0.0;
            double second = 0.0;

            [[nodiscard]]
            friend bool
            operator==( FractionPair lhs,
                        FractionPair rhs ) noexcept = default;
    };

    namespace detail
    {

        constexpr char fraction_pair_separator = ',';

        [[nodiscard]]
        inline grab::Result<double>
        parse_fraction_number( std::string_view input )
        {
            if( input.empty() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "fraction is empty" );
            }

            double            value = 0.0;
            const char* const first = input.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const char* const last   = first + input.size();
            const auto        parsed = std::from_chars( first, last, value );
            if( parsed.ec != std::errc{} || parsed.ptr != last )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "fraction contains an invalid number" );
            }
            if( !std::isfinite( value ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "fraction must be finite" );
            }
            return value;
        }

    }    // namespace detail

    [[nodiscard]]
    inline grab::Result<FractionPair>
    parse_fraction_pair( std::string_view input )
    {
        const std::size_t separator = input.find( detail::fraction_pair_separator );
        if( separator == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "fraction pair must match X,Y" );
        }
        if( input.find( detail::fraction_pair_separator, separator + 1U ) !=
            std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "fraction pair must match X,Y" );
        }

        auto first  = detail::parse_fraction_number( input.substr( 0U, separator ) );
        auto second = detail::parse_fraction_number( input.substr( separator + 1U ) );
        if( !first.has_value() )
        {
            return grab::fail( first.error().code, first.error().message );
        }
        if( !second.has_value() )
        {
            return grab::fail( second.error().code, second.error().message );
        }

        return FractionPair{
            .first  = *first,
            .second = *second,
        };
    }

    int
    run_click_command( std::span<char* const> args );

    int
    run_type_command( std::span<char* const> args );

    int
    run_key_command( std::span<char* const> args );

    int
    run_drag_curve_command( std::span<char* const> args );

}    // namespace grab::cli
