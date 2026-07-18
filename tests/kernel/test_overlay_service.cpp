#include "fake/fake_overlay_delegate.hpp"
#include "fake/fake_runtime.hpp"
#include "grab/capability.hpp"
#include "grab/context.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_service.hpp"
#include "kernel/presentation/space_graph.hpp"
#include "kernel/tree_fixtures.hpp"
#include "spi/overlay_delegate.hpp"
#include "spi/route.hpp"
#include "spi/runtime.hpp"
#include "spi/tree_source.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr std::chrono::milliseconds sceneNow{ 125 };
    constexpr std::chrono::seconds      reactorDrainTimeout{ 2 };
    constexpr std::uint32_t             topologyGeneration       = 1U;
    constexpr std::uint64_t             mappingId                = 1U;
    constexpr std::uint64_t             initialTreeRevision      = 1U;
    constexpr std::uint64_t             firstSceneRevision       = 1U;
    constexpr std::uint64_t             secondSceneRevision      = 2U;
    constexpr std::uint64_t             fixtureRootNodeValue     = 1U;
    constexpr std::size_t               openCallIndex            = 0U;
    constexpr std::size_t               failedApplyCallIndex     = 1U;
    constexpr std::size_t               resyncCallIndex          = 2U;
    constexpr std::size_t               recoveredApplyIndex      = 3U;
    constexpr std::size_t               singleCallCount          = 1U;
    constexpr std::size_t               singleShapeCount         = 1U;
    constexpr std::size_t               expectedRecoveryCalls    = 4U;
    constexpr std::size_t               thrownRecoveryCalls      = 3U;
    constexpr std::size_t               thrownResyncCallIndex    = 1U;
    constexpr std::size_t               thrownApplyCallIndex     = 2U;
    constexpr std::size_t               loweredShapeCount        = 3U;
    constexpr std::size_t               rectPathCommandCount     = 5U;
    constexpr std::size_t               ellipsePathCommandCount  = 6U;
    constexpr std::size_t               trianglePathCommandCount = 4U;
    constexpr std::size_t               sourcePathCommandCount   = 4U;
    constexpr std::size_t               cubicControlPointCount   = 3U;
    constexpr double                    sourceX                  = 10.0;
    constexpr double                    sourceY                  = 20.0;
    constexpr double                    sourceWidth              = 30.0;
    constexpr double                    sourceHeight             = 40.0;
    constexpr double                    sourceRadiusX            = 8.0;
    constexpr double                    sourceRadiusY            = 6.0;
    constexpr double                    axisScaleX               = 2.0;
    constexpr double                    axisScaleY               = 3.0;
    constexpr double                    axisTranslateX           = 5.0;
    constexpr double                    axisTranslateY           = -7.0;
    constexpr double                    expectedRectX            = 25.0;
    constexpr double                    expectedRectY            = 53.0;
    constexpr double                    expectedRectWidth        = 60.0;
    constexpr double                    expectedRectHeight       = 120.0;
    constexpr float                     sourceStrokeWidth        = 3.0F;
    constexpr double                    quarterTurnXx            = 0.0;
    constexpr double                    quarterTurnXy            = -1.0;
    constexpr double                    quarterTurnYx            = 1.0;
    constexpr double                    quarterTurnYy            = 0.0;
    constexpr double                    quarterTurnTranslateX    = 100.0;
    constexpr std::uint32_t             unknownSpaceValue        = 999U;
    constexpr std::string_view          runtimeName         = "overlay-test-runtime";
    constexpr std::string_view          flushFailureMessage = "injected flush failure";
    constexpr std::string_view applyExceptionMessage        = "injected apply exception";
    using DelegateThreadLog = std::vector<std::thread::id>;

    class ThreadRecordingOverlayDelegate final : public grab::spi::OverlayDelegate
    {
        public:

            ThreadRecordingOverlayDelegate() :
                ThreadRecordingOverlayDelegate( std::make_shared<DelegateThreadLog>() )
            {
            }

            explicit ThreadRecordingOverlayDelegate(
                std::shared_ptr<DelegateThreadLog> call_threads
            ) :
                call_threads_{ std::move( call_threads ) }
            {
            }

            [[nodiscard]]
            grab::Result<void>
            open( grab::CoordinateSpaceId space ) override
            {
                record_thread();
                return delegate_.open( space );
            }

            [[nodiscard]]
            grab::Result<void>
            apply( std::span<const grab::overlay::SceneDelta> deltas ) override
            {
                record_thread();
                if( throw_on_next_apply_ )
                {
                    throw_on_next_apply_ = false;
                    throw std::runtime_error{ std::string{ applyExceptionMessage } };
                }
                return delegate_.apply( deltas );
            }

            [[nodiscard]]
            grab::Result<void>
            resync( const grab::overlay::SceneSnapshot& scene ) override
            {
                record_thread();
                return delegate_.resync( scene );
            }

            [[nodiscard]]
            grab::Result<void>
            flush( grab::overlay::Revision through ) override
            {
                record_thread();
                if( flush_failure_.has_value() )
                {
                    auto error = std::move( *flush_failure_ );
                    flush_failure_.reset();
                    return std::unexpected( std::move( error ) );
                }
                return delegate_.flush( through );
            }

            void
            close() override
            {
                record_thread();
                delegate_.close();
            }

            [[nodiscard]]
            grab::testing::FakeOverlayDelegate&
            fake() noexcept
            {
                return delegate_;
            }

            [[nodiscard]]
            const DelegateThreadLog&
            call_threads() const noexcept
            {
                return *call_threads_;
            }

            void
            fail_next_flush( grab::ErrorCode code,
                             std::string     message )
            {
                flush_failure_ = grab::Error{
                    .code       = code,
                    .message    = std::move( message ),
                    .capability = {},
                    .target     = {},
                    .attempts   = {},
                };
            }

            void
            throw_next_apply() noexcept
            {
                throw_on_next_apply_ = true;
            }

        private:

            void
            record_thread()
            {
                call_threads_->push_back( std::this_thread::get_id() );
            }

            grab::testing::FakeOverlayDelegate delegate_;
            std::shared_ptr<DelegateThreadLog> call_threads_;
            std::optional<grab::Error>         flush_failure_;
            bool                               throw_on_next_apply_{};
    };

    class OverlayRuntime final : public grab::spi::Runtime
    {
        public:

            OverlayRuntime() :
                OverlayRuntime( std::make_shared<DelegateThreadLog>() )
            {
            }

            explicit OverlayRuntime( std::shared_ptr<DelegateThreadLog> call_threads ) :
                delegate_{ std::move( call_threads ) }
            {
                backing_.inject_snapshot( grab::testing::tree::snapshot(
                    initialTreeRevision,
                    {
                        grab::testing::tree::node( fixtureRootNodeValue,
                                                   grab::role::window ),
                    }
                ) );
            }

            [[nodiscard]]
            std::string_view
            name() const override
            {
                return runtimeName;
            }

            [[nodiscard]]
            std::uint32_t
            generation() const override
            {
                return backing_.generation();
            }

            [[nodiscard]]
            grab::Result<void>
            start( const grab::OperationContext& context ) override
            {
                return backing_.start( context );
            }

            [[nodiscard]]
            grab::Result<void>
            stop() override
            {
                return backing_.stop();
            }

            [[nodiscard]]
            grab::spi::TreeSource*
            tree_source() override
            {
                return backing_.tree_source();
            }

            [[nodiscard]]
            std::span<const grab::spi::RouteDescriptor>
            routes() const override
            {
                return backing_.routes();
            }

            [[nodiscard]]
            grab::spi::ActionRoute*
            action_route( std::size_t index ) override
            {
                return backing_.action_route( index );
            }

            [[nodiscard]]
            grab::spi::OverlayDelegate*
            overlay_delegate() override
            {
                return &delegate_;
            }

            [[nodiscard]]
            grab::testing::FakeOverlayDelegate&
            delegate() noexcept
            {
                return delegate_.fake();
            }

            [[nodiscard]]
            const DelegateThreadLog&
            delegate_call_threads() const noexcept
            {
                return delegate_.call_threads();
            }

            void
            fail_next_flush( grab::ErrorCode code,
                             std::string     message )
            {
                delegate_.fail_next_flush( code, std::move( message ) );
            }

            void
            throw_next_apply() noexcept
            {
                delegate_.throw_next_apply();
            }

        private:

            grab::testing::FakeRuntime     backing_;
            ThreadRecordingOverlayDelegate delegate_;
    };

    [[nodiscard]]
    grab::overlay::Shape
    rect_shape( grab::CoordinateSpaceId space )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{
            .bounds = {
                       .x     = sourceX,
                       .y     = sourceY,
                       .w     = sourceWidth,
                       .h     = sourceHeight,
                       .space = space,
                       },
        };
        shape.stroke.emplace();
        shape.stroke->width_px = sourceStrokeWidth;
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    ellipse_shape( grab::CoordinateSpaceId space )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Ellipse{
            .center =
                {
                         .x     = sourceX,
                         .y     = sourceY,
                         .space = space,
                         },
            .radius_x = sourceRadiusX,
            .radius_y = sourceRadiusY,
        };
        shape.fill.emplace();
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    triangle_shape( grab::CoordinateSpaceId space )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Polygon{
            .points = {
                       grab::SpacePoint{ .x = sourceX, .y = sourceY, .space = space },
                       grab::SpacePoint{
                    .x     = sourceX + sourceWidth,
                    .y     = sourceY,
                    .space = space,
                }, grab::SpacePoint{
                    .x     = sourceX,
                    .y     = sourceY + sourceHeight,
                    .space = space,
                }, },
        };
        shape.fill.emplace();
        return shape;
    }

    [[nodiscard]]
    grab::overlay::Shape
    path_shape( grab::CoordinateSpaceId space )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Path{
            .commands =
                {
                           grab::overlay::MoveTo{
                        .point = { .x = sourceX, .y = sourceY, .space = space },
                    }, grab::overlay::LineTo{
                        .point =
                            { .x = sourceX + sourceWidth, .y = sourceY, .space = space },
                    }, grab::overlay::BezierTo{
                        .control =
                            {
                                grab::SpacePoint{
                                    .x     = sourceX + sourceWidth,
                                    .y     = sourceY + sourceHeight,
                                    .space = space,
                                },
                                grab::SpacePoint{
                                    .x     = sourceX,
                                    .y     = sourceY + sourceHeight,
                                    .space = space,
                                },
                                grab::SpacePoint{
                                    .x     = sourceX,
                                    .y     = sourceY,
                                    .space = space,
                                },
                            },
                    }, grab::overlay::ClosePath{},
                           },
            .closed = true,
        };
        shape.stroke.emplace();
        return shape;
    }

    [[nodiscard]]
    bool
    path_uses_space( const grab::overlay::Path& path,
                     grab::CoordinateSpaceId    expected )
    {
        for( const auto& command : path.commands )
        {
            if( const auto* move = std::get_if<grab::overlay::MoveTo>( &command ) )
            {
                if( move->point.space != expected )
                {
                    return false;
                }
                continue;
            }
            if( const auto* line = std::get_if<grab::overlay::LineTo>( &command ) )
            {
                if( line->point.space != expected )
                {
                    return false;
                }
                continue;
            }
            if( const auto* bezier = std::get_if<grab::overlay::BezierTo>( &command ) )
            {
                if( bezier->control.size() != cubicControlPointCount )
                {
                    return false;
                }
                for( const auto& point : bezier->control )
                {
                    if( point.space != expected )
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    [[nodiscard]]
    bool
    transformed_rect_matches( const grab::overlay::Shape& shape,
                              grab::CoordinateSpaceId     expected_space )
    {
        const auto* rect = std::get_if<grab::overlay::Rect>( &shape.geometry );
        return rect !=
               nullptr &&
               rect->bounds.x ==
               expectedRectX &&
               rect->bounds.y ==
               expectedRectY &&
               rect->bounds.w ==
               expectedRectWidth &&
               rect->bounds.h ==
               expectedRectHeight &&
               rect->bounds.space ==
               expected_space &&
               shape.stroke.has_value() &&
               shape.stroke->width_px == sourceStrokeWidth;
    }

    [[nodiscard]]
    bool
    lowered_path_matches( const grab::overlay::Geometry& geometry,
                          std::size_t                    expected_command_count,
                          grab::CoordinateSpaceId        expected_space )
    {
        const auto* path = std::get_if<grab::overlay::Path>( &geometry );
        return path !=
               nullptr &&
               path->commands.size() ==
               expected_command_count &&
               path->closed &&
               path_uses_space( *path, expected_space );
    }

    [[nodiscard]]
    bool
    recovered_in_order( const grab::testing::FakeOverlayDelegate& delegate,
                        grab::overlay::ShapeId                    first,
                        grab::overlay::ShapeId                    second,
                        grab::CoordinateSpaceId                   expected_space )
    {
        const auto& calls = delegate.calls();
        if( calls.size() !=
            expectedRecoveryCalls ||
            !std::holds_alternative<grab::testing::OverlayApplyCall>(
                calls.at( failedApplyCallIndex )
            ) )
        {
            return false;
        }
        const auto* resync = std::get_if<grab::testing::OverlayResyncCall>(
            &calls.at( resyncCallIndex )
        );
        const auto* recovered_apply = std::get_if<grab::testing::OverlayApplyCall>(
            &calls.at( recoveredApplyIndex )
        );
        return resync !=
               nullptr &&
               resync->scene.shapes.size() ==
               singleShapeCount &&
               resync->scene.shapes.front().id ==
               first &&
               transformed_rect_matches( resync->scene.shapes.front().shape,
                                         expected_space ) &&
               resync->scene.through_revision.value ==
               firstSceneRevision &&
               recovered_apply !=
               nullptr &&
               recovered_apply->deltas.size() ==
               singleCallCount &&
               recovered_apply->deltas.front().revision.value ==
               secondSceneRevision &&
               delegate.shapes().contains( first ) &&
               delegate.shapes().contains( second );
    }

    [[nodiscard]]
    bool
    recovered_after_apply_exception( const grab::testing::FakeOverlayDelegate& delegate,
                                     grab::overlay::ShapeId                    first,
                                     grab::overlay::ShapeId                    second )
    {
        const auto& calls = delegate.calls();
        if( calls.size() != thrownRecoveryCalls )
        {
            return false;
        }
        const auto* resync = std::get_if<grab::testing::OverlayResyncCall>(
            &calls.at( thrownResyncCallIndex )
        );
        const auto* apply = std::get_if<grab::testing::OverlayApplyCall>(
            &calls.at( thrownApplyCallIndex )
        );
        return resync !=
               nullptr &&
               resync->scene.through_revision.value ==
               firstSceneRevision &&
               resync->scene.shapes.size() ==
               singleShapeCount &&
               resync->scene.shapes.front().id ==
               first &&
               apply !=
               nullptr &&
               apply->deltas.size() ==
               singleCallCount &&
               apply->deltas.front().revision.value ==
               secondSceneRevision &&
               delegate.shapes().contains( first ) &&
               delegate.shapes().contains( second );
    }

    [[nodiscard]]
    std::unique_ptr<grab::testing::FakeRuntime>
    runtime_without_overlay()
    {
        auto runtime = std::make_unique<grab::testing::FakeRuntime>();
        runtime->inject_snapshot( grab::testing::tree::snapshot(
            initialTreeRevision,
            { grab::testing::tree::node( fixtureRootNodeValue, grab::role::window ) }
        ) );
        return runtime;
    }

    [[nodiscard]]
    grab::Result<std::thread::id>
    reactor_thread_for( grab::Session& session )
    {
        auto completion = std::make_shared<std::promise<std::thread::id>>();
        auto completed  = completion->get_future();
        auto posted     = session.post(
            [completion]
            {
                completion->set_value( std::this_thread::get_id() );
            }
        );
        if( !posted.has_value() )
        {
            return std::unexpected( std::move( posted.error() ) );
        }
        return completed.get();
    }

    [[nodiscard]]
    bool
    every_call_uses_thread( const DelegateThreadLog& calls,
                            std::thread::id          expected )
    {
        return !calls.empty() &&
               std::ranges::all_of( calls,
                                    [expected]( std::thread::id actual )
                                    {
                                        return actual == expected;
                                    } );
    }

}    // namespace

TEST( OverlayService,
      SessionOverlayWithoutDelegateNamesRuntimeInCapabilityError )
{
    auto session = grab::Session::open_owning_runtime( runtime_without_overlay() );
    ASSERT_TRUE( session.has_value() ) << session.error().message;

    const auto overlay = ( *session )->overlay();

    ASSERT_FALSE( overlay.has_value() );
    EXPECT_EQ( overlay.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_EQ( overlay.error().capability, grab::capability::overlay );
    ASSERT_EQ( overlay.error().attempts.size(), singleCallCount );
    EXPECT_EQ( overlay.error().attempts.front().provider, "fake" );
    EXPECT_NE( overlay.error().attempts.front().reason.find( "overlay delegate" ),
               std::string::npos );
}

TEST( OverlayService,
      AddTransformsAxisAlignedRectBeforeDelegateApplyAndFlushesRevision )
{
    grab::detail::SpaceGraph graph;
    const auto               source_space   = graph.add_space( topologyGeneration );
    const auto               delegate_space = graph.add_space( topologyGeneration );
    graph.add_transform( grab::TransformRecord{
        .source      = source_space,
        .destination = delegate_space,
        .map =
            {
                  .xx = axisScaleX,
                  .tx = axisTranslateX,
                  .yy = axisScaleY,
                  .ty = axisTranslateY,
                  },
        .mapping_id = mappingId,
        .generation = topologyGeneration,
        .trust      = grab::TransformTrust::Exact,
    } );
    OverlayRuntime runtime;
    auto           service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;

    const auto added = ( *service )->add( rect_shape( source_space ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    ASSERT_TRUE( runtime.delegate().shapes().contains( *added ) );
    const auto& delivered = runtime.delegate().shapes().at( *added ).shape;
    EXPECT_TRUE( transformed_rect_matches( delivered, delegate_space ) );

    const auto flushed = ( *service )->flush();
    ASSERT_TRUE( flushed.has_value() ) << flushed.error().message;
    const auto* flush_call = std::get_if<grab::testing::OverlayFlushCall>(
        &runtime.delegate().calls().back()
    );
    ASSERT_NE( flush_call, nullptr );
    EXPECT_EQ( flush_call->through.value, firstSceneRevision );
}

TEST( OverlayService,
      RotationLowersRectEllipseAndPolygonToDelegateSpacePaths )
{
    grab::detail::SpaceGraph graph;
    const auto               source_space   = graph.add_space( topologyGeneration );
    const auto               delegate_space = graph.add_space( topologyGeneration );
    graph.add_transform( grab::TransformRecord{
        .source      = source_space,
        .destination = delegate_space,
        .map =
            {
                  .xx = quarterTurnXx,
                  .xy = quarterTurnXy,
                  .tx = quarterTurnTranslateX,
                  .yx = quarterTurnYx,
                  .yy = quarterTurnYy,
                  },
        .mapping_id = mappingId,
        .generation = topologyGeneration,
        .trust      = grab::TransformTrust::Exact,
    } );
    OverlayRuntime runtime;
    auto           service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;

    const auto rect_id     = ( *service )->add( rect_shape( source_space ) );
    const auto ellipse_id  = ( *service )->add( ellipse_shape( source_space ) );
    const auto triangle_id = ( *service )->add( triangle_shape( source_space ) );
    ASSERT_TRUE( rect_id.has_value() &&
                 ellipse_id.has_value() &&
                 triangle_id.has_value() );
    ASSERT_EQ( runtime.delegate().shapes().size(), loweredShapeCount );

    const auto& rect_geometry =
        runtime.delegate().shapes().at( *rect_id ).shape.geometry;
    const auto& ellipse_geometry =
        runtime.delegate().shapes().at( *ellipse_id ).shape.geometry;
    const auto& triangle_geometry =
        runtime.delegate().shapes().at( *triangle_id ).shape.geometry;
    EXPECT_TRUE(
        lowered_path_matches( rect_geometry, rectPathCommandCount, delegate_space )
    );
    EXPECT_TRUE(
        lowered_path_matches( ellipse_geometry, ellipsePathCommandCount, delegate_space )
    );
    EXPECT_TRUE( lowered_path_matches( triangle_geometry,
                                       trianglePathCommandCount,
                                       delegate_space ) );
}

TEST( OverlayService,
      PathCommandsAreAffinelyTransformedWithoutChangingGeometryType )
{
    grab::detail::SpaceGraph graph;
    const auto               source_space   = graph.add_space( topologyGeneration );
    const auto               delegate_space = graph.add_space( topologyGeneration );
    graph.add_transform( grab::TransformRecord{
        .source      = source_space,
        .destination = delegate_space,
        .map =
            {
                  .xx = axisScaleX,
                  .tx = axisTranslateX,
                  .yy = axisScaleY,
                  .ty = axisTranslateY,
                  },
        .mapping_id = mappingId,
        .generation = topologyGeneration,
        .trust      = grab::TransformTrust::Exact,
    } );
    OverlayRuntime runtime;
    auto           service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;

    const auto added = ( *service )->add( path_shape( source_space ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const auto& delivered = runtime.delegate().shapes().at( *added ).shape.geometry;
    const auto* path      = std::get_if<grab::overlay::Path>( &delivered );
    ASSERT_NE( path, nullptr );
    ASSERT_EQ( path->commands.size(), sourcePathCommandCount );
    ASSERT_TRUE( path_uses_space( *path, delegate_space ) );
    const auto* move = std::get_if<grab::overlay::MoveTo>( &path->commands.front() );
    ASSERT_NE( move, nullptr );
    EXPECT_DOUBLE_EQ( move->point.x, expectedRectX );
    EXPECT_DOUBLE_EQ( move->point.y, expectedRectY );
}

TEST( OverlayService,
      FlushPropagatesDelegateFailure )
{
    grab::detail::SpaceGraph graph;
    const auto               delegate_space = graph.add_space( topologyGeneration );
    OverlayRuntime           runtime;
    auto                     service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;
    const auto added = ( *service )->add( rect_shape( delegate_space ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    runtime.fail_next_flush( grab::ErrorCode::ProviderFailed,
                             std::string{ flushFailureMessage } );

    const auto flushed = ( *service )->flush();

    ASSERT_FALSE( flushed.has_value() );
    EXPECT_EQ( flushed.error().code, grab::ErrorCode::ProviderFailed );
    EXPECT_EQ( flushed.error().message, flushFailureMessage );
}

TEST( OverlayService,
      UnknownSourceSpaceReturnsSpaceGraphErrorWithoutApplyingDelta )
{
    grab::detail::SpaceGraph graph;
    const auto               delegate_space = graph.add_space( topologyGeneration );
    OverlayRuntime           runtime;
    auto                     service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;

    const auto added =
        ( *service )->add( rect_shape( grab::CoordinateSpaceId{ unknownSpaceValue } ) );

    ASSERT_FALSE( added.has_value() );
    EXPECT_EQ( added.error().code, grab::ErrorCode::RouteUnavailable );
    ASSERT_EQ( runtime.delegate().calls().size(), singleCallCount );
    EXPECT_TRUE( std::holds_alternative<grab::testing::OverlayOpenCall>(
        runtime.delegate().calls().at( openCallIndex )
    ) );

    const auto recovered = ( *service )->add( rect_shape( delegate_space ) );
    ASSERT_TRUE( recovered.has_value() ) << recovered.error().message;
    const auto* apply = std::get_if<grab::testing::OverlayApplyCall>(
        &runtime.delegate().calls().back()
    );
    ASSERT_NE( apply, nullptr );
    ASSERT_EQ( apply->deltas.size(), singleCallCount );
    EXPECT_EQ( apply->deltas.front().revision.value, firstSceneRevision );
}

TEST( OverlayService,
      ApplyFailureMakesNextVerbResyncSnapshotBeforeItsDelta )
{
    grab::detail::SpaceGraph graph;
    const auto               source_space   = graph.add_space( topologyGeneration );
    const auto               delegate_space = graph.add_space( topologyGeneration );
    graph.add_transform( grab::TransformRecord{
        .source      = source_space,
        .destination = delegate_space,
        .map =
            {
                  .xx = axisScaleX,
                  .tx = axisTranslateX,
                  .yy = axisScaleY,
                  .ty = axisTranslateY,
                  },
        .mapping_id = mappingId,
        .generation = topologyGeneration,
        .trust      = grab::TransformTrust::Exact,
    } );
    OverlayRuntime runtime;
    auto           service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;
    runtime.delegate().fail_next_apply( grab::ErrorCode::ProviderFailed,
                                        "injected apply failure" );

    const auto first  = ( *service )->add( rect_shape( source_space ) );
    const auto second = ( *service )->add( ellipse_shape( source_space ) );

    ASSERT_TRUE( first.has_value() && second.has_value() );
    EXPECT_TRUE(
        recovered_in_order( runtime.delegate(), *first, *second, delegate_space )
    );
}

TEST( OverlayService,
      ThrownApplyExceptionAlsoMakesNextVerbResyncBeforeItsDelta )
{
    grab::detail::SpaceGraph graph;
    const auto               delegate_space = graph.add_space( topologyGeneration );
    OverlayRuntime           runtime;
    auto                     service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;
    runtime.throw_next_apply();

    const auto first  = ( *service )->add( rect_shape( delegate_space ) );
    const auto second = ( *service )->add( ellipse_shape( delegate_space ) );

    ASSERT_TRUE( first.has_value() && second.has_value() );
    EXPECT_TRUE(
        recovered_after_apply_exception( runtime.delegate(), *first, *second )
    );
}

TEST( OverlayService,
      TwoSessionsExposeStableFacadesWithIndependentScenes )
{
    auto        first_thread_log  = std::make_shared<DelegateThreadLog>();
    auto        second_thread_log = std::make_shared<DelegateThreadLog>();
    auto        first_runtime     = std::make_unique<OverlayRuntime>( first_thread_log );
    auto        second_runtime  = std::make_unique<OverlayRuntime>( second_thread_log );
    auto* const first_delegate  = &first_runtime->delegate();
    auto* const second_delegate = &second_runtime->delegate();
    auto        first_session =
        grab::Session::open_owning_runtime( std::move( first_runtime ) );
    auto second_session =
        grab::Session::open_owning_runtime( std::move( second_runtime ) );
    ASSERT_TRUE( first_session.has_value() && second_session.has_value() );
    const auto first_reactor_thread  = reactor_thread_for( **first_session );
    const auto second_reactor_thread = reactor_thread_for( **second_session );
    ASSERT_TRUE( first_reactor_thread.has_value() && second_reactor_thread.has_value() );

    const auto first_overlay  = ( *first_session )->overlay();
    const auto second_overlay = ( *second_session )->overlay();
    ASSERT_TRUE( first_overlay.has_value() && second_overlay.has_value() );
    ASSERT_NE( *first_overlay, *second_overlay );
    const auto repeated_first_overlay = ( *first_session )->overlay();
    ASSERT_TRUE( repeated_first_overlay.has_value() );
    EXPECT_EQ( *repeated_first_overlay, *first_overlay );

    const auto first_id =
        ( *first_overlay )->add( rect_shape( first_delegate->space() ) );
    const auto second_id =
        ( *second_overlay )->add( rect_shape( second_delegate->space() ) );
    ASSERT_TRUE( first_id.has_value() && second_id.has_value() );
    auto updated = rect_shape( first_delegate->space() );
    std::get<grab::overlay::Rect>( updated.geometry ).bounds.x += sourceWidth;
    ASSERT_TRUE(
        ( *first_overlay )->update( *first_id, std::move( updated ) ).has_value()
    );
    ASSERT_TRUE( ( *first_overlay )->remove( *first_id ).has_value() );
    ( *first_overlay )->clear();
    ASSERT_TRUE( ( *first_overlay )->flush().has_value() );

    EXPECT_TRUE( first_delegate->shapes().empty() );
    ASSERT_EQ( second_delegate->shapes().size(), singleShapeCount );
    EXPECT_TRUE( second_delegate->shapes().contains( *second_id ) );
    EXPECT_TRUE( every_call_uses_thread( *first_thread_log, *first_reactor_thread ) );
    EXPECT_TRUE( every_call_uses_thread( *second_thread_log, *second_reactor_thread ) );

    const auto first_call_count_before_close  = first_thread_log->size();
    const auto second_call_count_before_close = second_thread_log->size();
    ( *first_session )->close();
    ( *second_session )->close();
    EXPECT_EQ( first_thread_log->size(),
               first_call_count_before_close + singleCallCount );
    EXPECT_EQ( second_thread_log->size(),
               second_call_count_before_close + singleCallCount );
    EXPECT_TRUE( every_call_uses_thread( *first_thread_log, *first_reactor_thread ) );
    EXPECT_TRUE( every_call_uses_thread( *second_thread_log, *second_reactor_thread ) );
}

TEST( OverlayService,
      ReactorThreadCloseDrainsWorkQueuedBeforeShutdown )
{
    auto thread_log = std::make_shared<DelegateThreadLog>();
    auto runtime    = std::make_unique<OverlayRuntime>( thread_log );
    auto session    = grab::Session::open_owning_runtime( std::move( runtime ) );
    ASSERT_TRUE( session.has_value() );
    ASSERT_TRUE( ( *session )->overlay().has_value() );
    const auto reactor_thread = reactor_thread_for( **session );
    ASSERT_TRUE( reactor_thread.has_value() );

    auto        close_entered     = std::make_shared<std::promise<void>>();
    auto        close_has_entered = close_entered->get_future();
    auto        release_close     = std::make_shared<std::promise<void>>();
    auto        close_released    = release_close->get_future().share();
    auto        queued_completed  = std::make_shared<std::promise<void>>();
    auto        work_completed    = queued_completed->get_future();
    auto* const open_session      = ( *session ).get();

    auto        close_posted      = open_session->post(
        [close_entered, close_released, open_session]
        {
            close_entered->set_value();
            close_released.wait();
            open_session->close();
        }
    );
    ASSERT_TRUE( close_posted.has_value() );
    close_has_entered.wait();

    auto work_posted = open_session->post(
        [queued_completed]
        {
            queued_completed->set_value();
        }
    );
    ASSERT_TRUE( work_posted.has_value() );
    release_close->set_value();

    EXPECT_EQ( work_completed.wait_for( reactorDrainTimeout ),
               std::future_status::ready );
    EXPECT_FALSE( open_session->is_open() );
    open_session->close();
    EXPECT_TRUE( every_call_uses_thread( *thread_log, *reactor_thread ) );
}

TEST( OverlayService,
      FlushFailureMarksServiceDesynchronizedAndNextVerbRecovers )
{
    constexpr std::size_t    minimumResyncCalls = 1U;
    grab::detail::SpaceGraph graph;
    const auto               space = graph.add_space( topologyGeneration );
    OverlayRuntime           runtime;
    auto                     service =
        grab::kernel::presentation::OverlayService::create( runtime,
                                                            graph,
                                                            space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;
    ASSERT_TRUE( ( *service )->add( rect_shape( space ) ).has_value() );

    runtime.delegate().fail_next_flush( grab::ErrorCode::DeviceInaccessible,
                                        "injected fence loss" );
    const auto failed = ( *service )->flush();
    ASSERT_FALSE( failed.has_value() );

    // The failed fence desynchronized the delegate; the next verb must route
    // through recovery (resync) instead of wedging on ResyncRequired.
    const auto readded = ( *service )->add( rect_shape( space ) );
    ASSERT_TRUE( readded.has_value() ) << readded.error().message;

    std::size_t resync_calls = 0U;
    for( const auto& call : runtime.delegate().calls() )
    {
        if( std::holds_alternative<grab::testing::OverlayResyncCall>( call ) )
        {
            ++resync_calls;
        }
    }
    EXPECT_GE( resync_calls, minimumResyncCalls );
    EXPECT_TRUE( ( *service )->flush().has_value() );
}
