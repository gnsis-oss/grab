#pragma once

#include "grab/result.hpp"

#include <concepts>
#include <limits>
#include <string>
#include <utility>

namespace grab
{

    template<std::integral To,
             std::integral From>
    [[nodiscard]]
    Result<To>
    checked_cast( From        value,
                  ErrorCode   code,
                  std::string message )
    {
        if( !std::in_range<To>( value ) )
        {
            return fail( code, std::move( message ) );
        }
        return static_cast<To>( value );
    }

    template<std::unsigned_integral T>
    [[nodiscard]]
    Result<T>
    checked_mul( T           lhs,
                 T           rhs,
                 ErrorCode   code,
                 std::string message )
    {
        if( lhs != 0U && rhs > std::numeric_limits<T>::max() / lhs )
        {
            return fail( code, std::move( message ) );
        }
        return lhs * rhs;
    }

    template<std::unsigned_integral T>
    [[nodiscard]]
    Result<T>
    checked_add( T           lhs,
                 T           rhs,
                 ErrorCode   code,
                 std::string message )
    {
        if( lhs > std::numeric_limits<T>::max() - rhs )
        {
            return fail( code, std::move( message ) );
        }
        return lhs + rhs;
    }

}    // namespace grab
