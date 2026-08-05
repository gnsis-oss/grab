#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_draw.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
// clang-format on

namespace
{

    using grab::overlay::DrawInteraction;
    using grab::overlay::DrawKind;
    using grab::overlay::DrawStyle;

    constexpr grab::CoordinateSpaceId primarySpace{ 7U };
    constexpr grab::CoordinateSpaceId secondarySpace{ 8U };

    constexpr grab::SpacePoint origin{ .x = 0.0, .y = 0.0, .space = primarySpace };
    constexpr grab::SpacePoint downRight{ .x = 10.0, .y = 15.0, .space = primarySpace };
    constexpr grab::SpacePoint firstPathSample{
        .x     = 2.0,
        .y     = 2.0,
        .space = primarySpace,
    };
    constexpr grab::SpacePoint secondPathSample{
        .x     = 4.0,
        .y     = 4.0,
        .space = primarySpace,
    };
    constexpr grab::SpacePoint thirdPathSample{
        .x     = 6.0,
        .y     = 6.0,
        .space = primarySpace,
    };
    constexpr grab::SpacePoint pathCommitSample{
        .x     = 8.0,
        .y     = 8.0,
        .space = primarySpace,
    };
    constexpr grab::SpacePoint nearPathSample{
        .x     = 0.5,
        .y     = 0.5,
        .space = primarySpace,
    };
    constexpr grab::SpacePoint farPathSample{
        .x     = 3.0,
        .y     = 3.0,
        .space = primarySpace,
    };
    constexpr grab::SpacePoint degenerateX{ .x = 0.5, .y = 10.0, .space = primarySpace };
    constexpr grab::SpacePoint degenerateY{ .x = 10.0, .y = 0.5, .space = primarySpace };
    constexpr grab::SpacePoint barelyValid{ .x = 3.1, .y = 3.1, .space = primarySpace };
    constexpr grab::SpacePoint otherSpaceSample{
        .x     = 100.0,
        .y     = 100.0,
        .space = secondarySpace,
    };

    constexpr DrawStyle   defaultDrawStyle{};
    constexpr double      centerDivisor         = 2.0;
    constexpr double      expectedCenterX       = downRight.x / centerDivisor;
    constexpr double      expectedCenterY       = downRight.y / centerDivisor;

    constexpr std::size_t initialPathPointCount = 1U;
    constexpr std::size_t firstGeneratedSample  = 1U;
    constexpr std::size_t sampleNumberStep      = 1U;
    constexpr std::size_t generatedSampleCount  = grab::overlay::maxPathSamples;
    constexpr std::size_t lastAcceptedSample =
        grab::overlay::maxPathSamples - initialPathPointCount;
    constexpr double generatedSampleSpacing =
        grab::overlay::minPathSampleSpacingPx + 1.0;

    constexpr std::uint8_t         customRed   = 11U;
    constexpr std::uint8_t         customGreen = 22U;
    constexpr std::uint8_t         customBlue  = 33U;
    constexpr std::uint8_t         customAlpha = 44U;
    constexpr grab::overlay::Color customColor{
        .r = customRed,
        .g = customGreen,
        .b = customBlue,
        .a = customAlpha,
    };
    constexpr float     customStrokeWidth = 7.5F;
    constexpr DrawStyle filledStyle{
        .color     = customColor,
        .stroke_px = customStrokeWidth,
        .filled    = true,
    };
    constexpr DrawStyle strokedStyle{
        .color     = customColor,
        .stroke_px = customStrokeWidth,
        .filled    = false,
    };

    void
    expect_point( const grab::SpacePoint& actual,
                  const grab::SpacePoint& expected )
    {
        EXPECT_DOUBLE_EQ( actual.x, expected.x );
        EXPECT_DOUBLE_EQ( actual.y, expected.y );
        EXPECT_EQ( actual.space, expected.space );
    }

    void
    expect_color( const grab::overlay::Color& actual,
                  const grab::overlay::Color& expected )
    {
        EXPECT_EQ( actual.r, expected.r );
        EXPECT_EQ( actual.g, expected.g );
        EXPECT_EQ( actual.b, expected.b );
        EXPECT_EQ( actual.a, expected.a );
    }

    void
    expect_rectangle( const grab::overlay::Shape& shape,
                      double                      expectedX,
                      double                      expectedY,
                      double                      expectedWidth,
                      double                      expectedHeight )
    {
        const auto* rectangle = std::get_if<grab::overlay::Rect>( &shape.geometry );
        ASSERT_NE( rectangle, nullptr );
        EXPECT_DOUBLE_EQ( rectangle->bounds.x, expectedX );
        EXPECT_DOUBLE_EQ( rectangle->bounds.y, expectedY );
        EXPECT_DOUBLE_EQ( rectangle->bounds.w, expectedWidth );
        EXPECT_DOUBLE_EQ( rectangle->bounds.h, expectedHeight );
        EXPECT_EQ( rectangle->bounds.space, primarySpace );
    }

    void
    expect_open_path( const grab::overlay::Shape&       shape,
                      std::span<const grab::SpacePoint> expected )
    {
        const auto* path = std::get_if<grab::overlay::Path>( &shape.geometry );
        ASSERT_NE( path, nullptr );
        EXPECT_FALSE( path->closed );
        ASSERT_EQ( path->commands.size(), expected.size() );
        const auto* move = std::get_if<grab::overlay::MoveTo>( &path->commands.front() );
        ASSERT_NE( move, nullptr );
        expect_point( move->point, expected.front() );

        auto command = path->commands.cbegin();
        ++command;
        for( const auto& expectedPoint : expected.subspan( initialPathPointCount ) )
        {
            const auto* line = std::get_if<grab::overlay::LineTo>( &*command );
            ASSERT_NE( line, nullptr );
            expect_point( line->point, expectedPoint );
            ++command;
        }
    }

    void
    append_generated_path_samples( DrawInteraction& interaction )
    {
        for( std::size_t sampleNumber = firstGeneratedSample;
             sampleNumber <= generatedSampleCount;
             sampleNumber += sampleNumberStep )
        {
            const auto x = static_cast<double>( sampleNumber ) * generatedSampleSpacing;
            [[maybe_unused]]
            const auto preview = interaction.update(
                grab::SpacePoint{ .x = x, .y = origin.y, .space = primarySpace }
            );
        }
    }

    void
    expect_capped_path( const grab::overlay::Shape& shape )
    {
        const auto* path = std::get_if<grab::overlay::Path>( &shape.geometry );
        ASSERT_NE( path, nullptr );
        EXPECT_FALSE( path->closed );
        EXPECT_LE( path->commands.size(), grab::overlay::maxPathSamples );
        ASSERT_EQ( path->commands.size(), grab::overlay::maxPathSamples );
        const auto* first =
            std::get_if<grab::overlay::MoveTo>( &path->commands.front() );
        ASSERT_NE( first, nullptr );
        expect_point( first->point, origin );
        const auto* last = std::get_if<grab::overlay::LineTo>( &path->commands.back() );
        ASSERT_NE( last, nullptr );
        const auto lastX =
            static_cast<double>( lastAcceptedSample ) * generatedSampleSpacing;
        expect_point(
            last->point,
            grab::SpacePoint{ .x = lastX, .y = origin.y, .space = primarySpace }
        );
    }

}    // namespace

TEST( OverlayDraw,
      RectangleFromPressToDownRightHasExpectedBounds )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Rectangle, origin, defaultDrawStyle );

    EXPECT_TRUE( interaction.active() );
    EXPECT_EQ( interaction.kind(), DrawKind::Rectangle );
    const auto preview = interaction.update( downRight );
    ASSERT_TRUE( preview.has_value() );
    expect_rectangle( *preview, origin.x, origin.y, downRight.x, downRight.y );

    const auto shape = interaction.commit( downRight );
    ASSERT_TRUE( shape.has_value() );
    expect_rectangle( *shape, origin.x, origin.y, downRight.x, downRight.y );
    ASSERT_TRUE( shape->stroke.has_value() );
    EXPECT_FALSE( shape->fill.has_value() );
    EXPECT_FLOAT_EQ( shape->stroke->width_px, defaultDrawStyle.stroke_px );
    expect_color( shape->stroke->color, grab::overlay::defaultOverlayColor );
    EXPECT_FALSE( interaction.active() );
}

TEST( OverlayDraw,
      RectangleDraggedUpLeftHasNormalizedBounds )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Rectangle, downRight, defaultDrawStyle );

    const auto shape = interaction.commit( origin );

    ASSERT_TRUE( shape.has_value() );
    expect_rectangle( *shape, origin.x, origin.y, downRight.x, downRight.y );
}

TEST( OverlayDraw,
      EllipseIsInscribedInDragBox )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Ellipse, origin, defaultDrawStyle );

    const auto shape = interaction.commit( downRight );

    ASSERT_TRUE( shape.has_value() );
    const auto* ellipse = std::get_if<grab::overlay::Ellipse>( &shape->geometry );
    ASSERT_NE( ellipse, nullptr );
    EXPECT_DOUBLE_EQ( ellipse->center.x, expectedCenterX );
    EXPECT_DOUBLE_EQ( ellipse->center.y, expectedCenterY );
    EXPECT_EQ( ellipse->center.space, primarySpace );
    EXPECT_DOUBLE_EQ( ellipse->radius_x, expectedCenterX );
    EXPECT_DOUBLE_EQ( ellipse->radius_y, expectedCenterY );
}

TEST( OverlayDraw,
      PathFollowsSamplesInOrderAsOpenPolyline )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Path, origin, defaultDrawStyle );

    EXPECT_TRUE( interaction.update( firstPathSample ).has_value() );
    EXPECT_TRUE( interaction.update( secondPathSample ).has_value() );
    EXPECT_TRUE( interaction.update( thirdPathSample ).has_value() );
    const auto shape = interaction.commit( pathCommitSample );

    ASSERT_TRUE( shape.has_value() );
    constexpr std::array expected{
        origin,
        firstPathSample,
        secondPathSample,
        thirdPathSample,
        pathCommitSample,
    };
    expect_open_path( *shape, expected );
}

TEST( OverlayDraw,
      PathDropsSamplesCloserThanMinimumSpacing )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Path, origin, defaultDrawStyle );

    EXPECT_FALSE( interaction.update( nearPathSample ).has_value() );
    const auto preview = interaction.update( farPathSample );
    ASSERT_TRUE( preview.has_value() );
    constexpr std::array expected{ origin, farPathSample };
    expect_open_path( *preview, expected );

    const auto shape = interaction.commit( farPathSample );
    ASSERT_TRUE( shape.has_value() );
    expect_open_path( *shape, expected );
}

TEST( OverlayDraw,
      PathCapsSamplesAndRemainsValid )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Path, origin, defaultDrawStyle );

    append_generated_path_samples( interaction );
    const auto extraSampleNumber = generatedSampleCount + sampleNumberStep;
    const auto extraX =
        static_cast<double>( extraSampleNumber ) * generatedSampleSpacing;
    const auto cappedPreview = interaction.update(
        grab::SpacePoint{ .x = extraX, .y = origin.y, .space = primarySpace }
    );
    ASSERT_TRUE( cappedPreview.has_value() );
    expect_capped_path( *cappedPreview );

    const auto commitSampleNumber = extraSampleNumber + sampleNumberStep;
    const auto commitX =
        static_cast<double>( commitSampleNumber ) * generatedSampleSpacing;
    const auto shape = interaction.commit(
        grab::SpacePoint{ .x = commitX, .y = origin.y, .space = primarySpace }
    );
    ASSERT_TRUE( shape.has_value() );
    expect_capped_path( *shape );
}

TEST( OverlayDraw,
      PressReleaseWithoutDragIsDegenerateForEveryKind )
{
    constexpr std::array drawKinds{
        DrawKind::Rectangle,
        DrawKind::Ellipse,
        DrawKind::Path,
    };
    DrawInteraction interaction;

    for( const auto drawKind : drawKinds )
    {
        interaction.begin( drawKind, origin, defaultDrawStyle );
        EXPECT_FALSE( interaction.commit( origin ).has_value() );
        EXPECT_FALSE( interaction.active() );
    }
}

TEST( OverlayDraw,
      BoxShapeIsDegenerateWhenEitherExtentIsTooSmall )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Rectangle, origin, defaultDrawStyle );
    EXPECT_FALSE( interaction.commit( degenerateX ).has_value() );

    interaction.begin( DrawKind::Ellipse, origin, defaultDrawStyle );
    EXPECT_FALSE( interaction.commit( degenerateY ).has_value() );

    interaction.begin( DrawKind::Rectangle, origin, defaultDrawStyle );
    const auto shape = interaction.commit( barelyValid );
    ASSERT_TRUE( shape.has_value() );
    expect_rectangle( *shape, origin.x, origin.y, barelyValid.x, barelyValid.y );
}

TEST( OverlayDraw,
      CancelDeactivatesAndPreventsUpdates )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Rectangle, origin, defaultDrawStyle );

    interaction.cancel();

    EXPECT_FALSE( interaction.active() );
    EXPECT_FALSE( interaction.update( downRight ).has_value() );
}

TEST( OverlayDraw,
      SamplesFromDifferentSpaceAreIgnored )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Path, origin, defaultDrawStyle );

    EXPECT_FALSE( interaction.update( otherSpaceSample ).has_value() );
    const auto shape = interaction.commit( secondPathSample );

    ASSERT_TRUE( shape.has_value() );
    constexpr std::array expected{ origin, secondPathSample };
    expect_open_path( *shape, expected );
}

TEST( OverlayDraw,
      DrawStyleSelectsFillOrStrokeAndPreservesProperties )
{
    DrawInteraction interaction;
    interaction.begin( DrawKind::Rectangle, origin, filledStyle );
    const auto filled = interaction.commit( downRight );
    ASSERT_TRUE( filled.has_value() );
    ASSERT_TRUE( filled->fill.has_value() );
    EXPECT_FALSE( filled->stroke.has_value() );
    expect_color( filled->fill->color, customColor );

    interaction.begin( DrawKind::Rectangle, origin, strokedStyle );
    const auto stroked = interaction.commit( downRight );
    ASSERT_TRUE( stroked.has_value() );
    ASSERT_TRUE( stroked->stroke.has_value() );
    EXPECT_FALSE( stroked->fill.has_value() );
    expect_color( stroked->stroke->color, customColor );
    EXPECT_FLOAT_EQ( stroked->stroke->width_px, customStrokeWidth );
}
