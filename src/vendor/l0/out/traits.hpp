#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace out
{

    template<typename E>
    struct ErrorTraits
    {
            static constexpr bool registered = false;
    };

    // ── Standard errors ─────────────────────────────────────────

    enum class Error : std::uint8_t
    {
        not_found,    // it's not there
        under,        // not enough
        over,         // too much
        barred,       // not allowed
        wrong,        // doesn't match expectation
        time,         // timing problem
        busy,         // external blockage
        stuck,        // internal blockage
        hardware,     // real-world problem
        unhandled,    // doesn't know how
        cant,         // catch-all
    };

    template<>
    struct ErrorTraits<Error>
    {
            static constexpr bool registered = true;

            static constexpr auto values     = std::array{
                Error::not_found,
                Error::under,
                Error::over,
                Error::barred,
                Error::wrong,
                Error::time,
                Error::busy,
                Error::stuck,
                Error::hardware,
                Error::unhandled,
                Error::cant
            };

            static constexpr std::string_view
            name( Error e )
            {
                switch( e )
                {
                    case Error::not_found :
                        return "not_found";
                    case Error::under :
                        return "under";
                    case Error::over :
                        return "over";
                    case Error::barred :
                        return "barred";
                    case Error::wrong :
                        return "wrong";
                    case Error::time :
                        return "time";
                    case Error::busy :
                        return "busy";
                    case Error::stuck :
                        return "stuck";
                    case Error::hardware :
                        return "hardware";
                    case Error::unhandled :
                        return "unhandled";
                    case Error::cant :
                        return "cant";
                }
                return "Unknown";
            }

            static constexpr std::string_view
            domain()
            {
                return "out::Error";
            }
    };

}    // namespace out
