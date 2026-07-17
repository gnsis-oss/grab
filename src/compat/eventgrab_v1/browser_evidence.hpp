#pragma once

#include "core/ascii.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace grab::compat::eventgrab_v1
{

    constexpr auto             browserKeywords = std::to_array<std::string_view>( {
        "firefox",
        "navigator",
        "mozilla",
        "chrome",
        "chromium",
        "brave",
        "vivaldi",
        "opera",
        "edge",
        "epiphany",
        "midori",
        "min",
        "zen-browser",
        "waterfox",
        "librewolf",
        "tor browser",
    } );

    constexpr std::string_view minKeyword      = "min";

    namespace detail
    {

        [[nodiscard]]
        constexpr bool
        contains_ascii( std::string_view haystack,
                        std::string_view needle ) noexcept
        {
            return haystack.find( needle ) != std::string_view::npos;
        }

        [[nodiscard]]
        constexpr bool
        has_token_boundaries( std::string_view value,
                              std::size_t      position,
                              std::size_t      length ) noexcept
        {
            const bool at_begin = position == 0U;
            const auto end      = position + length;
            const bool at_end   = end == value.size();

            return ( at_begin || !grab::core::ascii_is_alnum( value[position - 1U] ) ) &&
                   ( at_end || !grab::core::ascii_is_alnum( value[end] ) );
        }

        [[nodiscard]]
        constexpr bool
        contains_ascii_token( std::string_view haystack,
                              std::string_view needle ) noexcept
        {
            std::size_t position = haystack.find( needle );
            while( position != std::string_view::npos )
            {
                if( has_token_boundaries( haystack, position, needle.size() ) )
                {
                    return true;
                }
                position = haystack.find( needle, position + 1U );
            }
            return false;
        }

    }    // namespace detail

    [[nodiscard]]
    inline bool
    is_browser_app( std::string_view app_or_wm_class ) noexcept
    {
        const std::string lowered = grab::core::ascii_lower_copy( app_or_wm_class );
        // "min" (a real browser) is too short to substring-match safely: it
        // would flag "gnome-terminal", "admin", etc. Require a whole-token match
        // for it; every other keyword is a plain substring.
        return std::ranges::any_of(
            browserKeywords,
            [&lowered]( std::string_view keyword )
            {
                return keyword == minKeyword
                         ? detail::contains_ascii_token( lowered, keyword )
                         : detail::contains_ascii( lowered, keyword );
            }
        );
    }

}    // namespace grab::compat::eventgrab_v1
