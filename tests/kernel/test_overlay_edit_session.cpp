#include "fake/fake_overlay_delegate.hpp"
#include "fake/fake_runtime.hpp"
#include "grab/context.hpp"
#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
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
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
// clang-format on

namespace
{

    constexpr std::chrono::milliseconds sceneNow{ 125 };
    constexpr std::uint32_t             topologyGeneration      = 1U;
    constexpr std::uint64_t             mappingId               = 1U;
    constexpr std::uint64_t             initialTreeRevision     = 1U;
    constexpr std::uint64_t             fixtureRootNodeValue    = 1U;
    constexpr std::uint64_t             committedRevisionValue  = 3U;
    constexpr std::size_t               singleCancellationCount = 1U;
    constexpr double                    shapeLeft               = 10.0;
    constexpr double                    shapeTop                = 20.0;
    constexpr double                    shapeWidth              = 40.0;
    constexpr double                    shapeHeight             = 30.0;
    constexpr double                    centerFraction          = 0.5;
    constexpr double           pressX = shapeLeft + ( shapeWidth * centerFraction );
    constexpr double           pressY = shapeTop + ( shapeHeight * centerFraction );
    constexpr double           previewDeltaX        = 4.0;
    constexpr double           previewDeltaY        = -3.0;
    constexpr double           committedDeltaX      = 7.0;
    constexpr double           committedDeltaY      = -5.0;
    constexpr double           previewX             = pressX + previewDeltaX;
    constexpr double           previewY             = pressY + previewDeltaY;
    constexpr double           releaseX             = pressX + committedDeltaX;
    constexpr double           releaseY             = pressY + committedDeltaY;
    constexpr double           committedLeft        = shapeLeft + committedDeltaX;
    constexpr double           committedTop         = shapeTop + committedDeltaY;
    constexpr double           axisScaleX           = 2.0;
    constexpr double           axisScaleY           = 3.0;
    constexpr double           axisTranslateX       = 5.0;
    constexpr double           axisTranslateY       = -7.0;
    constexpr std::uint8_t     primaryPointerButton = 1U;
    constexpr std::string_view runtimeName          = "overlay-edit-test-runtime";
    constexpr std::string_view regionFailureMessage = "injected input-region failure";
    constexpr std::string_view grabFailureMessage   = "injected pointer-grab failure";

    class EditRuntime final : public grab::spi::Runtime
    {
        public:

            EditRuntime()
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
                return delegate_;
            }

        private:

            grab::testing::FakeRuntime         backing_;
            grab::testing::FakeOverlayDelegate delegate_;
    };

    [[nodiscard]]
    grab::overlay::Shape
    filled_rect( grab::CoordinateSpaceId space )
    {
        grab::overlay::Shape shape;
        shape.geometry = grab::overlay::Rect{
            .bounds = {
                       .x     = shapeLeft,
                       .y     = shapeTop,
                       .w     = shapeWidth,
                       .h     = shapeHeight,
                       .space = space,
                       },
        };
        shape.fill.emplace();
        return shape;
    }

    [[nodiscard]]
    const grab::overlay::Rect&
    as_rect( const grab::overlay::Shape& shape )
    {
        return std::get<grab::overlay::Rect>( shape.geometry );
    }

    [[nodiscard]]
    grab::spi::OverlayEditEvent
    edit_event( grab::spi::OverlayEditEventKind kind,
                grab::CoordinateSpaceId         space,
                double                          x,
                double                          y )
    {
        return grab::spi::OverlayEditEvent{
            .kind = kind,
            .position =
                {
                           .x     = x,
                           .y     = y,
                           .space = space,
                           },
            .button = primaryPointerButton,
        };
    }

    class OverlayEditSession : public ::testing::Test
    {
        protected:

            void
            SetUp() override
            {
                auto runtime = std::make_unique<EditRuntime>();
                delegate_    = &runtime->delegate();

                auto opened = grab::Session::open_owning_runtime( std::move( runtime ) );
                ASSERT_TRUE( opened.has_value() ) << opened.error().message;
                session_     = std::move( *opened );

                auto overlay = session_->overlay();
                ASSERT_TRUE( overlay.has_value() ) << overlay.error().message;
                overlay_   = *overlay;

                auto space = overlay_->space();
                ASSERT_TRUE( space.has_value() ) << space.error().message;
                space_ = *space;
            }

            [[nodiscard]]
            grab::Result<void>
            dispatch( grab::spi::OverlayEditEvent event )
            {
                auto completion = std::make_shared<std::promise<void>>();
                auto completed  = completion->get_future();
                auto posted     = session_->post(
                    [delegate = delegate_, event, completion]
                    {
                        delegate->emit_edit_event( event );
                        completion->set_value();
                    }
                );
                if( !posted.has_value() )
                {
                    return std::unexpected( std::move( posted.error() ) );
                }
                completed.get();
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            dispatch( grab::spi::OverlayEditEventKind kind,
                      double                          x,
                      double                          y )
            {
                return dispatch( edit_event( kind, space_, x, y ) );
            }

            std::unique_ptr<grab::Session>      session_;
            grab::testing::FakeOverlayDelegate* delegate_{};
            grab::Overlay*                      overlay_{};
            grab::CoordinateSpaceId             space_{};
    };

}    // namespace

TEST_F( OverlayEditSession,
        StartSession_InstallsNonEmptyInputRegion )
{
    const auto added = overlay_->add( filled_rect( space_ ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const std::array editable{ *added };

    {
        auto started = grab::overlay_edit( *overlay_, editable, grab::EditCallbacks{} );
        ASSERT_TRUE( started.has_value() ) << started.error().message;
        auto edit = std::move( *started );

        EXPECT_FALSE( delegate_->input_region().empty() );
        EXPECT_TRUE( delegate_->edit_handler_installed() );
        EXPECT_FALSE( delegate_->pointer_grabbed() );
        EXPECT_TRUE( edit.status().has_value() );
    }

    EXPECT_TRUE( delegate_->input_region().empty() );
    EXPECT_FALSE( delegate_->edit_handler_installed() );
    EXPECT_FALSE( delegate_->pointer_grabbed() );
}

TEST_F( OverlayEditSession,
        Teardown_RestoresEmptyRegionOnErrorPath )
{
    const auto added = overlay_->add( filled_rect( space_ ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const std::array editable{ *added };
    auto started = grab::overlay_edit( *overlay_, editable, grab::EditCallbacks{} );
    ASSERT_TRUE( started.has_value() ) << started.error().message;
    auto edit = std::move( *started );

    ASSERT_TRUE( dispatch( grab::spi::OverlayEditEventKind::ButtonPress, pressX, pressY )
                     .has_value() );
    ASSERT_TRUE( delegate_->pointer_grabbed() );
    delegate_->fail_next_input_region( grab::ErrorCode::ProviderFailed,
                                       std::string{ regionFailureMessage } );

    ASSERT_TRUE(
        dispatch( grab::spi::OverlayEditEventKind::PointerMotion, previewX, previewY )
            .has_value()
    );

    const auto status = edit.status();
    ASSERT_FALSE( status.has_value() );
    EXPECT_EQ( status.error().code, grab::ErrorCode::ProviderFailed );
    EXPECT_EQ( status.error().message, regionFailureMessage );
    EXPECT_TRUE( delegate_->input_region().empty() );
    EXPECT_FALSE( delegate_->edit_handler_installed() );
    EXPECT_FALSE( delegate_->pointer_grabbed() );
    ASSERT_TRUE( delegate_->shapes().contains( *added ) );
    const auto& restored = as_rect( delegate_->shapes().at( *added ).shape );
    EXPECT_DOUBLE_EQ( restored.bounds.x, shapeLeft );
    EXPECT_DOUBLE_EQ( restored.bounds.y, shapeTop );
}

TEST_F( OverlayEditSession,
        CommittedDrag_AppliedToSceneBeforeCallback )
{
    const auto added = overlay_->add( filled_rect( space_ ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const std::array    editable{ *added };

    bool                callback_called{};
    bool                callback_found_scene_shape{};
    bool                callback_flush_succeeded{};
    double              callback_shape_left{};
    double              callback_shape_top{};
    double              scene_shape_left_at_callback{};
    double              scene_shape_top_at_callback{};
    std::uint64_t       scene_revision_at_callback{};
    grab::EditCallbacks callbacks;
    callbacks.on_edit =
        [&]( grab::overlay::ShapeId id, const grab::overlay::Shape& shape )
    {
        callback_called            = true;
        callback_shape_left        = as_rect( shape ).bounds.x;
        callback_shape_top         = as_rect( shape ).bounds.y;
        const auto found           = delegate_->shapes().find( id );
        callback_found_scene_shape = found != delegate_->shapes().end();
        if( callback_found_scene_shape )
        {
            scene_shape_left_at_callback = as_rect( found->second.shape ).bounds.x;
            scene_shape_top_at_callback  = as_rect( found->second.shape ).bounds.y;
        }
        scene_revision_at_callback = delegate_->through_revision().value;
        callback_flush_succeeded   = overlay_->flush().has_value();
    };

    auto started = grab::overlay_edit( *overlay_, editable, std::move( callbacks ) );
    ASSERT_TRUE( started.has_value() ) << started.error().message;
    auto edit = std::move( *started );

    ASSERT_TRUE( dispatch( grab::spi::OverlayEditEventKind::ButtonPress, pressX, pressY )
                     .has_value() );
    ASSERT_TRUE(
        dispatch( grab::spi::OverlayEditEventKind::PointerMotion, previewX, previewY )
            .has_value()
    );
    ASSERT_TRUE(
        dispatch( grab::spi::OverlayEditEventKind::ButtonRelease, releaseX, releaseY )
            .has_value()
    );

    EXPECT_TRUE( callback_called );
    EXPECT_TRUE( callback_found_scene_shape );
    EXPECT_TRUE( callback_flush_succeeded );
    EXPECT_DOUBLE_EQ( callback_shape_left, committedLeft );
    EXPECT_DOUBLE_EQ( callback_shape_top, committedTop );
    EXPECT_DOUBLE_EQ( scene_shape_left_at_callback, committedLeft );
    EXPECT_DOUBLE_EQ( scene_shape_top_at_callback, committedTop );
    EXPECT_EQ( scene_revision_at_callback, committedRevisionValue );
    EXPECT_FALSE( delegate_->pointer_grabbed() );
    EXPECT_TRUE( edit.status().has_value() );
}

TEST_F( OverlayEditSession,
        GrabFailure_AbortsCleanlySetsStatus )
{
    const auto added = overlay_->add( filled_rect( space_ ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const std::array                      editable{ *added };
    std::size_t                           cancelled_count{};
    std::optional<grab::overlay::ShapeId> cancelled_id;
    grab::EditCallbacks                   callbacks;
    callbacks.on_cancelled = [&]( grab::overlay::ShapeId id )
    {
        ++cancelled_count;
        cancelled_id = id;
    };

    auto started = grab::overlay_edit( *overlay_, editable, std::move( callbacks ) );
    ASSERT_TRUE( started.has_value() ) << started.error().message;
    auto edit = std::move( *started );
    delegate_->fail_next_grab( grab::ErrorCode::ProviderFailed,
                               std::string{ grabFailureMessage } );

    ASSERT_TRUE( dispatch( grab::spi::OverlayEditEventKind::ButtonPress, pressX, pressY )
                     .has_value() );

    const auto status = edit.status();
    ASSERT_FALSE( status.has_value() );
    EXPECT_EQ( status.error().code, grab::ErrorCode::ProviderFailed );
    EXPECT_EQ( status.error().message, grabFailureMessage );
    EXPECT_TRUE( delegate_->input_region().empty() );
    EXPECT_FALSE( delegate_->edit_handler_installed() );
    EXPECT_FALSE( delegate_->pointer_grabbed() );
    EXPECT_EQ( cancelled_count, singleCancellationCount );
    ASSERT_TRUE( cancelled_id.has_value() );
    EXPECT_EQ( *cancelled_id, *added );
}

TEST_F( OverlayEditSession,
        RemoveShapeMidDrag_CancelsAndFires )
{
    const auto added = overlay_->add( filled_rect( space_ ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const std::array                      editable{ *added };
    std::size_t                           cancelled_count{};
    std::optional<grab::overlay::ShapeId> cancelled_id;
    grab::EditCallbacks                   callbacks;
    callbacks.on_cancelled = [&]( grab::overlay::ShapeId id )
    {
        ++cancelled_count;
        cancelled_id = id;
    };
    auto started = grab::overlay_edit( *overlay_, editable, std::move( callbacks ) );
    ASSERT_TRUE( started.has_value() ) << started.error().message;
    auto edit = std::move( *started );

    ASSERT_TRUE( dispatch( grab::spi::OverlayEditEventKind::ButtonPress, pressX, pressY )
                     .has_value() );
    ASSERT_TRUE(
        dispatch( grab::spi::OverlayEditEventKind::PointerMotion, previewX, previewY )
            .has_value()
    );
    ASSERT_TRUE( delegate_->pointer_grabbed() );

    const auto removed = overlay_->remove( *added );
    ASSERT_TRUE( removed.has_value() ) << removed.error().message;

    EXPECT_EQ( cancelled_count, singleCancellationCount );
    ASSERT_TRUE( cancelled_id.has_value() );
    EXPECT_EQ( *cancelled_id, *added );
    EXPECT_FALSE( delegate_->shapes().contains( *added ) );
    EXPECT_TRUE( delegate_->input_region().empty() );
    EXPECT_FALSE( delegate_->pointer_grabbed() );
    EXPECT_TRUE( delegate_->edit_handler_installed() );
    EXPECT_TRUE( edit.status().has_value() );
}

TEST_F( OverlayEditSession,
        ConcurrentSession_ReturnsError )
{
    const auto added = overlay_->add( filled_rect( space_ ) );
    ASSERT_TRUE( added.has_value() ) << added.error().message;
    const std::array editable{ *added };

    {
        auto first_started =
            grab::overlay_edit( *overlay_, editable, grab::EditCallbacks{} );
        ASSERT_TRUE( first_started.has_value() ) << first_started.error().message;
        auto       first = std::move( *first_started );

        const auto second =
            grab::overlay_edit( *overlay_, editable, grab::EditCallbacks{} );
        ASSERT_FALSE( second.has_value() );
        EXPECT_EQ( second.error().code, grab::ErrorCode::SessionExists );
        EXPECT_FALSE( delegate_->input_region().empty() );
        EXPECT_TRUE( delegate_->edit_handler_installed() );
        EXPECT_TRUE( first.status().has_value() );
    }

    EXPECT_TRUE( delegate_->input_region().empty() );
    EXPECT_FALSE( delegate_->edit_handler_installed() );
}

TEST_F( OverlayEditSession,
        AnimatedAndTransformShapes_Rejected )
{
    auto animated = filled_rect( space_ );
    animated.animation.emplace();
    const auto animated_id = overlay_->add( std::move( animated ) );
    ASSERT_TRUE( animated_id.has_value() ) << animated_id.error().message;
    const std::array animated_editable{ *animated_id };

    const auto       animated_session =
        grab::overlay_edit( *overlay_, animated_editable, grab::EditCallbacks{} );
    ASSERT_FALSE( animated_session.has_value() );
    EXPECT_EQ( animated_session.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_TRUE( delegate_->input_region().empty() );
    EXPECT_FALSE( delegate_->edit_handler_installed() );

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
    EditRuntime transformed_runtime;
    auto        service =
        grab::kernel::presentation::OverlayService::create( transformed_runtime,
                                                            graph,
                                                            delegate_space,
                                                            []
                                                            {
                                                                return sceneNow;
                                                            } );
    ASSERT_TRUE( service.has_value() ) << service.error().message;
    const auto transformed_id = ( *service )->add( filled_rect( source_space ) );
    ASSERT_TRUE( transformed_id.has_value() ) << transformed_id.error().message;
    const std::array transformed_editable{ *transformed_id };

    const auto       transformed_session =
        ( *service )->start_edit( transformed_editable, grab::EditCallbacks{} );
    ASSERT_FALSE( transformed_session.has_value() );
    EXPECT_EQ( transformed_session.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_TRUE( transformed_runtime.delegate().input_region().empty() );
    EXPECT_FALSE( transformed_runtime.delegate().edit_handler_installed() );
}
