#pragma once

#include "grab/result.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace grab::cli
{

    struct Geometry
    {
            std::uint16_t width  = 0U;
            std::uint16_t height = 0U;
            std::int16_t  x      = 0;
            std::int16_t  y      = 0;
    };

    namespace detail
    {

        constexpr char          dimension_marker = 'x';
        constexpr char          offset_marker    = '+';
        constexpr char          minus_sign       = '-';
        constexpr char          ascii_zero       = '0';
        constexpr char          ascii_nine       = '9';
        constexpr std::uint32_t decimal_base     = 10U;
        constexpr std::uint32_t no_digits        = 0U;
        constexpr std::uint32_t no_value         = 0U;

        [[nodiscard]]
        constexpr bool
        is_digit( char value ) noexcept
        {
            return value >= ascii_zero && value <= ascii_nine;
        }

        [[nodiscard]]
        constexpr std::uint32_t
        digit_value( char value ) noexcept
        {
            return static_cast<std::uint32_t>( value - ascii_zero );
        }

        [[nodiscard]]
        inline grab::Result<std::uint32_t>
        parse_unsigned( std::string_view input,
                        std::uint32_t    max_value )
        {
            if( input.empty() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "geometry contains an empty number" );
            }

            std::uint32_t value         = no_value;
            std::uint32_t digit_count   = no_digits;
            const auto    max_quotient  = max_value / decimal_base;
            const auto    max_remainder = max_value % decimal_base;

            for( const char current : input )
            {
                if( !is_digit( current ) )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "geometry contains a non-digit" );
                }
                const std::uint32_t digit = digit_value( current );
                if( value >
                    max_quotient ||
                    ( value == max_quotient && digit > max_remainder ) )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "geometry number is out of range" );
                }
                value = ( value * decimal_base ) + digit;
                ++digit_count;
            }

            if( digit_count == no_digits )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "geometry contains an empty number" );
            }
            return value;
        }

        [[nodiscard]]
        inline grab::Result<std::int16_t>
        parse_signed_i16( std::string_view input )
        {
            if( input.empty() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "geometry contains an empty offset" );
            }

            const bool negative = input.front() == minus_sign;
            if( negative )
            {
                input.remove_prefix( 1U );
            }

            constexpr auto int16_positive_max =
                static_cast<std::uint32_t>( std::numeric_limits<std::int16_t>::max() );
            constexpr std::uint32_t int16_negative_magnitude = int16_positive_max + 1U;
            const std::uint32_t     limit =
                negative ? int16_negative_magnitude : int16_positive_max;

            auto magnitude = parse_unsigned( input, limit );
            if( !magnitude.has_value() )
            {
                return grab::fail( magnitude.error().code, magnitude.error().message );
            }
            if( negative )
            {
                const auto signed_magnitude = static_cast<std::int32_t>( *magnitude );
                return static_cast<std::int16_t>( -signed_magnitude );
            }
            return static_cast<std::int16_t>( *magnitude );
        }

        [[nodiscard]]
        inline grab::Result<std::uint16_t>
        parse_nonzero_u16( std::string_view input )
        {
            constexpr auto u_int16_max =
                static_cast<std::uint32_t>( std::numeric_limits<std::uint16_t>::max() );
            auto value = parse_unsigned( input, u_int16_max );
            if( !value.has_value() )
            {
                return grab::fail( value.error().code, value.error().message );
            }
            if( *value == 0U )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "geometry dimensions must be non-zero" );
            }
            return static_cast<std::uint16_t>( *value );
        }

    }    // namespace detail

    [[nodiscard]]
    inline grab::Result<Geometry>
    parse_geometry( std::string_view input )
    {
        const std::size_t dimension = input.find( detail::dimension_marker );
        if( dimension == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "geometry must match WxH+X+Y" );
        }

        const std::size_t first_offset =
            input.find( detail::offset_marker, dimension + 1U );
        if( first_offset == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "geometry must match WxH+X+Y" );
        }

        const std::size_t second_offset =
            input.find( detail::offset_marker, first_offset + 1U );
        if( second_offset == std::string_view::npos )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "geometry must match WxH+X+Y" );
        }

        auto width = detail::parse_nonzero_u16( input.substr( 0U, dimension ) );
        auto height =
            detail::parse_nonzero_u16( input.substr( dimension + 1U,
                                                     first_offset - dimension - 1U ) );
        auto x = detail::parse_signed_i16(
            input.substr( first_offset + 1U, second_offset - first_offset - 1U )
        );
        auto y = detail::parse_signed_i16( input.substr( second_offset + 1U ) );
        if( !width.has_value() )
        {
            return grab::fail( width.error().code, width.error().message );
        }
        if( !height.has_value() )
        {
            return grab::fail( height.error().code, height.error().message );
        }
        if( !x.has_value() )
        {
            return grab::fail( x.error().code, x.error().message );
        }
        if( !y.has_value() )
        {
            return grab::fail( y.error().code, y.error().message );
        }

        return Geometry{
            .width  = *width,
            .height = *height,
            .x      = *x,
            .y      = *y,
        };
    }

    int
    run_capture_command( std::span<char* const> args );

}    // namespace grab::cli
