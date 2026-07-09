#include "platform/x11/xcb_window.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view expected_instance  = "plotjuggler4";
    constexpr std::string_view expected_class     = "PlotJuggler4";
    constexpr std::string_view plotjuggler_app    = "plotjuggler";
    constexpr std::string_view missing_app        = "foo";
    constexpr std::string_view other_instance     = "launcher";
    constexpr std::string_view other_class        = "Viewer";
    constexpr std::string_view instance_only_app  = "launcher";
    constexpr std::string_view class_only_app     = "viewer";
    constexpr std::size_t      terminator_bytes   = 1U;
    constexpr std::size_t      complete_raw_bytes = expected_instance.size() +
                                                    terminator_bytes +
                                                    expected_class.size() +
                                                    terminator_bytes;
    constexpr std::size_t      missing_trailing_raw_bytes =
        expected_instance.size() + terminator_bytes + expected_class.size();
    constexpr std::string_view complete_wm_class{
        "plotjuggler4\0PlotJuggler4\0",
        complete_raw_bytes,
    };
    constexpr std::string_view missing_trailing_wm_class{
        "plotjuggler4\0PlotJuggler4",
        missing_trailing_raw_bytes,
    };
    constexpr std::array<std::uint8_t, 0U> empty_wm_class{};

    [[nodiscard]]
    std::vector<std::uint8_t>
    raw_bytes( std::string_view raw )
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve( raw.size() );
        for( const char value : raw )
        {
            bytes.push_back( static_cast<std::uint8_t>( value ) );
        }
        return bytes;
    }

    [[nodiscard]]
    grab::platform::x11::WmClass
    parse_raw( std::string_view raw )
    {
        const std::vector<std::uint8_t> bytes = raw_bytes( raw );
        return grab::platform::x11::parse_wm_class(
            std::span<const std::uint8_t>{ bytes }
        );
    }

}    // namespace

TEST( X11WmClass,
      ParsesInstanceAndClass )
{
    const auto parsed = parse_raw( complete_wm_class );

    EXPECT_EQ( parsed.instance, expected_instance );
    EXPECT_EQ( parsed.window_class, expected_class );
}

TEST( X11WmClass,
      ParsesMissingTrailingTerminator )
{
    const auto parsed = parse_raw( missing_trailing_wm_class );

    EXPECT_EQ( parsed.instance, expected_instance );
    EXPECT_EQ( parsed.window_class, expected_class );
}

TEST( X11WmClass,
      ParsesEmptyBuffer )
{
    const auto parsed = grab::platform::x11::parse_wm_class(
        std::span<const std::uint8_t>{ empty_wm_class }
    );

    EXPECT_TRUE( parsed.instance.empty() );
    EXPECT_TRUE( parsed.window_class.empty() );
}

TEST( X11WmClass,
      MatchesClassCaseInsensitively )
{
    const grab::platform::x11::WmClass wm_class{
        .instance     = std::string{ other_instance },
        .window_class = std::string{ expected_class },
    };

    EXPECT_TRUE( grab::platform::x11::class_matches( wm_class, plotjuggler_app ) );
}

TEST( X11WmClass,
      RejectsMissingApp )
{
    const grab::platform::x11::WmClass wm_class{
        .instance     = std::string{ expected_instance },
        .window_class = std::string{ expected_class },
    };

    EXPECT_FALSE( grab::platform::x11::class_matches( wm_class, missing_app ) );
}

TEST( X11WmClass,
      MatchesInstanceAndClass )
{
    const grab::platform::x11::WmClass instance_match{
        .instance     = std::string{ other_instance },
        .window_class = std::string{ other_class },
    };
    const grab::platform::x11::WmClass class_match{
        .instance     = std::string{ expected_instance },
        .window_class = std::string{ other_class },
    };

    EXPECT_TRUE( grab::platform::x11::class_matches( instance_match,
                                                     instance_only_app ) );
    EXPECT_TRUE( grab::platform::x11::class_matches( class_match, class_only_app ) );
}
