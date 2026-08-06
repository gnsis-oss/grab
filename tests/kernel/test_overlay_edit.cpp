#include "grab/geometry/point.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/overlay.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_edit.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    using grab::kernel::presentation::edit_input_region;
    using grab::kernel::presentation::EditGeometryOptions;
    using grab::kernel::presentation::EditInteraction;
    using grab::kernel::presentation::hit_test;
    using grab::kernel::presentation::max_region_rects;
    using grab::kernel::presentation::min_size_px;

    constexpr grab::CoordinateSpaceId   testSpace{ 7U };
    constexpr grab::overlay::SceneEpoch testEpoch{ 3U };
    constexpr std::uint32_t             firstSlot   = 1U;
    constexpr std::uint32_t             secondSlot  = 2U;
    constexpr std::uint32_t             slotStep    = 1U;
    constexpr std::int32_t              equalZ      = 4;
    constexpr std::int32_t              lowerZ      = -2;
    constexpr std::int32_t              higherZ     = 9;

    constexpr double                    rectLeft    = 10.0;
    constexpr double                    rectTop     = 20.0;
    constexpr double                    rectWidth   = 40.0;
    constexpr double                    rectHeight  = 30.0;
    constexpr double                    rectRight   = rectLeft + rectWidth;
    constexpr double                    rectBottom  = rectTop + rectHeight;
    constexpr double                    rectCenterX = rectLeft + ( rectWidth * 0.5 );
    constexpr double                    rectCenterY = rectTop + ( rectHeight * 0.5 );
    constexpr double                    rectRadiusX = rectWidth * 0.5;
    constexpr double                    rectRadiusY = rectHeight * 0.5;
    constexpr double                    overlapLeft = 10.0;
    constexpr double                    overlapTop  = 10.0;
    constexpr double                    overlapSize = 100.0;
    constexpr double       overlapCenter          = overlapLeft + ( overlapSize * 0.5 );
    constexpr double       strokeWidth            = 2.0;
    constexpr float        strokeWidthFloat       = static_cast<float>( strokeWidth );
    constexpr double       dragX                  = 7.0;
    constexpr double       dragY                  = -5.0;
    constexpr double       outwardDrag            = 5.0;
    constexpr double       extendedRight          = rectRight + outwardDrag;
    constexpr double       extendedBottom         = rectBottom + outwardDrag;
    constexpr double       reducedLeft            = rectLeft - outwardDrag;
    constexpr double       reducedTop             = rectTop - outwardDrag;
    constexpr double       resizedEllipseCenterX  = ( rectLeft + extendedRight ) * 0.5;
    constexpr double       resizedEllipseCenterY  = ( rectTop + extendedBottom ) * 0.5;
    constexpr double       resizedEllipseRadiusX  = ( extendedRight - rectLeft ) * 0.5;
    constexpr double       resizedEllipseRadiusY  = ( extendedBottom - rectTop ) * 0.5;
    constexpr double       pastAnchor             = -100.0;
    constexpr double       beyondAnchor           = 100.0;

    constexpr double       ellipseCenterX         = 100.0;
    constexpr double       ellipseCenterY         = 100.0;
    constexpr double       ellipseRadiusX         = 40.0;
    constexpr double       ellipseRadiusY         = 30.0;
    constexpr double       ellipseTransparentX    = 67.0;
    constexpr double       ellipseTransparentY    = 75.0;
    constexpr double       ellipseStrokeX         = 120.0;
    constexpr double       ellipseStrokeY         = 125.98076211353316;

    constexpr double       pathLeft               = 10.0;
    constexpr double       pathTop                = 10.0;
    constexpr double       pathRight              = 70.0;
    constexpr double       pathBottom             = 50.0;
    constexpr double       pathCenterX            = 40.0;
    constexpr double       pathCenterY            = 30.0;
    constexpr double       firstBezierY           = 20.0;
    constexpr double       secondBezierY          = 40.0;
    constexpr std::size_t  expectedPathPointCount = 6U;
    constexpr double       resizedPathRight       = pathRight + outwardDrag;
    constexpr double       resizedPathBottom      = pathBottom + outwardDrag;

    constexpr double       lowerHandleLeft        = 10.0;
    constexpr double       lowerHandleTop         = 10.0;
    constexpr double       lowerHandleSize        = 40.0;
    constexpr double       sharedHandle           = lowerHandleLeft + lowerHandleSize;
    constexpr double       topBodyLeft            = 40.0;
    constexpr double       topBodyTop             = 40.0;
    constexpr double       topBodySize            = 40.0;
    constexpr double       resizedHandleEdge      = 60.0;
    constexpr double       resizedHandleSize      = resizedHandleEdge - lowerHandleLeft;

    constexpr double       regionEllipseCenter    = 100.0;
    constexpr double       regionEllipseRadius    = 30.0;
    constexpr std::int32_t transparentRegionX     = 75;
    constexpr std::int32_t transparentRegionY     = 75;
    constexpr std::size_t  capShapeCount          = 12U;
    constexpr double       farShapeStart          = 500.0;
    constexpr double       farShapeSpacing        = 30.0;
    constexpr double       farShapeTop            = 500.0;
    constexpr double       smallShapeExtent       = 4.0;

    [[nodiscard]]
    grab::SpacePoint
    point( double x,
           double y )
    {
        return grab::SpacePoint{ .x = x, .y = y, .space = testSpace };
    }

    [[nodiscard]]
    grab::overlay::ShapeId
    shape_id( std::uint32_t slot )
    {
        return grab::overlay::ShapeId{
            .epoch = testEpoch,
            .slot  = slot,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    filled_rect( double              x,
                 double              y,
                 double              width,
                 double              height,
                 grab::overlay::Band band = grab::overlay::Band::Annotation,
                 std::int32_t        z    = equalZ )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{
            .bounds = grab::SpaceRect{
                                      .x     = x,
                                      .y     = y,
                                      .w     = width,
                                      .h     = height,
                                      .space = testSpace,
                                      },
        };
        shape.fill.emplace();
        shape.band = band;
        shape.z    = z;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    stroked_ellipse()
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Ellipse{
            .center   = point( ellipseCenterX, ellipseCenterY ),
            .radius_x = ellipseRadiusX,
            .radius_y = ellipseRadiusY,
        };
        shape.stroke.emplace();
        shape.stroke->width_px = strokeWidthFloat;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    rectangle_path( bool filled )
    {
        grab::overlay::Path path;
        path.commands = {
            grab::overlay::MoveTo{ .point = point( pathLeft, pathTop ) },
            grab::overlay::LineTo{ .point = point( pathRight, pathTop ) },
            grab::overlay::BezierTo{
                                  .control =
                    {
                        point( pathRight, firstBezierY ),
                        point( pathRight, secondBezierY ),
                        point( pathRight, pathBottom ),
                    }, },
            grab::overlay::LineTo{ .point = point( pathLeft, pathBottom ) },
            grab::overlay::ClosePath{},
        };

        grab::overlay::Shape shape;
        shape.geometry = std::move( path );
        if( filled )
        {
            shape.fill.emplace();
        }
        else
        {
            shape.stroke.emplace();
            shape.stroke->width_px = strokeWidthFloat;
        }
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    filled_ellipse_for_resize()
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Ellipse{
            .center   = point( rectCenterX, rectCenterY ),
            .radius_x = rectRadiusX,
            .radius_y = rectRadiusY,
        };
        shape.fill.emplace();
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    filled_polygon_for_resize()
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Polygon{
            .points = {
                       point( rectLeft, rectTop ),
                       point( rectRight, rectTop ),
                       point( rectRight, rectBottom ),
                       point( rectLeft, rectBottom ),
                       },
        };
        shape.fill.emplace();
        return shape;
    }

    [[nodiscard]]
    grab::overlay::ShapeRecord
    record( grab::overlay::Shape shape,
            std::uint32_t        slot )
    {
        return grab::overlay::ShapeRecord{
            .id    = shape_id( slot ),
            .shape = std::move( shape ),
        };
    }

    [[nodiscard]]
    const grab::overlay::Rect&
    as_rect( const grab::overlay::Shape& shape )
    {
        return std::get<grab::overlay::Rect>( shape.geometry );
    }

    void
    expect_rect( const grab::overlay::Shape& shape,
                 double                      x,
                 double                      y,
                 double                      width,
                 double                      height )
    {
        const auto& rect = as_rect( shape );
        EXPECT_DOUBLE_EQ( rect.bounds.x, x );
        EXPECT_DOUBLE_EQ( rect.bounds.y, y );
        EXPECT_DOUBLE_EQ( rect.bounds.w, width );
        EXPECT_DOUBLE_EQ( rect.bounds.h, height );
        EXPECT_EQ( rect.bounds.space, testSpace );
    }

    [[nodiscard]]
    std::vector<grab::SpacePoint>
    path_points( const grab::overlay::Shape& shape )
    {
        const auto& path = std::get<grab::overlay::Path>( shape.geometry );
        std::vector<grab::SpacePoint> result;
        for( const auto& command : path.commands )
        {
            if( const auto* move = std::get_if<grab::overlay::MoveTo>( &command ) )
            {
                result.push_back( move->point );
            }
            else if( const auto* line = std::get_if<grab::overlay::LineTo>( &command ) )
            {
                result.push_back( line->point );
            }
            else if( const auto* bezier =
                         std::get_if<grab::overlay::BezierTo>( &command ) )
            {
                result.insert( result.end(),
                               bezier->control.begin(),
                               bezier->control.end() );
            }
        }
        return result;
    }

    [[nodiscard]]
    std::pair<double,
              double>
    horizontal_extent( std::span<const grab::SpacePoint> points )
    {
        const auto [minimum, maximum] =
            std::ranges::minmax_element( points, {}, &grab::SpacePoint::x );
        return { minimum->x, maximum->x };
    }

    [[nodiscard]]
    std::pair<double,
              double>
    vertical_extent( std::span<const grab::SpacePoint> points )
    {
        const auto [minimum, maximum] =
            std::ranges::minmax_element( points, {}, &grab::SpacePoint::y );
        return { minimum->y, maximum->y };
    }

    [[nodiscard]]
    bool
    region_contains( std::span<const grab::geometry::Rectangle> region,
                     grab::geometry::Point                      at )
    {
        return std::ranges::any_of( region,
                                    [at]( grab::geometry::Rectangle rectangle )
                                    {
                                        return rectangle.contains( at );
                                    } );
    }

}    // namespace

TEST( OverlayEdit,
      ReversePaintOrderUsesBandBeforeEqualZ )
{
    const auto       annotation = record( filled_rect( overlapLeft,
                                                       overlapTop,
                                                       overlapSize,
                                                       overlapSize,
                                                       grab::overlay::Band::Annotation,
                                                       equalZ ),
                                          secondSlot );
    const auto       trail      = record( filled_rect( overlapLeft,
                                                       overlapTop,
                                                       overlapSize,
                                                       overlapSize,
                                                       grab::overlay::Band::Trail,
                                                       equalZ ),
                                          firstSlot );
    const std::array shapes{ trail, annotation };
    const std::array editable{ annotation.id, trail.id };

    const auto       hit = hit_test( shapes,
                                     editable,
                                     point( overlapCenter, overlapCenter ),
                                     EditGeometryOptions{} );

    ASSERT_TRUE( hit.has_value() );
    EXPECT_EQ( *hit, trail.id );
}

TEST( OverlayEdit,
      EllipseTransparentCornerMissesAndStrokeHits )
{
    const std::array shapes{ record( stroked_ellipse(), firstSlot ) };
    const std::array editable{ shapes.front().id };

    EXPECT_FALSE( hit_test( shapes,
                            editable,
                            point( ellipseTransparentX, ellipseTransparentY ),
                            EditGeometryOptions{} )
                      .has_value() );
    EXPECT_EQ( hit_test( shapes,
                         editable,
                         point( ellipseStrokeX, ellipseStrokeY ),
                         EditGeometryOptions{} ),
               std::optional{ shapes.front().id } );
}

TEST( OverlayEdit,
      HollowPathInteriorMisses )
{
    const std::array shapes{ record( rectangle_path( false ), firstSlot ) };
    const std::array editable{ shapes.front().id };

    EXPECT_FALSE( hit_test( shapes,
                            editable,
                            point( pathCenterX, pathCenterY ),
                            EditGeometryOptions{} )
                      .has_value() );
}

TEST( OverlayEdit,
      NonEditableTopShapeIsSkipped )
{
    const auto editable_shape = record( filled_rect( overlapLeft,
                                                     overlapTop,
                                                     overlapSize,
                                                     overlapSize,
                                                     grab::overlay::Band::Annotation,
                                                     lowerZ ),
                                        firstSlot );
    const auto non_editable   = record( filled_rect( overlapLeft,
                                                     overlapTop,
                                                     overlapSize,
                                                     overlapSize,
                                                     grab::overlay::Band::Trail,
                                                     higherZ ),
                                        secondSlot );
    const std::array shapes{ editable_shape, non_editable };
    const std::array editable{ editable_shape.id };

    EXPECT_EQ( hit_test( shapes,
                         editable,
                         point( overlapCenter, overlapCenter ),
                         EditGeometryOptions{} ),
               std::optional{ editable_shape.id } );
}

TEST( OverlayEdit,
      ResizeHandleTakesPriorityOverBody )
{
    const auto       lower = record( filled_rect( lowerHandleLeft,
                                                  lowerHandleTop,
                                                  lowerHandleSize,
                                                  lowerHandleSize,
                                                  grab::overlay::Band::Annotation,
                                                  lowerZ ),
                                     firstSlot );
    const auto       upper = record( filled_rect( topBodyLeft,
                                                  topBodyTop,
                                                  topBodySize,
                                                  topBodySize,
                                                  grab::overlay::Band::Trail,
                                                  higherZ ),
                                     secondSlot );
    const std::array shapes{ lower, upper };
    const std::array editable{ lower.id, upper.id };

    EXPECT_EQ( hit_test( shapes,
                         editable,
                         point( sharedHandle, sharedHandle ),
                         EditGeometryOptions{} ),
               std::optional{ lower.id } );

    EditInteraction interaction;
    ASSERT_TRUE( interaction.begin( shapes,
                                    editable,
                                    point( sharedHandle, sharedHandle ),
                                    EditGeometryOptions{} ) );
    EXPECT_EQ( interaction.target(), lower.id );
    const auto resized =
        interaction.update( point( resizedHandleEdge, resizedHandleEdge ) );
    ASSERT_TRUE( resized.has_value() );
    expect_rect( *resized,
                 lowerHandleLeft,
                 lowerHandleTop,
                 resizedHandleSize,
                 resizedHandleSize );
}

// GTest assertion macros account for most of this apparent complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( OverlayEdit,
      MoveTranslatesEveryPathPointAndPreservesSize )
{
    const std::array shapes{ record( rectangle_path( true ), firstSlot ) };
    const std::array editable{ shapes.front().id };
    const auto       original_points = path_points( shapes.front().shape );
    ASSERT_EQ( original_points.size(), expectedPathPointCount );

    EditInteraction interaction;
    ASSERT_TRUE( interaction.begin( shapes,
                                    editable,
                                    point( pathCenterX, pathCenterY ),
                                    EditGeometryOptions{} ) );
    const auto moved =
        interaction.update( point( pathCenterX + dragX, pathCenterY + dragY ) );
    ASSERT_TRUE( moved.has_value() );
    const auto moved_points = path_points( *moved );
    ASSERT_EQ( moved_points.size(), original_points.size() );

    for( std::size_t index{}; index < original_points.size(); ++index )
    {
        EXPECT_DOUBLE_EQ( moved_points.at( index ).x,
                          original_points.at( index ).x + dragX );
        EXPECT_DOUBLE_EQ( moved_points.at( index ).y,
                          original_points.at( index ).y + dragY );
        EXPECT_EQ( moved_points.at( index ).space, original_points.at( index ).space );
    }
    const auto original_horizontal = horizontal_extent( original_points );
    const auto original_vertical   = vertical_extent( original_points );
    const auto moved_horizontal    = horizontal_extent( moved_points );
    const auto moved_vertical      = vertical_extent( moved_points );
    EXPECT_DOUBLE_EQ( moved_horizontal.second - moved_horizontal.first,
                      original_horizontal.second - original_horizontal.first );
    EXPECT_DOUBLE_EQ( moved_vertical.second - moved_vertical.first,
                      original_vertical.second - original_vertical.first );
}

// GTest assertion macros account for most of this apparent complexity.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST( OverlayEdit,
      ResizeFourCornersKeepsOppositeCornerAnchored )
{
    struct CornerCase
    {
            grab::SpacePoint begin;
            grab::SpacePoint end;
            double           expected_x{};
            double           expected_y{};
            double           expected_width{};
            double           expected_height{};
    };

    const std::array cases{
        CornerCase{
                   .begin           = point( rectLeft,    rectTop ),
                   .end             = point( reducedLeft,     reducedTop ),
                   .expected_x      = reducedLeft,
                   .expected_y      = reducedTop,
                   .expected_width  = rectRight - reducedLeft,
                   .expected_height = rectBottom - reducedTop,
                   },
        CornerCase{
                   .begin           = point( rectRight,    rectTop ),
                   .end             = point( extendedRight,     reducedTop ),
                   .expected_x      = rectLeft,
                   .expected_y      = reducedTop,
                   .expected_width  = extendedRight - rectLeft,
                   .expected_height = rectBottom - reducedTop,
                   },
        CornerCase{
                   .begin           = point( rectRight, rectBottom ),
                   .end             = point( extendedRight, extendedBottom ),
                   .expected_x      = rectLeft,
                   .expected_y      = rectTop,
                   .expected_width  = extendedRight - rectLeft,
                   .expected_height = extendedBottom - rectTop,
                   },
        CornerCase{
                   .begin           = point( rectLeft, rectBottom ),
                   .end             = point( reducedLeft, extendedBottom ),
                   .expected_x      = reducedLeft,
                   .expected_y      = rectTop,
                   .expected_width  = rectRight - reducedLeft,
                   .expected_height = extendedBottom - rectTop,
                   },
    };
    const auto rectangular_shape =
        record( filled_rect( rectLeft, rectTop, rectWidth, rectHeight ), firstSlot );
    const std::array shapes{ rectangular_shape };
    const std::array editable{ shapes.front().id };

    for( const auto& corner : cases )
    {
        EditInteraction interaction;
        ASSERT_TRUE(
            interaction.begin( shapes, editable, corner.begin, EditGeometryOptions{} )
        );
        const auto resized = interaction.update( corner.end );
        ASSERT_TRUE( resized.has_value() );
        expect_rect( *resized,
                     corner.expected_x,
                     corner.expected_y,
                     corner.expected_width,
                     corner.expected_height );
    }

    const std::array ellipse_shapes{ record( filled_ellipse_for_resize(), firstSlot ) };
    const std::array ellipse_editable{ ellipse_shapes.front().id };
    EditInteraction  ellipse_interaction;
    ASSERT_TRUE( ellipse_interaction.begin( ellipse_shapes,
                                            ellipse_editable,
                                            point( rectRight, rectBottom ),
                                            EditGeometryOptions{} ) );
    const auto resized_ellipse =
        ellipse_interaction.update( point( extendedRight, extendedBottom ) );
    ASSERT_TRUE( resized_ellipse.has_value() );
    const auto& ellipse = std::get<grab::overlay::Ellipse>( resized_ellipse->geometry );
    EXPECT_DOUBLE_EQ( ellipse.center.x, resizedEllipseCenterX );
    EXPECT_DOUBLE_EQ( ellipse.center.y, resizedEllipseCenterY );
    EXPECT_DOUBLE_EQ( ellipse.radius_x, resizedEllipseRadiusX );
    EXPECT_DOUBLE_EQ( ellipse.radius_y, resizedEllipseRadiusY );

    const std::array polygon_shapes{ record( filled_polygon_for_resize(), firstSlot ) };
    const std::array polygon_editable{ polygon_shapes.front().id };
    EditInteraction  polygon_interaction;
    ASSERT_TRUE( polygon_interaction.begin( polygon_shapes,
                                            polygon_editable,
                                            point( rectRight, rectBottom ),
                                            EditGeometryOptions{} ) );
    const auto resized_polygon =
        polygon_interaction.update( point( extendedRight, extendedBottom ) );
    ASSERT_TRUE( resized_polygon.has_value() );
    const auto& polygon = std::get<grab::overlay::Polygon>( resized_polygon->geometry );
    ASSERT_FALSE( polygon.points.empty() );
    EXPECT_DOUBLE_EQ( polygon.points.front().x, rectLeft );
    EXPECT_DOUBLE_EQ( polygon.points.front().y, rectTop );
    const auto polygon_horizontal = horizontal_extent( polygon.points );
    const auto polygon_vertical   = vertical_extent( polygon.points );
    EXPECT_DOUBLE_EQ( polygon_horizontal.second, extendedRight );
    EXPECT_DOUBLE_EQ( polygon_vertical.second, extendedBottom );

    const std::array path_shapes{ record( rectangle_path( true ), firstSlot ) };
    const std::array path_editable{ path_shapes.front().id };
    EditInteraction  path_interaction;
    ASSERT_TRUE( path_interaction.begin( path_shapes,
                                         path_editable,
                                         point( pathRight, pathBottom ),
                                         EditGeometryOptions{} ) );
    const auto resized_path =
        path_interaction.update( point( resizedPathRight, resizedPathBottom ) );
    ASSERT_TRUE( resized_path.has_value() );
    const auto resized_path_points = path_points( *resized_path );
    const auto path_horizontal     = horizontal_extent( resized_path_points );
    const auto path_vertical       = vertical_extent( resized_path_points );
    EXPECT_DOUBLE_EQ( path_horizontal.first, pathLeft );
    EXPECT_DOUBLE_EQ( path_horizontal.second, resizedPathRight );
    EXPECT_DOUBLE_EQ( path_vertical.first, pathTop );
    EXPECT_DOUBLE_EQ( path_vertical.second, resizedPathBottom );
}

TEST( OverlayEdit,
      ResizePastAnchorClampsAndNeverInverts )
{
    const auto clamp_shape =
        record( filled_rect( rectLeft, rectTop, rectWidth, rectHeight ), firstSlot );
    const std::array shapes{ clamp_shape };
    const std::array editable{ shapes.front().id };

    EditInteraction  bottom_right;
    ASSERT_TRUE( bottom_right.begin( shapes,
                                     editable,
                                     point( rectRight, rectBottom ),
                                     EditGeometryOptions{} ) );
    const auto clamped_bottom_right =
        bottom_right.update( point( pastAnchor, pastAnchor ) );
    ASSERT_TRUE( clamped_bottom_right.has_value() );
    expect_rect( *clamped_bottom_right, rectLeft, rectTop, min_size_px, min_size_px );

    EditInteraction top_left;
    ASSERT_TRUE( top_left.begin( shapes,
                                 editable,
                                 point( rectLeft, rectTop ),
                                 EditGeometryOptions{} ) );
    const auto clamped_top_left = top_left.update( point( beyondAnchor, beyondAnchor ) );
    ASSERT_TRUE( clamped_top_left.has_value() );
    expect_rect( *clamped_top_left,
                 rectRight - min_size_px,
                 rectBottom - min_size_px,
                 min_size_px,
                 min_size_px );
}

TEST( OverlayEdit,
      CancelDiscardsPreviewAndRestoresOriginalGeometry )
{
    const auto original_shape =
        record( filled_rect( rectLeft, rectTop, rectWidth, rectHeight ), firstSlot );
    const std::array shapes{ original_shape };
    const std::array editable{ shapes.front().id };
    EditInteraction  interaction;
    ASSERT_TRUE( interaction.begin( shapes,
                                    editable,
                                    point( rectCenterX, rectCenterY ),
                                    EditGeometryOptions{} ) );
    const auto preview =
        interaction.update( point( rectCenterX + dragX, rectCenterY + dragY ) );
    ASSERT_TRUE( preview.has_value() );
    expect_rect( *preview, rectLeft + dragX, rectTop + dragY, rectWidth, rectHeight );

    interaction.cancel();

    EXPECT_FALSE( interaction.active() );
    EXPECT_FALSE( interaction.update( point( rectCenterX, rectCenterY ) ).has_value() );
    expect_rect( shapes.front().shape, rectLeft, rectTop, rectWidth, rectHeight );

    ASSERT_TRUE( interaction.begin( shapes,
                                    editable,
                                    point( rectCenterX, rectCenterY ),
                                    EditGeometryOptions{} ) );
    const auto restored = interaction.update( point( rectCenterX, rectCenterY ) );
    ASSERT_TRUE( restored.has_value() );
    expect_rect( *restored, rectLeft, rectTop, rectWidth, rectHeight );
}

TEST( OverlayEdit,
      BeginRejectsAnimatedAndNonEditableShapes )
{
    auto animated = filled_rect( rectLeft, rectTop, rectWidth, rectHeight );
    animated.animation.emplace();
    const std::array animated_shapes{ record( std::move( animated ), firstSlot ) };
    const std::array animated_editable{ animated_shapes.front().id };
    EditInteraction  interaction;

    EXPECT_FALSE( interaction.begin( animated_shapes,
                                     animated_editable,
                                     point( rectCenterX, rectCenterY ),
                                     EditGeometryOptions{} ) );
    EXPECT_FALSE( interaction.active() );

    const auto static_shape =
        record( filled_rect( rectLeft, rectTop, rectWidth, rectHeight ), secondSlot );
    const std::array                              static_shapes{ static_shape };
    const std::span<const grab::overlay::ShapeId> none_editable;
    EXPECT_FALSE( interaction.begin( static_shapes,
                                     none_editable,
                                     point( rectCenterX, rectCenterY ),
                                     EditGeometryOptions{} ) );
    EXPECT_FALSE( interaction.active() );
}

TEST( OverlayEdit,
      InputRegionUsesPaintedCoverageAndCapsRectangleCount )
{
    std::vector<grab::overlay::ShapeRecord> shapes;
    std::vector<grab::overlay::ShapeId>     editable;
    auto                                    region_ellipse = grab::overlay::Shape{};
    region_ellipse.geometry                                = grab::overlay::Ellipse{
        .center   = point( regionEllipseCenter, regionEllipseCenter ),
        .radius_x = regionEllipseRadius,
        .radius_y = regionEllipseRadius,
    };
    region_ellipse.fill.emplace();
    shapes.push_back( record( std::move( region_ellipse ), firstSlot ) );
    editable.push_back( shapes.back().id );

    for( std::size_t index{}; index < capShapeCount; ++index )
    {
        const auto x =
            farShapeStart + ( static_cast<double>( index ) * farShapeSpacing );
        const auto slot = firstSlot + slotStep + static_cast<std::uint32_t>( index );
        shapes.push_back(
            record( filled_rect( x, farShapeTop, smallShapeExtent, smallShapeExtent ),
                    slot )
        );
        editable.push_back( shapes.back().id );
    }

    const auto region = edit_input_region( shapes, editable, EditGeometryOptions{} );

    EXPECT_EQ( region.size(), max_region_rects );
    EXPECT_FALSE( region_contains( region,
                                   grab::geometry::Point{
                                       .x = transparentRegionX,
                                       .y = transparentRegionY,
                                   } ) );
}
