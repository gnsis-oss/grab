#include "core/log.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstddef>
#include <string_view>
// clang-format on

namespace
{

    constexpr int              kNoCalls             = 0;
    constexpr int              kOneCall             = 1;
    constexpr std::size_t      kImpossibleEventSize = 0U;
    constexpr int              kSmokeCount          = 1;
    constexpr std::string_view kSmokeTag            = "log.smoke";
    constexpr std::string_view kCompileTag          = "log.compile";
    constexpr std::string_view kLevelKey            = "level";
    constexpr std::string_view kCountKey            = "count";
    constexpr std::string_view kNominalText         = "nominal";
    constexpr std::string_view kVerboseText         = "verbose";
    constexpr std::string_view kDebugText           = "debug";

}    // namespace

TEST( Log,
      LevelConstantsMatchCompileDefinitions )
{
    static_assert( grab::log::kOffLevel == GRAB_LOG_LEVEL_OFF );
    static_assert( grab::log::kNominalLevel == GRAB_LOG_LEVEL_NOMINAL );
    static_assert( grab::log::kVerboseLevel == GRAB_LOG_LEVEL_VERBOSE );
    static_assert( grab::log::kDebugLevel == GRAB_LOG_LEVEL_DEBUG );
    static_assert( grab::log::kCompileLevel == LOG_COMPILE_LEVEL );

    static_assert( grab::log::enabled( grab::log::Level::nominal ) ==
                   ( LOG_COMPILE_LEVEL >= GRAB_LOG_LEVEL_NOMINAL ) );
    static_assert( grab::log::enabled( grab::log::Level::verbose ) ==
                   ( LOG_COMPILE_LEVEL >= GRAB_LOG_LEVEL_VERBOSE ) );
    static_assert( grab::log::enabled( grab::log::Level::debug ) ==
                   ( LOG_COMPILE_LEVEL >= GRAB_LOG_LEVEL_DEBUG ) );
}

#if LOG_COMPILE_LEVEL < GRAB_LOG_LEVEL_DEBUG
TEST( Log,
      DisabledDebugEmitterIsNotInstantiated )
{
    grab::log::debug(
        []<typename Event>( Event& )
        {
            static_assert( sizeof( Event ) == kImpossibleEventSize );
        }
    );
    SUCCEED();
}
#endif

TEST( Log,
      DisabledDebugEmitterDoesNotRun )
{
    int calls = kNoCalls;
    grab::log::debug(
        [&calls]( auto& event )
        {
            ++calls;
            event.tag( kCompileTag );
        }
    );

    if constexpr( grab::log::enabled( grab::log::Level::debug ) )
    {
        EXPECT_EQ( calls, kOneCall );
    }
    else
    {
        EXPECT_EQ( calls, kNoCalls );
    }
}

TEST( Log,
      EmitsEnabledLevels )
{
    grab::log::nominal(
        []( auto& event )
        {
            event.tag( kSmokeTag )
                .value( kLevelKey, kNominalText )
                .value( kCountKey, kSmokeCount );
        }
    );
    grab::log::verbose(
        []( auto& event )
        {
            event.tag( kSmokeTag ).value( kLevelKey, kVerboseText );
        }
    );
    grab::log::debug(
        []( auto& event )
        {
            event.tag( kSmokeTag ).value( kLevelKey, kDebugText );
        }
    );
    SUCCEED();
}
