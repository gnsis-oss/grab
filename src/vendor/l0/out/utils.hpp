#pragma once

#include <out/traits.hpp>
#include <string_view>

namespace out
{

    template<typename E>
    requires( std::is_scoped_enum_v<E> && ErrorTraits<E>::registered )
    [[nodiscard]]
    constexpr std::string_view
    name( E e )
    {
        return ErrorTraits<E>::name( e );
    }

    template<typename E>
    requires( std::is_scoped_enum_v<E> && ErrorTraits<E>::registered )
    [[nodiscard]]
    constexpr std::string_view
    domain()
    {
        return ErrorTraits<E>::domain();
    }

}    // namespace out
