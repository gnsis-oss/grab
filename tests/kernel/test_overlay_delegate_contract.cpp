#include "fake/fake_overlay_delegate.hpp"
#include "grab/context.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "spi/overlay_delegate.hpp"
#include "spi/route.hpp"
#include "spi/runtime.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr std::chrono::milliseconds sceneTime{ 125 };
    constexpr grab::CoordinateSpaceId   delegateSpace{ 9U };
    constexpr double                    rectangleX          = 10.0;
    constexpr double                    updatedRectangleX   = 15.0;
    constexpr double                    rectangleY          = 20.0;
    constexpr double                    rectangleWidth      = 30.0;
    constexpr double                    rectangleHeight     = 40.0;
    constexpr float                     strokeWidth         = 3.0F;
    constexpr std::int32_t              defaultZ            = 0;
    constexpr std::int32_t              foregroundZ         = 7;
    constexpr std::uint32_t             initialGeneration   = 1U;
    constexpr std::uint64_t             secondRevisionValue = 2U;
    constexpr std::size_t               firstDeltaIndex     = 0U;
    constexpr std::size_t               secondDeltaIndex    = 1U;
    constexpr std::size_t               thirdDeltaIndex     = 2U;
    constexpr std::size_t               oneDeltaCount       = 1U;
    constexpr std::size_t               twoCloseCalls       = 2U;
    constexpr std::string_view injectedFailureMessage = "injected overlay apply failure";

    static_assert( std::derived_from<grab::testing::FakeOverlayDelegate,
                                     grab::spi::OverlayDelegate> );
    static_assert( !std::copy_constructible<grab::spi::OverlayDelegate> );
    static_assert( !std::move_constructible<grab::spi::OverlayDelegate> );

    [[nodiscard]]
    grab::overlay::Shape
    rect_shape( double              x    = rectangleX,
                grab::overlay::Band band = grab::overlay::Band::Annotation,
                std::int32_t        z    = defaultZ )
    {
        grab::overlay::Rect rect;
        rect.bounds.x     = x;
        rect.bounds.y     = rectangleY;
        rect.bounds.w     = rectangleWidth;
        rect.bounds.h     = rectangleHeight;
        rect.bounds.space = delegateSpace;

        grab::overlay::Shape shape;
        shape.geometry = rect;
        shape.stroke.emplace();
        shape.stroke->width_px = strokeWidth;
        shape.fill.emplace();
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

    [[nodiscard]]
    bool
    delegate_matches( const grab::testing::FakeOverlayDelegate& delegate,
                      const grab::overlay::SceneSnapshot&       snapshot )
    {
        return delegate.epoch() ==
               snapshot.epoch &&
               delegate.through_revision() ==
               snapshot.through_revision &&
               delegate.shapes().size() ==
               snapshot.shapes.size() &&
               std::ranges::all_of(
                   snapshot.shapes,
                   [&delegate]( const grab::overlay::ShapeRecord& expected )
                   {
                       const auto actual = delegate.shapes().find( expected.id );
                       return actual !=
                              delegate.shapes().end() &&
                              records_equal( actual->second, expected );
                   }
               );
    }

    [[nodiscard]]
    std::span<const grab::overlay::SceneDelta>
    one_delta( const std::vector<grab::overlay::SceneDelta>& deltas,
               std::size_t                                   index )
    {
        return std::span{ deltas }.subspan( index, oneDeltaCount );
    }

    class RuntimeWithoutOverlay final : public grab::spi::Runtime
    {
        public:

            [[nodiscard]]
            std::string_view
            name() const override
            {
                return "runtime-without-overlay";
            }

            [[nodiscard]]
            std::uint32_t
            generation() const override
            {
                return initialGeneration;
            }

            [[nodiscard]]
            grab::Result<void>
            start( [[maybe_unused]] const grab::OperationContext& context ) override
            {
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            stop() override
            {
                return {};
            }

            [[nodiscard]]
            std::span<const grab::spi::RouteDescriptor>
            routes() const override
            {
                return {};
            }
    };

    class OverlayDelegateContractTest : public ::testing::Test
    {
        protected:

            OverlayDelegateContractTest() :
                scene{ [this]
                       {
                           return now;
                       } }
            {
            }

            void
            SetUp() override
            {
                ASSERT_TRUE( delegate.open( delegateSpace ).has_value() );
                scene.set_delta_sink(
                    [this]( const grab::overlay::SceneDelta& delta )
                    {
                        deltas.push_back( delta );
                    }
                );
            }

            // NOLINTBEGIN(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
            std::chrono::milliseconds                now{ sceneTime };
            grab::kernel::presentation::OverlayScene scene;
            grab::testing::FakeOverlayDelegate       delegate;
            std::vector<grab::overlay::SceneDelta>   deltas;
            // NOLINTEND(misc-non-private-member-variables-in-classes,cppcoreguidelines-non-private-member-variables-in-classes)
    };

}    // namespace

TEST( OverlayDelegateRuntime,
      OptionalAccessorDefaultsToNull )
{
    RuntimeWithoutOverlay runtime;

    EXPECT_EQ( runtime.overlay_delegate(), nullptr );
}

TEST_F( OverlayDelegateContractTest,
        ContiguousSceneDeltasProduceSnapshotEquivalentShapeSet )
{
    const auto first  = scene.add( rect_shape() );
    const auto second = scene.add(
        rect_shape( updatedRectangleX, grab::overlay::Band::Trail, foregroundZ )
    );
    ASSERT_TRUE( first.has_value() ) << first.error().message;
    ASSERT_TRUE( second.has_value() ) << second.error().message;

    const auto updated = scene.update( *first, rect_shape( updatedRectangleX ) );
    const auto removed = scene.remove( *second );
    ASSERT_TRUE( updated.has_value() ) << updated.error().message;
    ASSERT_TRUE( removed.has_value() ) << removed.error().message;

    const auto applied = delegate.apply( deltas );

    ASSERT_TRUE( applied.has_value() ) << applied.error().message;
    EXPECT_TRUE( delegate.synced() );
    EXPECT_TRUE( delegate_matches( delegate, scene.snapshot() ) );
}

TEST_F( OverlayDelegateContractTest,
        MissingRevisionDesyncsUntilSnapshotThenStreamResumes )
{
    const auto first  = scene.add( rect_shape() );
    const auto second = scene.add( rect_shape( updatedRectangleX ) );
    const auto third =
        scene.add( rect_shape( rectangleX, grab::overlay::Band::Trail, foregroundZ ) );
    ASSERT_TRUE( first.has_value() && second.has_value() && third.has_value() );
    ASSERT_TRUE( delegate.apply( one_delta( deltas, firstDeltaIndex ) ).has_value() );

    const auto gap = delegate.apply( one_delta( deltas, thirdDeltaIndex ) );

    ASSERT_FALSE( gap.has_value() );
    EXPECT_EQ( gap.error().code, grab::ErrorCode::ResyncRequired );
    EXPECT_TRUE( delegate.desynced() );

    const auto snapshot = scene.snapshot();
    const auto resynced = delegate.resync( snapshot );
    ASSERT_TRUE( resynced.has_value() ) << resynced.error().message;
    EXPECT_TRUE( delegate_matches( delegate, snapshot ) );

    const auto update = scene.update(
        *first,
        rect_shape( rectangleX, grab::overlay::Band::Annotation, foregroundZ )
    );
    ASSERT_TRUE( update.has_value() ) << update.error().message;
    ASSERT_TRUE(
        delegate.apply( one_delta( deltas, deltas.size() - oneDeltaCount ) ).has_value()
    );
    EXPECT_TRUE( delegate_matches( delegate, scene.snapshot() ) );
}

TEST_F( OverlayDelegateContractTest,
        ReplayingOlderUpsertIsRecordedButDoesNotRollBackState )
{
    const auto id = scene.add( rect_shape() );
    ASSERT_TRUE( id.has_value() ) << id.error().message;
    const auto updated = scene.update( *id, rect_shape( updatedRectangleX ) );
    ASSERT_TRUE( updated.has_value() ) << updated.error().message;
    ASSERT_TRUE( delegate.apply( deltas ).has_value() );

    const auto replay = delegate.apply( one_delta( deltas, firstDeltaIndex ) );

    ASSERT_TRUE( replay.has_value() ) << replay.error().message;
    EXPECT_EQ( delegate.through_revision().value, secondRevisionValue );
    EXPECT_TRUE( delegate_matches( delegate, scene.snapshot() ) );
}

TEST_F( OverlayDelegateContractTest,
        InjectedApplyFailureRejectsDeltasUntilAtomicResync )
{
    const auto id = scene.add( rect_shape() );
    ASSERT_TRUE( id.has_value() ) << id.error().message;
    delegate.fail_next_apply( grab::ErrorCode::ProviderFailed,
                              std::string{ injectedFailureMessage } );

    const auto injected = delegate.apply( deltas );
    ASSERT_FALSE( injected.has_value() );
    EXPECT_EQ( injected.error().code, grab::ErrorCode::ProviderFailed );
    EXPECT_TRUE( delegate.desynced() );

    const auto rejected = delegate.apply( deltas );
    ASSERT_FALSE( rejected.has_value() );
    EXPECT_EQ( rejected.error().code, grab::ErrorCode::ResyncRequired );
    EXPECT_TRUE( delegate.shapes().empty() );

    const auto resynced = delegate.resync( scene.snapshot() );
    ASSERT_TRUE( resynced.has_value() ) << resynced.error().message;
    const auto updated = scene.update( *id, rect_shape( updatedRectangleX ) );
    ASSERT_TRUE( updated.has_value() ) << updated.error().message;
    ASSERT_TRUE( delegate.apply( one_delta( deltas, secondDeltaIndex ) ).has_value() );
    EXPECT_TRUE( delegate_matches( delegate, scene.snapshot() ) );
}

TEST_F( OverlayDelegateContractTest,
        ApplyDeepCopiesCallerOwnedSpan )
{
    const auto id = scene.add( rect_shape() );
    ASSERT_TRUE( id.has_value() ) << id.error().message;
    ASSERT_TRUE( delegate.apply( deltas ).has_value() );

    auto* const caller_upsert =
        std::get_if<grab::overlay::Upsert>( &deltas.front().change );
    ASSERT_NE( caller_upsert, nullptr );
    std::get<grab::overlay::Rect>( caller_upsert->record.shape.geometry ).bounds.x =
        updatedRectangleX;
    deltas.clear();

    const auto* const recorded =
        std::get_if<grab::testing::OverlayApplyCall>( &delegate.calls().back() );
    ASSERT_NE( recorded, nullptr );
    ASSERT_EQ( recorded->deltas.size(), oneDeltaCount );
    const auto* const recorded_upsert =
        std::get_if<grab::overlay::Upsert>( &recorded->deltas.front().change );
    ASSERT_NE( recorded_upsert, nullptr );
    const auto& recorded_rect =
        std::get<grab::overlay::Rect>( recorded_upsert->record.shape.geometry );
    EXPECT_EQ( recorded_rect.bounds.x, rectangleX );
    EXPECT_EQ( delegate.shapes().at( *id ).shape.geometry.index(),
               recorded_upsert->record.shape.geometry.index() );
}

TEST_F( OverlayDelegateContractTest,
        CloseIsIdempotentAndEveryCloseIsRecorded )
{
    delegate.close();
    delegate.close();

    EXPECT_TRUE( delegate.closed() );
    const auto close_calls = std::ranges::count_if(
        delegate.calls(),
        []( const grab::testing::OverlayCall& call )
        {
            return std::holds_alternative<grab::testing::OverlayCloseCall>( call );
        }
    );
    EXPECT_EQ( close_calls, twoCloseCalls );
}
