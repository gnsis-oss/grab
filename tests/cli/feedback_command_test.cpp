#include "frontends/cli/feedback_command.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view          noClickFlag       = "--no-click";
    constexpr std::string_view          noHoldFlag        = "--no-hold";
    constexpr std::string_view          holdMsFlag        = "--hold-ms";
    constexpr std::string_view          doubleClickMsFlag = "--double-click-ms";
    constexpr std::string_view          pauseMsFlag       = "--pause-ms";
    constexpr std::string_view          slopPxFlag        = "--slop-px";
    constexpr std::string_view          rippleRadiusFlag  = "--ripple-radius";
    constexpr std::string_view          rippleMsFlag      = "--ripple-ms";
    constexpr std::string_view          barWidthFlag      = "--bar-width";
    constexpr std::string_view          barHeightFlag     = "--bar-height";
    constexpr std::string_view          unknownFlag       = "--unknown";
    constexpr std::string_view          malformedValue    = "not-a-number";
    constexpr std::string_view          negativeValue     = "-1";
    constexpr std::string_view          customHoldText    = "875";
    constexpr std::string_view          customDoubleText  = "325";
    constexpr std::string_view          customPauseText   = "950";
    constexpr std::string_view          customSlopText    = "7.25";
    constexpr std::string_view          customRadiusText  = "32.5";
    constexpr std::string_view          customRippleText  = "650";
    constexpr std::string_view          customWidthText   = "90.5";
    constexpr std::string_view          customHeightText  = "8.25";

    constexpr std::chrono::milliseconds defaultHold{ 500 };
    constexpr std::chrono::milliseconds defaultDoubleClick{ 400 };
    constexpr std::chrono::milliseconds defaultPause{ 700 };
    constexpr std::chrono::milliseconds defaultRipple{ 400 };
    constexpr double                    defaultSlopPx       = 5.0;
    constexpr double                    defaultRippleRadius = 48.0;
    constexpr double                    defaultBarWidth     = 64.0;
    constexpr double                    defaultBarHeight    = 6.0;
    constexpr double                    defaultBarOffsetY   = 24.0;

    constexpr std::chrono::milliseconds customHold{ 875 };
    constexpr std::chrono::milliseconds customDoubleClick{ 325 };
    constexpr std::chrono::milliseconds customPause{ 950 };
    constexpr std::chrono::milliseconds customRipple{ 650 };
    constexpr double                    customSlopPx       = 7.25;
    constexpr double                    customRippleRadius = 32.5;
    constexpr double                    customBarWidth     = 90.5;
    constexpr double                    customBarHeight    = 8.25;

    TEST( FeedbackCommand,
          DefaultsEnableClickAndHoldStyles )
    {
        const auto parsed = grab::cli::parse_feedback_options( {} );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_TRUE( parsed->click.has_value() );
        EXPECT_DOUBLE_EQ( parsed->click->radius_px, defaultRippleRadius );
        EXPECT_EQ( parsed->click->duration, defaultRipple );
        ASSERT_TRUE( parsed->hold.has_value() );
        EXPECT_DOUBLE_EQ( parsed->hold->width_px, defaultBarWidth );
        EXPECT_DOUBLE_EQ( parsed->hold->height_px, defaultBarHeight );
        EXPECT_DOUBLE_EQ( parsed->hold->offset_y_px, defaultBarOffsetY );
        EXPECT_EQ( parsed->thresholds.hold, defaultHold );
        EXPECT_EQ( parsed->thresholds.double_click, defaultDoubleClick );
        EXPECT_EQ( parsed->thresholds.pause, defaultPause );
        EXPECT_DOUBLE_EQ( parsed->thresholds.slop_px, defaultSlopPx );
    }

    TEST( FeedbackCommand,
          DisableFlagsClearOnlyTheirSelectedStyles )
    {
        constexpr auto no_click = std::to_array<std::string_view>( { noClickFlag } );
        constexpr auto no_hold  = std::to_array<std::string_view>( { noHoldFlag } );
        constexpr auto neither =
            std::to_array<std::string_view>( { noClickFlag, noHoldFlag } );

        const auto click_disabled = grab::cli::parse_feedback_options( no_click );
        const auto hold_disabled  = grab::cli::parse_feedback_options( no_hold );
        const auto both_disabled  = grab::cli::parse_feedback_options( neither );

        ASSERT_TRUE( click_disabled.has_value() ) << click_disabled.error().message;
        EXPECT_FALSE( click_disabled->click.has_value() );
        EXPECT_TRUE( click_disabled->hold.has_value() );
        ASSERT_TRUE( hold_disabled.has_value() ) << hold_disabled.error().message;
        EXPECT_TRUE( hold_disabled->click.has_value() );
        EXPECT_FALSE( hold_disabled->hold.has_value() );
        ASSERT_TRUE( both_disabled.has_value() ) << both_disabled.error().message;
        EXPECT_FALSE( both_disabled->click.has_value() );
        EXPECT_FALSE( both_disabled->hold.has_value() );
    }

    TEST( FeedbackCommand,
          TimingAndSlopFlagsMapToGestureThresholds )
    {
        constexpr auto args   = std::to_array<std::string_view>( {
            holdMsFlag,
            customHoldText,
            doubleClickMsFlag,
            customDoubleText,
            pauseMsFlag,
            customPauseText,
            slopPxFlag,
            customSlopText,
        } );

        const auto     parsed = grab::cli::parse_feedback_options( args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->thresholds.hold, customHold );
        EXPECT_EQ( parsed->thresholds.double_click, customDoubleClick );
        EXPECT_EQ( parsed->thresholds.pause, customPause );
        EXPECT_DOUBLE_EQ( parsed->thresholds.slop_px, customSlopPx );
    }

    TEST( FeedbackCommand,
          RippleAndBarFlagsMapToStyles )
    {
        constexpr auto args   = std::to_array<std::string_view>( {
            rippleRadiusFlag,
            customRadiusText,
            rippleMsFlag,
            customRippleText,
            barWidthFlag,
            customWidthText,
            barHeightFlag,
            customHeightText,
        } );

        const auto     parsed = grab::cli::parse_feedback_options( args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_TRUE( parsed->click.has_value() );
        EXPECT_DOUBLE_EQ( parsed->click->radius_px, customRippleRadius );
        EXPECT_EQ( parsed->click->duration, customRipple );
        ASSERT_TRUE( parsed->hold.has_value() );
        EXPECT_DOUBLE_EQ( parsed->hold->width_px, customBarWidth );
        EXPECT_DOUBLE_EQ( parsed->hold->height_px, customBarHeight );
    }

    TEST( FeedbackCommand,
          RejectsUnknownMissingMalformedNegativeAndRepeatedOptions )
    {
        constexpr auto unknown = std::to_array<std::string_view>( { unknownFlag } );
        constexpr auto missing = std::to_array<std::string_view>( { holdMsFlag } );
        constexpr auto malformed =
            std::to_array<std::string_view>( { slopPxFlag, malformedValue } );
        constexpr auto negative =
            std::to_array<std::string_view>( { rippleMsFlag, negativeValue } );
        constexpr auto repeated =
            std::to_array<std::string_view>( { noClickFlag, noClickFlag } );

        const auto unknown_result   = grab::cli::parse_feedback_options( unknown );
        const auto missing_result   = grab::cli::parse_feedback_options( missing );
        const auto malformed_result = grab::cli::parse_feedback_options( malformed );
        const auto negative_result  = grab::cli::parse_feedback_options( negative );
        const auto repeated_result  = grab::cli::parse_feedback_options( repeated );

        ASSERT_FALSE( unknown_result.has_value() );
        EXPECT_EQ( unknown_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( unknown_result.error().message.contains( unknownFlag ) );
        ASSERT_FALSE( missing_result.has_value() );
        EXPECT_EQ( missing_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( missing_result.error().message.contains( holdMsFlag ) );
        ASSERT_FALSE( malformed_result.has_value() );
        EXPECT_EQ( malformed_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( malformed_result.error().message.contains( slopPxFlag ) );
        ASSERT_FALSE( negative_result.has_value() );
        EXPECT_EQ( negative_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( negative_result.error().message.contains( rippleMsFlag ) );
        ASSERT_FALSE( repeated_result.has_value() );
        EXPECT_EQ( repeated_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( repeated_result.error().message.contains( noClickFlag ) );
    }

}    // namespace
