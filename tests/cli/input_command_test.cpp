#include "cli/input_command.hpp"

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

TEST( InputCommand,
      KeyRequiresWindowTarget )
{
    std::array storage{ std::string{ keysymFlag }, std::string{ returnKeysym } };
    auto       args = writable_arguments( storage );

    EXPECT_EQ( grab::cli::run_key_command( args ), usageExitCode );
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
