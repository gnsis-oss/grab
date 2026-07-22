#include "frontends/cli/windows_command.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"
#include "grab/window_info.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::uint32_t firstWindowId     = 4'194'311U;
    constexpr std::uint32_t secondWindowId    = 4'194'312U;
    constexpr std::uint32_t firstWindowPid    = 2'481U;
    constexpr std::int32_t  firstWindowX      = 120;
    constexpr std::int32_t  firstWindowY      = -40;
    constexpr std::uint32_t firstWindowWidth  = 1'280U;
    constexpr std::uint32_t firstWindowHeight = 720U;
    constexpr std::int32_t  secondWindowX     = 0;
    constexpr std::int32_t  secondWindowY     = 0;
    constexpr std::uint32_t secondWindowWidth = 640U;
    constexpr std::uint32_t secondWindowHigh  = 480U;
    constexpr std::size_t   noWindows         = 0U;
    constexpr std::size_t   oneWindow         = 1U;
    constexpr std::size_t   twoWindows        = 2U;
    constexpr const char*   plotJugglerClass  = "PlotJuggler4";
    constexpr const char*   terminalClass     = "Xterm";
    constexpr const char*   lowercaseFragment = "plotjuggler";
    constexpr const char*   missingClass      = "no-such-class";
    constexpr const char*   normalType        = "normal";
    constexpr const char*   splashType        = "splash";
    constexpr const char*   missingType       = "dock";
    constexpr std::uint32_t absentWindowId    = 7U;
    constexpr std::int32_t  placeX            = 200;
    constexpr std::int32_t  placeY            = -30;
    constexpr std::uint32_t placeWidth        = 1'600U;
    constexpr std::uint32_t placeHeight       = 900U;
    constexpr auto          placeTimeout      = std::chrono::milliseconds{ 250 };

    [[nodiscard]]
    std::vector<grab::WindowSummary>
    sample_windows()
    {
        return {
            grab::WindowSummary{
                                .id       = firstWindowId,
                                .wm_class = plotJugglerClass,
                                .title    = "PlotJuggler 4",
                                .type     = normalType,
                                .pid      = firstWindowPid,
                                .bounds =
                                grab::geometry::Rectangle{
                                .x      = firstWindowX,
                                .y      = firstWindowY,
                                .width  = firstWindowWidth,
                                .height = firstWindowHeight
                                }, },
            grab::WindowSummary{
                                .id       = secondWindowId,
                                .wm_class = terminalClass,
                                .title    = "",
                                .type     = splashType,
                                .pid      = std::nullopt,
                                .bounds   = grab::geometry::Rectangle{
                                .x      = secondWindowX,
                                .y      = secondWindowY,
                                .width  = secondWindowWidth,
                                .height = secondWindowHigh
                                }, },
        };
    }

    // The CLI parsers take argv-shaped `char* const` spans, so tests need
    // mutable backing storage for their argument strings.
    class ArgumentVector
    {
        public:

            explicit ArgumentVector( std::vector<std::string> values ) :
                storage_( std::move( values ) )
            {
                pointers_.reserve( storage_.size() );
                for( std::string& value : storage_ )
                {
                    pointers_.push_back( value.data() );
                }
            }

            [[nodiscard]]
            std::span<char* const>
            span() const noexcept
            {
                return { pointers_.data(), pointers_.size() };
            }

        private:

            std::vector<std::string> storage_;
            std::vector<char*>       pointers_;
    };

    TEST( WindowsCommand,
          ParsesFlagsInAnyOrder )
    {
        const ArgumentVector args{
            { "--class", plotJugglerClass, "--json", "--display", ":1" }
        };

        const auto options = grab::cli::parse_windows_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        EXPECT_TRUE( options->as_json );
        EXPECT_EQ( options->wm_class, plotJugglerClass );
        EXPECT_EQ( options->display, ":1" );
    }

    TEST( WindowsCommand,
          DefaultsToHumanReadableAndUnfiltered )
    {
        const ArgumentVector args{ {} };

        const auto           options = grab::cli::parse_windows_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        EXPECT_FALSE( options->as_json );
        EXPECT_TRUE( options->wm_class.empty() );
        EXPECT_TRUE( options->display.empty() );
    }

    TEST( WindowsCommand,
          RejectsUnknownAndValuelessFlags )
    {
        const ArgumentVector unknown{ { "--nope" } };
        const ArgumentVector dangling{ { "--class" } };

        EXPECT_FALSE( grab::cli::parse_windows_options( unknown.span() ).has_value() );
        EXPECT_FALSE( grab::cli::parse_windows_options( dangling.span() ).has_value() );
    }

    TEST( WindowsCommand,
          FiltersByCaseInsensitiveClassFragment )
    {
        const auto filtered =
            grab::cli::filter_windows_by_class( sample_windows(), lowercaseFragment );

        ASSERT_EQ( filtered.size(), oneWindow );
        EXPECT_EQ( filtered.front().id, firstWindowId );
    }

    TEST( WindowsCommand,
          EmptyFilterKeepsEveryWindowAndAMissAllOfThem )
    {
        EXPECT_EQ( grab::cli::filter_windows_by_class( sample_windows(), "" ).size(),
                   twoWindows );
        EXPECT_EQ(
            grab::cli::filter_windows_by_class( sample_windows(), missingClass ).size(),
            noWindows
        );
    }

    TEST( WindowsCommand,
          ParsesATypeFilter )
    {
        const ArgumentVector args{
            { "--type", normalType }
        };

        const auto options = grab::cli::parse_windows_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        EXPECT_EQ( options->type, normalType );
    }

    TEST( WindowsCommand,
          RejectsAValuelessTypeFlag )
    {
        const ArgumentVector dangling{ { "--type" } };

        EXPECT_FALSE( grab::cli::parse_windows_options( dangling.span() ).has_value() );
    }

    // The splash-screen case this exists for: same class, same title, and it
    // also advertises NORMAL as a fallback, so only the type separates them.
    TEST( WindowsCommand,
          FiltersByExactTypeCaseInsensitively )
    {
        const auto normal =
            grab::cli::filter_windows_by_type( sample_windows(), normalType );
        ASSERT_EQ( normal.size(), oneWindow );
        EXPECT_EQ( normal.front().id, firstWindowId );

        const auto splash =
            grab::cli::filter_windows_by_type( sample_windows(), "SPLASH" );
        ASSERT_EQ( splash.size(), oneWindow );
        EXPECT_EQ( splash.front().id, secondWindowId );
    }

    TEST( WindowsCommand,
          EmptyTypeFilterKeepsEveryWindowAndAMissAllOfThem )
    {
        EXPECT_EQ( grab::cli::filter_windows_by_type( sample_windows(), "" ).size(),
                   twoWindows );
        EXPECT_EQ(
            grab::cli::filter_windows_by_type( sample_windows(), missingType ).size(),
            noWindows
        );
    }

    // A closed vocabulary needs exact matching: "menu" must not also select
    // "popup_menu" the way the WM_CLASS substring rule would.
    TEST( WindowsCommand,
          TypeFilterDoesNotMatchOnSubstrings )
    {
        std::vector<grab::WindowSummary> windows = sample_windows();
        windows.front().type                     = "popup_menu";

        EXPECT_EQ( grab::cli::filter_windows_by_type( windows, "menu" ).size(),
                   noWindows );
        EXPECT_EQ( grab::cli::filter_windows_by_type( windows, "popup_menu" ).size(),
                   oneWindow );
    }

    TEST( WindowsCommand,
          JsonCarriesEveryFieldAndNullsAMissingPid )
    {
        const std::string json = grab::cli::format_windows_json( sample_windows() );

        EXPECT_NE( json.find( "\"wm_class\": \"PlotJuggler4\"" ), std::string::npos )
            << json;
        EXPECT_NE( json.find( "\"type\": \"normal\"" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"type\": \"splash\"" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"pid\": 2481" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"pid\": null" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"x\": 120" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"y\": -40" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"width\": 1280" ), std::string::npos ) << json;
        EXPECT_NE( json.find( "\"height\": 720" ), std::string::npos ) << json;
        EXPECT_TRUE( json.starts_with( "[" ) ) << json;
        EXPECT_TRUE( json.ends_with( "]" ) ) << json;
    }

    TEST( WindowsCommand,
          EmptyListRendersAsAnEmptyJsonArrayAndNoText )
    {
        EXPECT_EQ( grab::cli::format_windows_json( {} ), "[]" );
        EXPECT_EQ( grab::cli::format_windows_text( {} ), "" );
    }

    TEST( WindowsCommand,
          TextIsOneLinePerWindow )
    {
        const std::string text = grab::cli::format_windows_text( sample_windows() );

        EXPECT_EQ( text,
                   "4194311 PlotJuggler4 normal pid=2481 120,-40 1280x720 "
                   "\"PlotJuggler 4\"\n"
                   "4194312 Xterm splash pid=- 0,0 640x480 \"\"\n" );
    }

    TEST( FocusCommand,
          RequiresAWindowClass )
    {
        const ArgumentVector empty{ {} };
        const ArgumentVector dangling{ { "--window" } };
        const ArgumentVector unknown{
            { "--class", terminalClass }
        };

        EXPECT_FALSE( grab::cli::parse_focus_options( empty.span() ).has_value() );
        EXPECT_FALSE( grab::cli::parse_focus_options( dangling.span() ).has_value() );
        EXPECT_FALSE( grab::cli::parse_focus_options( unknown.span() ).has_value() );
    }

    TEST( FocusCommand,
          ParsesWindowAndDisplay )
    {
        const ArgumentVector args{
            { "--window", plotJugglerClass, "--display", ":1" }
        };

        const auto options = grab::cli::parse_focus_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        EXPECT_EQ( options->selector.wm_class, plotJugglerClass );
        EXPECT_FALSE( options->selector.window_id.has_value() );
        EXPECT_EQ( options->display, ":1" );
    }

    TEST( FocusCommand,
          ParsesAWindowIdSelector )
    {
        const ArgumentVector args{
            { "--window-id", "4194311" }
        };

        const auto options = grab::cli::parse_focus_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        ASSERT_TRUE( options->selector.window_id.has_value() );
        EXPECT_EQ( *options->selector.window_id, firstWindowId );
        EXPECT_TRUE( options->selector.wm_class.empty() );
    }

    TEST( FocusCommand,
          RejectsBothSelectorsAndNonDecimalIds )
    {
        const ArgumentVector both{
            { "--window", plotJugglerClass, "--window-id", "4194311" }
        };
        const ArgumentVector hexadecimal{
            { "--window-id", "0x400007" }
        };

        EXPECT_FALSE( grab::cli::parse_focus_options( both.span() ).has_value() );
        EXPECT_FALSE( grab::cli::parse_focus_options( hexadecimal.span() ).has_value() );
    }

    TEST( PlacementGeometry,
          ParsesPositiveAndNegativeOffsets )
    {
        const auto positive = grab::cli::parse_placement_geometry( "1600x900+200+30" );
        ASSERT_TRUE( positive.has_value() ) << positive.error().message;
        EXPECT_EQ( positive->width, placeWidth );
        EXPECT_EQ( positive->height, placeHeight );
        EXPECT_EQ( positive->x, placeX );
        EXPECT_EQ( positive->y, -placeY );

        // A '-' separator and an explicit '+-' sign are two spellings of the
        // same negative offset.
        const auto separator = grab::cli::parse_placement_geometry( "1600x900+200-30" );
        ASSERT_TRUE( separator.has_value() ) << separator.error().message;
        EXPECT_EQ( separator->x, placeX );
        EXPECT_EQ( separator->y, placeY );

        const auto signed_form =
            grab::cli::parse_placement_geometry( "1600x900+200+-30" );
        ASSERT_TRUE( signed_form.has_value() ) << signed_form.error().message;
        EXPECT_EQ( signed_form->y, placeY );
    }

    TEST( PlacementGeometry,
          RejectsMalformedAndZeroSizedInput )
    {
        EXPECT_FALSE( grab::cli::parse_placement_geometry( "" ).has_value() );
        EXPECT_FALSE( grab::cli::parse_placement_geometry( "1600x900" ).has_value() );
        EXPECT_FALSE( grab::cli::parse_placement_geometry( "1600+200+30" ).has_value() );
        EXPECT_FALSE(
            grab::cli::parse_placement_geometry( "0x900+200+30" ).has_value()
        );
        EXPECT_FALSE(
            grab::cli::parse_placement_geometry( "1600x900+200+30x" ).has_value()
        );
    }

    TEST( PlacementGeometry,
          FormatsBackIntoTheSpellingItParses )
    {
        const grab::geometry::Rectangle bounds{
            .x      = placeX,
            .y      = placeY,
            .width  = placeWidth,
            .height = placeHeight
        };

        const std::string text = grab::cli::format_geometry( bounds );

        EXPECT_EQ( text, "1600x900+200-30" );
        const auto parsed = grab::cli::parse_placement_geometry( text );
        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->x, bounds.x );
        EXPECT_EQ( parsed->y, bounds.y );
        EXPECT_EQ( parsed->width, bounds.width );
        EXPECT_EQ( parsed->height, bounds.height );
    }

    TEST( PlaceCommand,
          ParsesEveryOption )
    {
        const ArgumentVector args{
            { "--window-id",
             "4194311", "--geometry",
             "1600x900+200-30", "--display",
             ":1", "--timeout",
             "250" }
        };

        const auto options = grab::cli::parse_place_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        ASSERT_TRUE( options->selector.window_id.has_value() );
        EXPECT_EQ( *options->selector.window_id, firstWindowId );
        EXPECT_EQ( options->geometry.width, placeWidth );
        EXPECT_EQ( options->geometry.y, placeY );
        EXPECT_EQ( options->display, ":1" );
        EXPECT_EQ( options->timeout, placeTimeout );
    }

    TEST( PlaceCommand,
          DefaultsTheSettleTimeout )
    {
        const ArgumentVector args{
            { "--window", plotJugglerClass, "--geometry", "1600x900+200+30" }
        };

        const auto options = grab::cli::parse_place_options( args.span() );

        ASSERT_TRUE( options.has_value() ) << options.error().message;
        EXPECT_EQ( options->timeout, grab::Screen::defaultPlacementTimeout );
    }

    TEST( PlaceCommand,
          RequiresASelectorAndAGeometry )
    {
        const ArgumentVector noGeometry{
            { "--window", plotJugglerClass }
        };
        const ArgumentVector noSelector{
            { "--geometry", "1600x900+200+30" }
        };
        const ArgumentVector badTimeout{
            { "--window",
             plotJugglerClass, "--geometry",
             "1600x900+200+30", "--timeout",
             "soon" }
        };

        EXPECT_FALSE( grab::cli::parse_place_options( noGeometry.span() ).has_value() );
        EXPECT_FALSE( grab::cli::parse_place_options( noSelector.span() ).has_value() );
        EXPECT_FALSE( grab::cli::parse_place_options( badTimeout.span() ).has_value() );
    }

    TEST( SelectWindowId,
          PrefersTheExactIdAndFallsBackToTheFirstClassMatch )
    {
        const grab::cli::WindowSelector by_id{
            .wm_class  = {},
            .window_id = secondWindowId
        };
        const grab::cli::WindowSelector by_class{
            .wm_class  = lowercaseFragment,
            .window_id = std::nullopt
        };

        const auto identified = grab::cli::select_window_id( by_id, sample_windows() );
        ASSERT_TRUE( identified.has_value() ) << identified.error().message;
        EXPECT_EQ( *identified, secondWindowId );

        const auto classified =
            grab::cli::select_window_id( by_class, sample_windows() );
        ASSERT_TRUE( classified.has_value() ) << classified.error().message;
        EXPECT_EQ( *classified, firstWindowId );
    }

    TEST( SelectWindowId,
          ReportsMissesForBothSelectorShapes )
    {
        const grab::cli::WindowSelector absent_id{
            .wm_class  = {},
            .window_id = absentWindowId
        };
        const grab::cli::WindowSelector absent_class{
            .wm_class  = missingClass,
            .window_id = std::nullopt
        };

        const auto by_id = grab::cli::select_window_id( absent_id, sample_windows() );
        ASSERT_FALSE( by_id.has_value() );
        EXPECT_EQ( by_id.error().code, grab::ErrorCode::WindowNotFound );

        const auto by_class =
            grab::cli::select_window_id( absent_class, sample_windows() );
        ASSERT_FALSE( by_class.has_value() );
        EXPECT_EQ( by_class.error().code, grab::ErrorCode::WindowNotFound );
    }

}    // namespace
