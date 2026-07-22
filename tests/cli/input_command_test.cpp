#include "frontends/cli/input_command.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
// clang-format on

namespace
{

    constexpr int         usageExitCode  = 2;
    constexpr const char* keysymFlag     = "--keysym";
    constexpr const char* returnKeysym   = "Return";
    constexpr const char* unknownKeysym  = "NoSuchGrabKeysym";
    constexpr const char* windowFlag     = "--window";
    constexpr const char* windowIdFlag   = "--window-id";
    constexpr const char* windowIdValue  = "4194311";
    constexpr const char* notAWindowId   = "0x400007";
    constexpr const char* windowClass    = "GrabInputCommandTest";
    constexpr const char* sourceFlag     = "--src";
    constexpr const char* sourcePoint    = "0.2,0.3";
    constexpr const char* malformedPoint = "0.2";

    template<std::size_t Count>
    [[nodiscard]]
    std::array<char*,
               Count>
    writable_arguments( std::array<std::string,
                                   Count>& storage )
    {
        std::array<char*, Count> arguments{};
        std::ranges::transform( storage,
                                arguments.begin(),
                                []( std::string& argument )
                                {
                                    return argument.data();
                                } );
        return arguments;
    }

}    // namespace

// XTest delivers to whatever holds focus, so an absent window selector is a
// deliberate "leave focus alone" instruction rather than a usage error. The
// press itself may still fail on a display without XTest, hence NE not EQ.
TEST( InputCommand,
      KeyWithoutAWindowSelectorIsNotAUsageError )
{
    std::array storage{ std::string{ keysymFlag }, std::string{ returnKeysym } };
    auto       args = writable_arguments( storage );

    EXPECT_NE( grab::cli::run_key_command( args ), usageExitCode );
}

TEST( InputCommand,
      KeyRejectsBothWindowSelectorsAtOnce )
{
    std::array storage{
        std::string{ windowFlag },
        std::string{ windowClass },
        std::string{ windowIdFlag },
        std::string{ windowIdValue },
        std::string{ keysymFlag },
        std::string{ returnKeysym },
    };
    auto args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_key_command( args ), usageExitCode );
}

TEST( InputCommand,
      KeyRejectsNonDecimalWindowId )
{
    std::array storage{
        std::string{ windowIdFlag },
        std::string{ notAWindowId },
        std::string{ keysymFlag },
        std::string{ returnKeysym },
    };
    auto args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_key_command( args ), usageExitCode );
}

// drag-curve needs a resolved accessibility node to anchor its fractions on,
// which only a WM_CLASS locator produces.
TEST( InputCommand,
      DragCurveRejectsAWindowIdSelector )
{
    std::array storage{
        std::string{ windowIdFlag },
        std::string{ windowIdValue },
        std::string{ sourceFlag },
        std::string{ sourcePoint },
    };
    auto args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_drag_curve_command( args ), usageExitCode );
}

TEST( InputCommand,
      KeyRejectsUnknownKeysymAsUsageError )
{
    std::array storage{
        std::string{ windowFlag },
        std::string{ windowClass },
        std::string{ keysymFlag },
        std::string{ unknownKeysym },
    };
    auto args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_key_command( args ), usageExitCode );
}

TEST( InputCommand,
      DragCurveRequiresDestination )
{
    std::array storage{
        std::string{ windowFlag },
        std::string{ windowClass },
        std::string{ sourceFlag },
        std::string{ sourcePoint },
    };
    auto args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_drag_curve_command( args ), usageExitCode );
}

TEST( InputCommand,
      DragCurveRejectsMalformedFractionPoint )
{
    std::array storage{
        std::string{ windowFlag },
        std::string{ windowClass },
        std::string{ sourceFlag },
        std::string{ malformedPoint },
    };
    auto args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_drag_curve_command( args ), usageExitCode );
}

TEST( WindowFractionToCoordinate,
      ZeroFractionMapsToOrigin )
{
    const auto coordinate =
        grab::cli::window_fraction_to_coordinate( 160.0, 320.0, 0.0, "x" );

    ASSERT_TRUE( coordinate.has_value() ) << coordinate.error().message;
    EXPECT_EQ( *coordinate, 160 );
}

TEST( WindowFractionToCoordinate,
      MaximumFractionMapsToLastPixel )
{
    const auto coordinate =
        grab::cli::window_fraction_to_coordinate( 160.0, 320.0, 1.0, "x" );

    ASSERT_TRUE( coordinate.has_value() ) << coordinate.error().message;
    EXPECT_EQ( *coordinate, 160 + 320 - 1 );
}

TEST( WindowFractionToCoordinate,
      FractionOutsideUnitIntervalIsInvalid )
{
    const auto coordinate =
        grab::cli::window_fraction_to_coordinate( 160.0, 320.0, -0.01, "x" );

    ASSERT_FALSE( coordinate.has_value() );
    EXPECT_EQ( coordinate.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( WindowFractionToCoordinate,
      SubPixelExtentIsInvalid )
{
    const auto coordinate =
        grab::cli::window_fraction_to_coordinate( 160.0, 0.0, 0.5, "x" );

    ASSERT_FALSE( coordinate.has_value() );
    EXPECT_EQ( coordinate.error().code, grab::ErrorCode::InvalidArgument );
}
