#include "frontends/cli/overlay_command.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
// clang-format on

namespace
{

    constexpr std::chrono::milliseconds defaultTtl{ 3'000 };
    constexpr std::chrono::milliseconds customFade{ 725 };
    constexpr std::chrono::milliseconds customTrailFade{ 2'400 };
    constexpr float                     customTrailWidth = 5.5F;
    // The parser emits space-unresolved geometry; open_shape stamps the
    // overlay surface's real space at the session boundary.
    constexpr std::uint32_t             unresolvedSpaceValue = 0U;
    constexpr std::size_t               pathPointCount       = 3U;

    void
    expect_color( const grab::overlay::Color& actual,
                  const grab::overlay::Color& expected )
    {
        EXPECT_EQ( actual.r, expected.r );
        EXPECT_EQ( actual.g, expected.g );
        EXPECT_EQ( actual.b, expected.b );
        EXPECT_EQ( actual.a, expected.a );
    }

    TEST( OverlayCommand,
          RectDefaultsToThreeSecondTtl )
    {
        constexpr auto args =
            std::to_array<std::string_view>( { "--at", "10,20,30,40" } );

        const auto parsed = grab::cli::parse_overlay_shape_options( "rect", args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_TRUE( parsed->wait_for.has_value() );
        EXPECT_EQ( *parsed->wait_for, defaultTtl );
        const auto* const lifetime =
            std::get_if<grab::overlay::Ttl>( &parsed->shape.lifetime );
        ASSERT_NE( lifetime, nullptr );
        EXPECT_EQ( lifetime->duration, defaultTtl );
        const auto* const rectangle =
            std::get_if<grab::overlay::Rect>( &parsed->shape.geometry );
        ASSERT_NE( rectangle, nullptr );
        EXPECT_DOUBLE_EQ( rectangle->bounds.x, 10.0 );
        EXPECT_DOUBLE_EQ( rectangle->bounds.y, 20.0 );
        EXPECT_DOUBLE_EQ( rectangle->bounds.w, 30.0 );
        EXPECT_DOUBLE_EQ( rectangle->bounds.h, 40.0 );
        EXPECT_EQ( rectangle->bounds.space.value, unresolvedSpaceValue );
        ASSERT_TRUE( parsed->shape.stroke.has_value() );
        expect_color( parsed->shape.stroke->color, grab::overlay::defaultOverlayColor );
    }

    TEST( OverlayCommand,
          EllipseAcceptsFadeLifetime )
    {
        constexpr auto args = std::to_array<std::string_view>( {
            "--fade",
            "725",
            "--at",
            "100,120,30,15",
        } );

        const auto parsed   = grab::cli::parse_overlay_shape_options( "ellipse", args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        ASSERT_EQ( parsed->wait_for, customFade );
        const auto* const lifetime =
            std::get_if<grab::overlay::Fade>( &parsed->shape.lifetime );
        ASSERT_NE( lifetime, nullptr );
        EXPECT_EQ( lifetime->duration, customFade );
        const auto* const ellipse =
            std::get_if<grab::overlay::Ellipse>( &parsed->shape.geometry );
        ASSERT_NE( ellipse, nullptr );
        EXPECT_DOUBLE_EQ( ellipse->center.x, 100.0 );
        EXPECT_DOUBLE_EQ( ellipse->center.y, 120.0 );
        EXPECT_DOUBLE_EQ( ellipse->radius_x, 30.0 );
        EXPECT_DOUBLE_EQ( ellipse->radius_y, 15.0 );
    }

    TEST( OverlayCommand,
          PathHoldBuildsPersistentLineCommands )
    {
        constexpr auto args   = std::to_array<std::string_view>( {
            "--at",
            "5,6,25,26,45,46",
            "--hold",
        } );

        const auto     parsed = grab::cli::parse_overlay_shape_options( "path", args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_FALSE( parsed->wait_for.has_value() );
        EXPECT_TRUE(
            std::holds_alternative<grab::overlay::Persistent>( parsed->shape.lifetime )
        );
        const auto* const path =
            std::get_if<grab::overlay::Path>( &parsed->shape.geometry );
        ASSERT_NE( path, nullptr );
        EXPECT_EQ( path->commands.size(), pathPointCount );
        EXPECT_TRUE(
            std::holds_alternative<grab::overlay::MoveTo>( path->commands.front() )
        );
        EXPECT_TRUE(
            std::holds_alternative<grab::overlay::LineTo>( path->commands.back() )
        );
    }

    TEST( OverlayCommand,
          RejectsConflictingLifetimeFlags )
    {
        constexpr auto args   = std::to_array<std::string_view>( {
            "--at",
            "10,20,30,40",
            "--ttl",
            "100",
            "--hold",
        } );

        const auto     parsed = grab::cli::parse_overlay_shape_options( "rect", args );

        ASSERT_FALSE( parsed.has_value() );
        EXPECT_EQ( parsed.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( parsed.error().message.contains( "one lifetime policy" ) );
    }

    TEST( OverlayCommand,
          TrailOptionsDefaultBothOriginsToDefaultOverlayColor )
    {
        const grab::cli::OverlayTrailOptions options{};

        expect_color( options.physical_color, grab::overlay::defaultOverlayColor );
        expect_color( options.injected_color, grab::overlay::defaultOverlayColor );
    }

    TEST( OverlayCommand,
          TrailDefaultsBothOriginsToDefaultOverlayColor )
    {
        const auto parsed = grab::cli::parse_overlay_trail_options( {} );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        expect_color( parsed->physical_color, grab::overlay::defaultOverlayColor );
        expect_color( parsed->injected_color, grab::overlay::defaultOverlayColor );
    }

    TEST( OverlayCommand,
          TrailParsesRgbFadeAndWidthOverrides )
    {
        constexpr auto args   = std::to_array<std::string_view>( {
            "--color",
            "12aBcF",
            "--injected-color",
            "0102fe",
            "--fade-ms",
            "2400",
            "--width",
            "5.5",
        } );

        const auto     parsed = grab::cli::parse_overlay_trail_options( args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->physical_color.r, 0X12U );
        EXPECT_EQ( parsed->physical_color.g, 0XABU );
        EXPECT_EQ( parsed->physical_color.b, 0XCFU );
        EXPECT_EQ( parsed->injected_color.r, 0X01U );
        EXPECT_EQ( parsed->injected_color.g, 0X02U );
        EXPECT_EQ( parsed->injected_color.b, 0XFEU );
        EXPECT_EQ( parsed->fade, customTrailFade );
        EXPECT_FLOAT_EQ( parsed->width_px, customTrailWidth );
    }

    TEST( OverlayCommand,
          TrailRejectsMalformedValues )
    {
        constexpr auto badColor =
            std::to_array<std::string_view>( { "--color", "12345" } );
        constexpr auto badFade = std::to_array<std::string_view>( { "--fade-ms", "0" } );
        constexpr auto badWidth =
            std::to_array<std::string_view>( { "--width", "nan" } );

        EXPECT_FALSE( grab::cli::parse_overlay_trail_options( badColor ).has_value() );
        EXPECT_FALSE( grab::cli::parse_overlay_trail_options( badFade ).has_value() );
        EXPECT_FALSE( grab::cli::parse_overlay_trail_options( badWidth ).has_value() );
    }

}    // namespace
