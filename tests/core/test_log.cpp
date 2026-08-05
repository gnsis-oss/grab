#include "kernel/support/log.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace
{

    constexpr int              noCalls             = 0;
    constexpr int              oneCall             = 1;
    constexpr std::size_t      impossibleEventSize = 0U;
    constexpr int              smokeCount          = 1;
    constexpr std::string_view smokeTag            = "log.smoke";
    constexpr std::string_view compileTag          = "log.compile";
    constexpr std::string_view otherTag            = "log.other";
    constexpr std::string_view levelKey            = "level";
    constexpr std::string_view countKey            = "count";
    constexpr std::string_view nominalText         = "nominal";
    constexpr std::string_view verboseText         = "verbose";
    constexpr std::string_view debugText           = "debug";
    constexpr std::string_view controlValue        = "a\nb";
    constexpr std::string_view controlKey          = "multiline";
    constexpr std::size_t      oneRecord           = 1U;
    constexpr std::size_t      noRecords           = 0U;

    // Routes the log to a temporary file, runs `body`, and returns what was
    // written. The runtime level and sink are process-global, so each case
    // restores them.
    template<typename Body>
    [[nodiscard]]
    std::string
    captured( grab::log::Level level,
              Body             body )
    {
        const std::string path =
            std::string{ std::tmpnam( nullptr ) } + ".grab-log-test";

        const auto previous = grab::log::runtime_level();
        EXPECT_TRUE( grab::log::sink_to_file( path ) );
        grab::log::set_runtime_level( level );

        body();

        grab::log::set_runtime_level( previous );
        grab::log::sink_off();

        std::ifstream     stream{ path };
        std::stringstream buffer;
        buffer << stream.rdbuf();
        stream.close();
        ( void )std::remove( path.c_str() );
        return buffer.str();
    }

    [[nodiscard]]
    std::size_t
    line_count( std::string_view text )
    {
        std::size_t lines = 0;
        for( const char character : text )
        {
            if( character == '\n' )
            {
                ++lines;
            }
        }
        return lines;
    }

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

// An emitter runs only when BOTH gates admit it: the compile ceiling and the
// runtime level. Passing the compile ceiling alone is not enough, which is what
// makes logging free in a shipped binary nobody has asked to be verbose.
TEST( Log,
      EmitterRunsOnlyWhenBothGatesAdmitIt )
{
    int        calls  = noCalls;
    const auto output = captured( grab::log::Level::Debug,
                                  [&calls]
                                  {
                                      grab::log::debug(
                                          [&calls]( auto& event )
                                          {
                                              ++calls;
                                              event.tag( compileTag );
                                          }
                                      );
                                  } );

    if constexpr( grab::log::enabled( grab::log::Level::Debug ) )
    {
        EXPECT_EQ( calls, oneCall );
        EXPECT_EQ( line_count( output ), oneRecord );
    }
    else
    {
        EXPECT_EQ( calls, noCalls );
        EXPECT_TRUE( output.empty() );
    }
}

// The compile ceiling admits it, but the runtime level does not.
TEST( Log,
      CompileEnabledEmitterStillDoesNotRunWhenTheRuntimeLevelIsBelowIt )
{
    int        calls  = noCalls;

    const auto output = captured( grab::log::Level::Nominal,
                                  [&calls]
                                  {
                                      grab::log::debug(
                                          [&calls]( auto& event )
                                          {
                                              ++calls;
                                              event.tag( compileTag );
                                          }
                                      );
                                  } );

    EXPECT_EQ( calls, noCalls );
    EXPECT_TRUE( output.empty() );
}

// The runtime gate is independent of the compile ceiling. This is the property
// that lets a release-shaped build ship logging that costs nothing until asked.
TEST( Log,
      RuntimeLevelOffSuppressesEverythingAndDoesNotRunTheEmitter )
{
    int        calls  = noCalls;
    const auto output = captured( grab::log::Level::Off,
                                  [&calls]
                                  {
                                      grab::log::nominal(
                                          [&calls]( auto& event )
                                          {
                                              ++calls;
                                              event.tag( smokeTag );
                                          }
                                      );
                                  } );

    EXPECT_EQ( calls, noCalls );
    EXPECT_TRUE( output.empty() );
}

TEST( Log,
      RuntimeLevelAdmitsItsOwnLevelAndBelow )
{
    if constexpr( !grab::log::enabled( grab::log::Level::Verbose ) )
    {
        GTEST_SKIP() << "build's compile ceiling excludes verbose";
    }

    const auto output = captured( grab::log::Level::Nominal,
                                  []
                                  {
                                      grab::log::nominal(
                                          []( auto& event )
                                          {
                                              event.tag( smokeTag );
                                          }
                                      );
                                      grab::log::verbose(
                                          []( auto& event )
                                          {
                                              event.tag( smokeTag );
                                          }
                                      );
                                  } );

    EXPECT_EQ( line_count( output ), oneRecord );
    EXPECT_NE( output.find( nominalText ), std::string::npos );
}

TEST( Log,
      RecordCarriesLevelTagSourceLocationAndFields )
{
    const auto output = captured( grab::log::Level::Nominal,
                                  []
                                  {
                                      grab::log::nominal(
                                          []( auto& event )
                                          {
                                              event.tag( smokeTag )
                                                  .value( levelKey, nominalText )
                                                  .value( countKey, smokeCount );
                                          }
                                      );
                                  } );

    EXPECT_NE( output.find( "nominal" ), std::string::npos );
    EXPECT_NE( output.find( "tag=log.smoke" ), std::string::npos );
    EXPECT_NE( output.find( "level=nominal" ), std::string::npos );
    EXPECT_NE( output.find( "count=1" ), std::string::npos );
    // Source location, without a macro at the call site.
    EXPECT_NE( output.find( "test_log.cpp:" ), std::string::npos );
    // Elapsed prefix, and never negative — the epoch is captured before now().
    EXPECT_EQ( output.front(), '+' );
    EXPECT_NE( output.at( 1 ), '-' );
}

// One record is one line. A control character in a value would otherwise split
// a record in two for anything reading the log.
TEST( Log,
      ControlCharactersInValuesDoNotSplitARecord )
{
    const auto output =
        captured( grab::log::Level::Nominal,
                  []
                  {
                      grab::log::nominal(
                          []( auto& event )
                          {
                              event.tag( smokeTag ).value( controlKey, controlValue );
                          }
                      );
                  } );

    EXPECT_EQ( line_count( output ), oneRecord );
}

TEST( Log,
      TagFilterAdmitsOnlyListedTags )
{
    const std::array<std::string_view, 1> allow{ smokeTag };
    grab::log::set_tag_filter( allow );

    const auto output = captured( grab::log::Level::Nominal,
                                  []
                                  {
                                      grab::log::nominal(
                                          []( auto& event )
                                          {
                                              event.tag( smokeTag );
                                          }
                                      );
                                      grab::log::nominal(
                                          []( auto& event )
                                          {
                                              event.tag( otherTag );
                                          }
                                      );
                                  } );

    grab::log::set_tag_filter( {} );

    EXPECT_EQ( line_count( output ), oneRecord );
    EXPECT_NE( output.find( "tag=log.smoke" ), std::string::npos );
    EXPECT_EQ( output.find( "tag=log.other" ), std::string::npos );
}

TEST( Log,
      SinkOffDiscardsRecordsWithoutFailing )
{
    grab::log::sink_off();
    const auto previous = grab::log::runtime_level();
    grab::log::set_runtime_level( grab::log::Level::Nominal );

    grab::log::nominal(
        []( auto& event )
        {
            event.tag( smokeTag );
        }
    );

    grab::log::set_runtime_level( previous );
    SUCCEED();
}

TEST( Log,
      SinkToFileFailsOnAnUnopenablePath )
{
    EXPECT_FALSE( grab::log::sink_to_file( "/proc/self/no/such/directory/x.log" ) );
}

TEST( Log,
      EmitsEnabledLevels )
{
    const auto output =
        captured( grab::log::Level::Debug,
                  []
                  {
                      grab::log::nominal(
                          []( auto& event )
                          {
                              event.tag( smokeTag ).value( levelKey, nominalText );
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
                  } );

    std::size_t expected = noRecords;
    if constexpr( grab::log::enabled( grab::log::Level::Nominal ) )
    {
        ++expected;
    }
    if constexpr( grab::log::enabled( grab::log::Level::Verbose ) )
    {
        ++expected;
    }
    if constexpr( grab::log::enabled( grab::log::Level::Debug ) )
    {
        ++expected;
    }
    EXPECT_EQ( line_count( output ), expected );
}
