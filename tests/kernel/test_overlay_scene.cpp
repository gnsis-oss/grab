#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_scene.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr std::chrono::milliseconds addedAt{ 125 };
    constexpr std::chrono::milliseconds duplicateUpdateAt{ 175 };
    constexpr std::chrono::milliseconds ttlDuration{ 50 };
    constexpr std::chrono::milliseconds pastDeadlineOffset{ 1 };
    constexpr std::chrono::milliseconds sameLifetimeUpdateAt{ 150 };
    constexpr std::chrono::milliseconds newLifetimeUpdateAt{ 175 };
    constexpr std::chrono::milliseconds initialTtlDuration{ 200 };
    constexpr std::chrono::milliseconds changedTtlDuration{ 300 };
    constexpr std::chrono::milliseconds shortenedTtlDuration{ 10 };
    constexpr std::chrono::milliseconds zeroTtlDuration{};
    constexpr std::chrono::milliseconds fadeDuration{ 50 };
    constexpr std::uint64_t             firstRevisionValue  = 1U;
    constexpr std::uint64_t             secondRevisionValue = 2U;
    constexpr std::uint64_t             thirdRevisionValue  = 3U;
    constexpr std::uint64_t             fourthRevisionValue = 4U;
    constexpr std::uint64_t             revisionIncrement   = 1U;
    constexpr std::uint64_t             epochIncrement      = 1U;
    constexpr std::size_t               singleShapeCount    = 1U;
    constexpr std::size_t               duplicateDeltaCount = 2U;
    constexpr std::size_t               threeDeltaCount     = 3U;
    constexpr std::size_t               fourDeltaCount      = 4U;
    constexpr std::size_t               orderedShapeCount   = 4U;
    constexpr std::size_t               secondDeltaIndex    = 1U;
    constexpr std::size_t               thirdDeltaIndex     = 2U;
    constexpr std::size_t               firstOrderedIndex   = 0U;
    constexpr std::size_t               secondOrderedIndex  = 1U;
    constexpr std::size_t               thirdOrderedIndex   = 2U;
    constexpr std::size_t               fourthOrderedIndex  = 3U;
    constexpr double                    rectangleX          = 10.0;
    constexpr double                    updatedRectangleX   = 15.0;
    constexpr double                    rectangleY          = 20.0;
    constexpr double                    rectangleWidth      = 30.0;
    constexpr double                    rectangleHeight     = 40.0;
    constexpr double                    ellipseRadiusY      = 18.0;
    constexpr double                    negativeRadius      = -1.0;
    constexpr double       notANumber       = std::numeric_limits<double>::quiet_NaN();
    constexpr double       positiveInfinity = std::numeric_limits<double>::infinity();
    constexpr float        strokeWidth      = 3.0F;
    constexpr std::int32_t updatedZ         = 7;
    constexpr std::int32_t lowerZ           = -5;
    constexpr std::int32_t higherZ          = 6;
    constexpr std::int32_t trailZ           = -20;
    constexpr grab::CoordinateSpaceId coordinateSpace{ 4U };

    [[nodiscard]]
    grab::overlay::Shape
    rect_shape()
    {
        grab::overlay::Rect rect;
        rect.bounds.x     = rectangleX;
        rect.bounds.y     = rectangleY;
        rect.bounds.w     = rectangleWidth;
        rect.bounds.h     = rectangleHeight;
        rect.bounds.space = coordinateSpace;

        grab::overlay::Shape shape;
        shape.geometry = rect;
        shape.stroke.emplace();
        shape.stroke->width_px = strokeWidth;
        shape.fill.emplace();
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    ordered_shape( grab::overlay::Band band,
                   std::int32_t        z )
    {
        auto shape = rect_shape();
        shape.band = band;
        shape.z    = z;
        return shape;
    }

    [[nodiscard]]
    bool
    colors_equal( const grab::overlay::Color& left,
                  const grab::overlay::Color& right ) noexcept
    {
        return left.r ==
               right.r &&
               left.g ==
               right.g &&
               left.b ==
               right.b &&
               left.a == right.a;
    }

    [[nodiscard]]
    bool
    shapes_equal( const grab::overlay::Shape& left,
                  const grab::overlay::Shape& right ) noexcept
    {
        const auto* left_rect  = std::get_if<grab::overlay::Rect>( &left.geometry );
        const auto* right_rect = std::get_if<grab::overlay::Rect>( &right.geometry );
        if( left_rect == nullptr || right_rect == nullptr )
        {
            return false;
        }
        const bool strokes_equal =
            ( !left.stroke.has_value() && !right.stroke.has_value() ) ||
            ( left.stroke.has_value() &&
              right.stroke.has_value() &&
              left.stroke->width_px ==
              right.stroke->width_px &&
              colors_equal( left.stroke->color, right.stroke->color ) );
        const bool fills_equal = ( !left.fill.has_value() && !right.fill.has_value() ) ||
                                 ( left.fill.has_value() &&
                                   right.fill.has_value() &&
                                   colors_equal( left.fill->color, right.fill->color ) );
        return left_rect->bounds.x ==
               right_rect->bounds.x &&
               left_rect->bounds.y ==
               right_rect->bounds.y &&
               left_rect->bounds.w ==
               right_rect->bounds.w &&
               left_rect->bounds.h ==
               right_rect->bounds.h &&
               left_rect->bounds.space ==
               right_rect->bounds.space &&
               strokes_equal &&
               fills_equal &&
               left.lifetime.index() ==
               right.lifetime.index() &&
               left.band ==
               right.band &&
               left.z == right.z;
    }

    [[nodiscard]]
    bool
    records_equal( const grab::overlay::ShapeRecord& left,
                   const grab::overlay::ShapeRecord& right ) noexcept
    {
        return left.id ==
               right.id &&
               left.started_at ==
               right.started_at &&
               shapes_equal( left.shape, right.shape );
    }

    struct ReplayedScene
    {
            grab::overlay::SceneEpoch               epoch{};
            grab::overlay::Revision                 through_revision{};
            std::vector<grab::overlay::ShapeRecord> records;
    };

    void
    replay( ReplayedScene&                   replayed,
            const grab::overlay::SceneDelta& delta )
    {
        replayed.epoch            = delta.epoch;
        replayed.through_revision = delta.revision;
        if( const auto* upsert = std::get_if<grab::overlay::Upsert>( &delta.change ) )
        {
            const auto existing = std::ranges::find_if(
                replayed.records,
                [&upsert]( const grab::overlay::ShapeRecord& record )
                {
                    return record.id == upsert->record.id;
                }
            );
            if( existing == replayed.records.end() )
            {
                replayed.records.push_back( upsert->record );
            }
            else
            {
                *existing = upsert->record;
            }
            return;
        }
        if( const auto* remove = std::get_if<grab::overlay::Remove>( &delta.change ) )
        {
            std::erase_if( replayed.records,
                           [&remove]( const grab::overlay::ShapeRecord& record )
                           {
                               return record.id == remove->id;
                           } );
            return;
        }

        replayed.records.clear();
        replayed.epoch = std::get<grab::overlay::Clear>( delta.change ).new_epoch;
    }

    [[nodiscard]]
    bool
    replay_equals( const ReplayedScene&                replayed,
                   const grab::overlay::SceneSnapshot& snapshot ) noexcept
    {
        return replayed.epoch ==
               snapshot.epoch &&
               replayed.through_revision ==
               snapshot.through_revision &&
               replayed.records.size() ==
               snapshot.shapes.size() &&
               std::ranges::all_of(
                   snapshot.shapes,
                   [&replayed]( const grab::overlay::ShapeRecord& expected )
                   {
                       const auto actual = std::ranges::find_if(
                           replayed.records,
                           [&expected]( const grab::overlay::ShapeRecord& record )
                           {
                               return record.id == expected.id;
                           }
                       );
                       return actual !=
                              replayed.records.end() &&
                              records_equal( *actual, expected );
                   }
               );
    }

    [[nodiscard]]
    bool
    revisions_are_contiguous( const std::vector<grab::overlay::SceneDelta>& deltas,
                              std::uint64_t first_revision ) noexcept
    {
        if( deltas.empty() || deltas.front().revision.value != first_revision )
        {
            return false;
        }
        for( auto current = std::next( deltas.begin() ); current != deltas.end();
             ++current )
        {
            const auto previous = std::prev( current );
            if( current->revision.value != previous->revision.value + revisionIncrement )
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]]
    bool
    rejects_invalid_shape( grab::kernel::presentation::OverlayScene&     scene,
                           grab::overlay::ShapeId                        valid_id,
                           const grab::overlay::Shape&                   invalid,
                           const std::vector<grab::overlay::SceneDelta>& deltas )
    {
        const auto add_result    = scene.add( invalid );
        const auto update_result = scene.update( valid_id, invalid );
        return !add_result.has_value() &&
               !update_result.has_value() &&
               add_result.error().code ==
               grab::ErrorCode::InvalidArgument &&
               update_result.error().code ==
               grab::ErrorCode::InvalidArgument &&
               deltas.empty();
    }

    [[nodiscard]]
    std::vector<grab::overlay::Shape>
    invalid_shapes()
    {
        auto not_a_number                                               = rect_shape();
        std::get<grab::overlay::Rect>( not_a_number.geometry ).bounds.x = notANumber;

        auto infinite_coordinate                                        = rect_shape();
        std::get<grab::overlay::Rect>( infinite_coordinate.geometry ).bounds.h =
            positiveInfinity;

        grab::SpacePoint path_point;
        path_point.x     = notANumber;
        path_point.y     = rectangleY;
        path_point.space = coordinateSpace;
        grab::overlay::Path path;
        path.commands.emplace_back( grab::overlay::MoveTo{ .point = path_point } );
        auto non_finite_path     = rect_shape();
        non_finite_path.geometry = std::move( path );

        grab::SpacePoint polygon_point;
        polygon_point.x     = rectangleX;
        polygon_point.y     = positiveInfinity;
        polygon_point.space = coordinateSpace;
        grab::overlay::Polygon polygon;
        polygon.points.push_back( polygon_point );
        auto non_finite_polygon     = rect_shape();
        non_finite_polygon.geometry = std::move( polygon );

        grab::overlay::Ellipse ellipse;
        ellipse.center.x                   = rectangleX;
        ellipse.center.y                   = rectangleY;
        ellipse.center.space               = coordinateSpace;
        ellipse.radius_x                   = rectangleWidth;
        ellipse.radius_y                   = ellipseRadiusY;
        auto non_finite_ellipse_center     = rect_shape();
        auto ellipse_with_bad_center       = ellipse;
        ellipse_with_bad_center.center.x   = notANumber;
        non_finite_ellipse_center.geometry = ellipse_with_bad_center;

        auto non_finite_ellipse_radius     = rect_shape();
        auto ellipse_with_bad_radius       = ellipse;
        ellipse_with_bad_radius.radius_y   = positiveInfinity;
        non_finite_ellipse_radius.geometry = ellipse_with_bad_radius;

        ellipse.radius_x                   = negativeRadius;
        auto negative_radius               = rect_shape();
        negative_radius.geometry           = ellipse;

        auto empty_path                    = rect_shape();
        empty_path.geometry                = grab::overlay::Path{};

        auto no_style                      = rect_shape();
        no_style.stroke                    = std::nullopt;
        no_style.fill                      = std::nullopt;

        return {
            std::move( not_a_number ),
            std::move( infinite_coordinate ),
            std::move( non_finite_path ),
            std::move( non_finite_polygon ),
            std::move( non_finite_ellipse_center ),
            std::move( non_finite_ellipse_radius ),
            std::move( negative_radius ),
            std::move( empty_path ),
            std::move( no_style ),
        };
    }

}    // namespace

TEST( OverlayScene,
      AddSnapshotRoundtripCarriesKernelStartTime )
{
    auto                                     now = addedAt;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    const auto                               shape = rect_shape();

    const auto                               added = scene.add( shape );

    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), singleShapeCount );
    EXPECT_EQ( snapshot.epoch, added->epoch );
    EXPECT_EQ( snapshot.through_revision.value, firstRevisionValue );
    EXPECT_EQ( snapshot.shapes.front().id, *added );
    EXPECT_EQ( snapshot.shapes.front().started_at, addedAt );
    EXPECT_TRUE( shapes_equal( snapshot.shapes.front().shape, shape ) );
}

TEST( OverlayScene,
      MutationRevisionsIncreaseAndDeltaReplayEqualsSnapshot )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );

    const auto first  = scene.add( rect_shape() );
    const auto second = scene.add( rect_shape() );
    ASSERT_TRUE( first.has_value() && second.has_value() );

    auto updated                                               = rect_shape();
    std::get<grab::overlay::Rect>( updated.geometry ).bounds.x = updatedRectangleX;
    updated.z                                                  = updatedZ;
    const auto update_result = scene.update( *first, updated );
    const auto remove_result = scene.remove( *second );
    ASSERT_TRUE( update_result.has_value() && remove_result.has_value() );

    const auto third = scene.add( rect_shape() );
    ASSERT_TRUE( third.has_value() ) << third.error().message;
    const auto delta_count_before_stale_update = deltas.size();
    const auto stale_update                    = scene.update( *second, rect_shape() );
    ASSERT_FALSE( stale_update.has_value() );
    EXPECT_TRUE( third->slot !=
                 second->slot &&
                 stale_update.error().code ==
                 grab::ErrorCode::StaleShape &&
                 deltas.size() == delta_count_before_stale_update );
    EXPECT_TRUE( revisions_are_contiguous( deltas, firstRevisionValue ) );

    ReplayedScene replayed;
    for( const auto& delta : deltas )
    {
        replay( replayed, delta );
    }
    EXPECT_TRUE( replay_equals( replayed, scene.snapshot() ) );
}

TEST( OverlayScene,
      ClearBumpsEpochEmitsClearAndStalesOldIds )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    const auto old_id = scene.add( rect_shape() );
    ASSERT_TRUE( old_id.has_value() ) << old_id.error().message;

    scene.clear();

    const auto snapshot = scene.snapshot();
    EXPECT_TRUE( snapshot.shapes.empty() &&
                 snapshot.epoch.value ==
                 old_id->epoch.value +
                 epochIncrement &&
                 snapshot.through_revision.value == firstRevisionValue );
    ASSERT_FALSE( deltas.empty() );
    const auto& clear_delta = deltas.back();
    const auto* clear       = std::get_if<grab::overlay::Clear>( &clear_delta.change );
    ASSERT_NE( clear, nullptr );
    EXPECT_TRUE( clear_delta.epoch ==
                 snapshot.epoch &&
                 clear_delta.revision.value ==
                 firstRevisionValue &&
                 clear->new_epoch == snapshot.epoch );

    const auto delta_count_before_stale_update = deltas.size();
    const auto stale_update                    = scene.update( *old_id, rect_shape() );
    ASSERT_FALSE( stale_update.has_value() );
    EXPECT_TRUE( stale_update.error().code ==
                 grab::ErrorCode::StaleShape &&
                 deltas.size() == delta_count_before_stale_update );
}

TEST( OverlayScene,
      DuplicateUpdateStillEmitsNewRevision )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    const auto shape = rect_shape();
    const auto id    = scene.add( shape );
    ASSERT_TRUE( id.has_value() ) << id.error().message;

    now                = duplicateUpdateAt;
    const auto updated = scene.update( *id, shape );

    ASSERT_TRUE( updated.has_value() ) << updated.error().message;
    ASSERT_EQ( deltas.size(), duplicateDeltaCount );
    EXPECT_EQ( deltas.front().revision.value, firstRevisionValue );
    EXPECT_EQ( deltas.back().revision.value, secondRevisionValue );
    const auto* duplicate = std::get_if<grab::overlay::Upsert>( &deltas.back().change );
    ASSERT_NE( duplicate, nullptr );
    EXPECT_EQ( duplicate->record.id, *id );
    EXPECT_EQ( duplicate->record.started_at, addedAt );
    EXPECT_TRUE( shapes_equal( duplicate->record.shape, shape ) );
}

TEST( OverlayScene,
      SnapshotDrainsExpiredTtlAndStalesItsId )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    auto expiring     = rect_shape();
    expiring.lifetime = grab::overlay::Ttl{ .duration = ttlDuration };
    const auto id     = scene.add( expiring );
    ASSERT_TRUE( id.has_value() ) << id.error().message;

    now                 = addedAt + ttlDuration + pastDeadlineOffset;
    const auto snapshot = scene.snapshot();

    EXPECT_TRUE( snapshot.shapes.empty() );
    EXPECT_EQ( snapshot.through_revision.value, secondRevisionValue );
    ASSERT_EQ( deltas.size(), duplicateDeltaCount );
    const auto* remove = std::get_if<grab::overlay::Remove>( &deltas.back().change );
    ASSERT_NE( remove, nullptr );
    EXPECT_EQ( remove->id, *id );
    EXPECT_EQ( deltas.back().revision.value, secondRevisionValue );

    const auto delta_count_before_stale_update = deltas.size();
    const auto stale_update                    = scene.update( *id, expiring );
    ASSERT_FALSE( stale_update.has_value() );
    EXPECT_EQ( stale_update.error().code, grab::ErrorCode::StaleShape );
    EXPECT_EQ( deltas.size(), delta_count_before_stale_update );
}

TEST( OverlayScene,
      InvalidGeometryIsRejectedByAddAndUpdateWithoutDelta )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    const auto valid_id = scene.add( rect_shape() );
    ASSERT_TRUE( valid_id.has_value() ) << valid_id.error().message;
    deltas.clear();

    std::size_t invalid_index{};
    for( const auto& invalid : invalid_shapes() )
    {
        SCOPED_TRACE( invalid_index );
        EXPECT_TRUE( rejects_invalid_shape( scene, *valid_id, invalid, deltas ) );
        ++invalid_index;
    }

    const auto snapshot = scene.snapshot();
    ASSERT_EQ( snapshot.shapes.size(), singleShapeCount );
    EXPECT_EQ( snapshot.through_revision.value, firstRevisionValue );
    EXPECT_TRUE( shapes_equal( snapshot.shapes.front().shape, rect_shape() ) );
}

TEST( OverlayScene,
      SnapshotOrdersByBandZAndInsertion )
{
    auto                                     now = addedAt;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    const auto trail = scene.add( ordered_shape( grab::overlay::Band::Trail, trailZ ) );
    const auto annotation_high =
        scene.add( ordered_shape( grab::overlay::Band::Annotation, higherZ ) );
    const auto annotation_first =
        scene.add( ordered_shape( grab::overlay::Band::Annotation, lowerZ ) );
    const auto annotation_second =
        scene.add( ordered_shape( grab::overlay::Band::Annotation, lowerZ ) );
    ASSERT_TRUE( trail.has_value() &&
                 annotation_high.has_value() &&
                 annotation_first.has_value() &&
                 annotation_second.has_value() );

    const auto snapshot = scene.snapshot();

    ASSERT_EQ( snapshot.shapes.size(), orderedShapeCount );
    EXPECT_TRUE( snapshot.shapes.at( firstOrderedIndex ).id ==
                 *annotation_first &&
                 snapshot.shapes.at( secondOrderedIndex ).id ==
                 *annotation_second &&
                 snapshot.shapes.at( thirdOrderedIndex ).id ==
                 *annotation_high &&
                 snapshot.shapes.at( fourthOrderedIndex ).id == *trail );
}

TEST( OverlayScene,
      LifetimeAlternativeAloneRestartsStartTimeAndFadeExpires )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    auto ttl      = rect_shape();
    ttl.lifetime  = grab::overlay::Ttl{ .duration = initialTtlDuration };
    const auto id = scene.add( ttl );
    ASSERT_TRUE( id.has_value() );

    now                                                   = sameLifetimeUpdateAt;
    std::get<grab::overlay::Ttl>( ttl.lifetime ).duration = changedTtlDuration;
    const auto same_alternative                           = scene.update( *id, ttl );
    now                                                   = newLifetimeUpdateAt;
    auto fade                                             = ttl;
    fade.lifetime              = grab::overlay::Fade{ .duration = fadeDuration };
    const auto new_alternative = scene.update( *id, fade );
    ASSERT_TRUE( same_alternative.has_value() && new_alternative.has_value() );
    ASSERT_EQ( deltas.size(), threeDeltaCount );
    const auto* same_upsert =
        std::get_if<grab::overlay::Upsert>( &deltas.at( secondDeltaIndex ).change );
    const auto* new_upsert =
        std::get_if<grab::overlay::Upsert>( &deltas.at( thirdDeltaIndex ).change );
    ASSERT_TRUE( same_upsert != nullptr && new_upsert != nullptr );
    EXPECT_TRUE( same_upsert->record.started_at ==
                 addedAt &&
                 new_upsert->record.started_at == newLifetimeUpdateAt );

    now                = newLifetimeUpdateAt + fadeDuration + pastDeadlineOffset;
    const auto expired = scene.snapshot();
    EXPECT_TRUE( expired.shapes.empty() &&
                 expired.through_revision.value ==
                 fourthRevisionValue &&
                 deltas.size() ==
                 fourDeltaCount &&
                 std::holds_alternative<grab::overlay::Remove>( deltas.back().change ) );
}

TEST( OverlayScene,
      AddDrainsDeadlineDueAtMutationTime )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    auto immediate     = rect_shape();
    immediate.lifetime = grab::overlay::Ttl{ .duration = zeroTtlDuration };

    const auto added   = scene.add( immediate );

    ASSERT_TRUE( added.has_value() ) << added.error().message;
    ASSERT_EQ( deltas.size(), duplicateDeltaCount );
    EXPECT_TRUE( revisions_are_contiguous( deltas, firstRevisionValue ) );
    const auto* upsert = std::get_if<grab::overlay::Upsert>( &deltas.front().change );
    const auto* remove = std::get_if<grab::overlay::Remove>( &deltas.back().change );
    ASSERT_TRUE( upsert != nullptr && remove != nullptr );
    EXPECT_TRUE( upsert->record.id == *added && remove->id == *added );
    const auto snapshot = scene.snapshot();
    EXPECT_TRUE( snapshot.shapes.empty() &&
                 snapshot.through_revision.value == secondRevisionValue );
}

TEST( OverlayScene,
      SameLifetimeShorteningDrainsNewlyExpiredShape )
{
    auto                                     now = addedAt;
    std::vector<grab::overlay::SceneDelta>   deltas;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    scene.set_delta_sink(
        [&deltas]( const grab::overlay::SceneDelta& delta )
        {
            deltas.push_back( delta );
        }
    );
    auto ttl      = rect_shape();
    ttl.lifetime  = grab::overlay::Ttl{ .duration = initialTtlDuration };
    const auto id = scene.add( ttl );
    ASSERT_TRUE( id.has_value() ) << id.error().message;

    now                                                   = sameLifetimeUpdateAt;
    std::get<grab::overlay::Ttl>( ttl.lifetime ).duration = shortenedTtlDuration;
    const auto updated                                    = scene.update( *id, ttl );

    ASSERT_TRUE( updated.has_value() ) << updated.error().message;
    ASSERT_EQ( deltas.size(), threeDeltaCount );
    const auto* upsert =
        std::get_if<grab::overlay::Upsert>( &deltas.at( secondDeltaIndex ).change );
    const auto* remove = std::get_if<grab::overlay::Remove>( &deltas.back().change );
    ASSERT_TRUE( upsert != nullptr && remove != nullptr );
    EXPECT_TRUE( upsert->record.started_at == addedAt && remove->id == *id );
    const auto snapshot = scene.snapshot();
    EXPECT_TRUE( snapshot.shapes.empty() &&
                 snapshot.through_revision.value == thirdRevisionValue );
}

TEST( OverlayScene,
      ExpiryBatchStaysOrderedWhenSinkReentersMutation )
{
    auto                                     now = addedAt;
    grab::kernel::presentation::OverlayScene scene{ [&now]
                                                    {
                                                        return now;
                                                    } };
    auto                                     expiring = rect_shape();
    expiring.lifetime = grab::overlay::Ttl{ .duration = ttlDuration };
    const auto first  = scene.add( expiring );
    const auto second = scene.add( expiring );
    ASSERT_TRUE( first.has_value() && second.has_value() );

    std::vector<grab::overlay::SceneDelta> published;
    bool                                   nested_add_succeeded{};
    bool                                   nested_add_attempted{};
    scene.set_delta_sink(
        [&scene, &published, &nested_add_succeeded, &nested_add_attempted](
            const grab::overlay::SceneDelta& delta
        )
        {
            published.push_back( delta );
            if( nested_add_attempted ||
                !std::holds_alternative<grab::overlay::Remove>( delta.change ) )
            {
                return;
            }
            nested_add_attempted = true;
            nested_add_succeeded = scene.add( rect_shape() ).has_value();
        }
    );

    now                          = addedAt + ttlDuration + pastDeadlineOffset;
    const auto before_nested_add = scene.snapshot();

    ASSERT_EQ( published.size(), threeDeltaCount );
    EXPECT_TRUE( before_nested_add.shapes.empty() &&
                 nested_add_succeeded &&
                 revisions_are_contiguous( published, thirdRevisionValue ) &&
                 std::holds_alternative<grab::overlay::Remove>(
                     published.at( firstOrderedIndex ).change
                 ) &&
                 std::holds_alternative<grab::overlay::Remove>(
                     published.at( secondOrderedIndex ).change
                 ) &&
                 std::holds_alternative<grab::overlay::Upsert>(
                     published.at( thirdOrderedIndex ).change
                 ) );
    EXPECT_EQ( scene.snapshot().shapes.size(), singleShapeCount );
}
