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
      LevelConstantsAreOrdered )
{
    static_assert( grab::log::offLevel < grab::log::nominalLevel );
    static_assert( grab::log::nominalLevel < grab::log::verboseLevel );
    static_assert( grab::log::verboseLevel < grab::log::debugLevel );

    static_assert( static_cast<int>( grab::log::Level::Off ) == grab::log::offLevel );
    static_assert( static_cast<int>( grab::log::Level::Nominal ) ==
                   grab::log::nominalLevel );
    static_assert( static_cast<int>( grab::log::Level::Verbose ) ==
                   grab::log::verboseLevel );
    static_assert( static_cast<int>( grab::log::Level::Debug ) ==
                   grab::log::debugLevel );
}

// `compileLevel` comes from the generated header the build directory
// configured (cmake/Logging.cmake), not from a preprocessor define. Whatever
// it is, `enabled()` must agree with it — and must be usable in a constant
// expression, which is what makes the emitter lambdas disappear.
TEST( Log,
      EnabledAgreesWithGeneratedCompileLevel )
{
    static_assert( grab::log::enabled( grab::log::Level::Off ) );
    static_assert( grab::log::enabled( grab::log::Level::Nominal ) ==
                   ( grab::log::compileLevel >= grab::log::nominalLevel ) );
    static_assert( grab::log::enabled( grab::log::Level::Verbose ) ==
                   ( grab::log::compileLevel >= grab::log::verboseLevel ) );
    static_assert( grab::log::enabled( grab::log::Level::Debug ) ==
                   ( grab::log::compileLevel >= grab::log::debugLevel ) );
}

// A disabled emitter's lambda must never be instantiated, not merely never
// called: the body here would fail to compile if it were. `if constexpr` on a
// `consteval` predicate is what guarantees that, with no preprocessor
// conditional around the test.
TEST( Log,
      DisabledDebugEmitterIsNotInstantiated )
{
    if constexpr( !grab::log::enabled( grab::log::Level::Debug ) )
    {
        grab::log::debug(
            []<typename Event>( Event& )
            {
                static_assert( sizeof( Event ) == impossibleEventSize );
            }
        );
    }
    SUCCEED();
}

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
