#include "kernel/support/log.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <string_view>
// clang-format on

namespace
{

    constexpr int              noCalls             = 0;
    constexpr int              oneCall             = 1;
    constexpr std::size_t      impossibleEventSize = 0U;
    constexpr int              smokeCount          = 1;
    constexpr std::string_view smokeTag            = "log.smoke";
    constexpr std::string_view compileTag          = "log.compile";
    constexpr std::string_view levelKey            = "level";
    constexpr std::string_view countKey            = "count";
    constexpr std::string_view nominalText         = "nominal";
    constexpr std::string_view verboseText         = "verbose";
    constexpr std::string_view debugText           = "debug";

}    // namespace

TEST( Log,
      LevelConstantsMatchCompileDefinitions )
{
    static_assert( grab::log::offLevel == GRAB_LOG_LEVEL_OFF );
    static_assert( grab::log::nominalLevel == GRAB_LOG_LEVEL_NOMINAL );
    static_assert( grab::log::verboseLevel == GRAB_LOG_LEVEL_VERBOSE );
    static_assert( grab::log::debugLevel == GRAB_LOG_LEVEL_DEBUG );
    static_assert( grab::log::compileLevel == LOG_COMPILE_LEVEL );

    static_assert( grab::log::enabled( grab::log::Level::Nominal ) ==
                   ( LOG_COMPILE_LEVEL >= GRAB_LOG_LEVEL_NOMINAL ) );
    static_assert( grab::log::enabled( grab::log::Level::Verbose ) ==
                   ( LOG_COMPILE_LEVEL >= GRAB_LOG_LEVEL_VERBOSE ) );
    static_assert( grab::log::enabled( grab::log::Level::Debug ) ==
                   ( LOG_COMPILE_LEVEL >= GRAB_LOG_LEVEL_DEBUG ) );
}

#if LOG_COMPILE_LEVEL < GRAB_LOG_LEVEL_DEBUG
TEST( Log,
      DisabledDebugEmitterIsNotInstantiated )
{
    grab::log::debug(
        []<typename Event>( Event& )
        {
            static_assert( sizeof( Event ) == impossibleEventSize );
        }
    );
    SUCCEED();
}
#endif

TEST( Log,
      DisabledDebugEmitterDoesNotRun )
{
    int calls = noCalls;
    grab::log::debug(
        [&calls]( auto& event )
        {
            ++calls;
            event.tag( compileTag );
        }
    );

    if constexpr( grab::log::enabled( grab::log::Level::Debug ) )
    {
        EXPECT_EQ( calls, oneCall );
    }
    else
    {
        EXPECT_EQ( calls, noCalls );
    }
}

TEST( Log,
      EmitsEnabledLevels )
{
    grab::log::nominal(
        []( auto& event )
        {
            event.tag( smokeTag )
                .value( levelKey, nominalText )
                .value( countKey, smokeCount );
        }
    );
    grab::log::verbose(
        []( auto& event )
        {
            event.tag( smokeTag ).value( levelKey, verboseText );
        }
    );
    grab::log::debug(
        []( auto& event )
        {
            event.tag( smokeTag ).value( levelKey, debugText );
        }
    );
    SUCCEED();
}
