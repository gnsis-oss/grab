#pragma once

#include <algorithm>

#include "grab/event.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace grab::event
{

    constexpr std::array<std::string_view, 16> browserKeywords{
        {
         "firefox", "navigator",
         "mozilla", "chrome",
         "chromium", "brave",
         "vivaldi", "opera",
         "edge", "epiphany",
         "midori", "min",
         "zen-browser", "waterfox",
         "librewolf", "tor browser",
         }
    };
    static_assert( browserKeywords.size() == 16U );

    constexpr std::string_view minKeyword = "min";

    namespace detail
    {

        [[nodiscard]]
        constexpr char
        lower_ascii( char character ) noexcept
        {
            if( character >= 'A' && character <= 'Z' )
            {
                return static_cast<char>( character + ( 'a' - 'A' ) );
            }
            return character;
        }

        [[nodiscard]]
        inline std::string
        lower_ascii( std::string_view value )
        {
            std::string lowered{ value };
            for( char& character : lowered )
            {
                character = lower_ascii( character );
            }
            return lowered;
        }

        [[nodiscard]]
        constexpr bool
        contains_ascii( std::string_view haystack,
                        std::string_view needle ) noexcept
        {
            return haystack.find( needle ) != std::string_view::npos;
        }

        [[nodiscard]]
        constexpr bool
        is_ascii_alnum( char character ) noexcept
        {
            return ( character >= 'a' && character <= 'z' ) ||
                   ( character >= '0' && character <= '9' );
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

            return ( at_begin || !is_ascii_alnum( value[position - 1U] ) ) &&
                   ( at_end || !is_ascii_alnum( value[end] ) );
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
        const std::string lowered = detail::lower_ascii( app_or_wm_class );
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
            } );
    }

    [[nodiscard]]
    inline std::optional<grab::BrowserTab>
    browser_tab_on_title_change( std::string_view app,
                                 grab::Pid        pid,
                                 bool             is_browser,
                                 std::string_view new_title,
                                 std::string_view prev_title )
    {
        if( !is_browser || new_title.empty() || new_title == prev_title )
        {
            return std::nullopt;
        }

        return grab::BrowserTab{
            .app            = std::string{ app },
            .pid            = pid,
            .tab_title      = std::string{ new_title },
            .prev_tab_title = std::string{ prev_title },
        };
    }

    [[nodiscard]]
    inline std::optional<grab::BrowserTab>
    browser_tab_on_focus_change( std::string_view current_app,
                                 grab::Pid        current_pid,
                                 bool             current_is_browser,
                                 std::string_view current_title,
                                 bool             has_previous,
                                 bool             previous_is_browser,
                                 std::string_view previous_app )
    {
        if( !current_is_browser ||
            ( has_previous && previous_is_browser && previous_app == current_app ) )
        {
            return std::nullopt;
        }

        return grab::BrowserTab{
            .app            = std::string{ current_app },
            .pid            = current_pid,
            .tab_title      = std::string{ current_title },
            .prev_tab_title = {},
        };
    }

}    // namespace grab::event
