#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace grab::core
{

    [[nodiscard]]
    constexpr char
    ascii_lower( char character ) noexcept
    {
        if( character >= 'A' && character <= 'Z' )
        {
            return static_cast<char>( character + ( 'a' - 'A' ) );
        }
        return character;
    }

    [[nodiscard]]
    inline std::string
    ascii_lower_copy( std::string_view value )
    {
        std::string lowered{ value };
        for( char& character : lowered )
        {
            character = ascii_lower( character );
        }
        return lowered;
    }

    [[nodiscard]]
    constexpr bool
    ascii_iequals( std::string_view left,
                   std::string_view right ) noexcept
    {
        if( left.size() != right.size() )
        {
            return false;
        }

        for( std::size_t index = 0U; index < left.size(); ++index )
        {
            if( ascii_lower( left[index] ) != ascii_lower( right[index] ) )
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    constexpr bool
    ascii_icontains( std::string_view haystack,
                     std::string_view needle ) noexcept
    {
        if( needle.empty() )
        {
            return true;
        }
        if( haystack.size() < needle.size() )
        {
            return false;
        }

        const std::size_t last_offset = haystack.size() - needle.size();
        for( std::size_t offset = 0U; offset <= last_offset; ++offset )
        {
            if( ascii_iequals( haystack.substr( offset, needle.size() ), needle ) )
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    constexpr bool
    ascii_istarts_with( std::string_view value,
                        std::string_view prefix ) noexcept
    {
        return value.size() >=
               prefix.size() &&
               ascii_iequals( value.substr( 0U, prefix.size() ), prefix );
    }

    [[nodiscard]]
    constexpr bool
    ascii_is_alnum( char character ) noexcept
    {
        const char lowered = ascii_lower( character );
        return ( lowered >= 'a' && lowered <= 'z' ) ||
               ( lowered >= '0' && lowered <= '9' );
    }

}    // namespace grab::core
