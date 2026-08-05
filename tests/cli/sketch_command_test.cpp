#include "frontends/cli/sketch_command.hpp"
#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_draw.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view          strokePxFlag     = "--stroke-px";
    constexpr std::string_view          filledFlag       = "--filled";
    constexpr std::string_view          colorFlag        = "--color";
    constexpr std::string_view          unknownFlag      = "--unknown";
    constexpr std::string_view          strokeText       = "7.5";
    constexpr std::string_view          colorText        = "12aBcF";
    constexpr std::string_view          badColorText     = "12345";
    constexpr std::string_view          editKey          = "e";
    constexpr std::string_view          rectangleKey     = "r";
    constexpr std::string_view          ellipseKey       = "o";
    constexpr std::string_view          pathKey          = "p";
    constexpr std::string_view          escapeKey        = "Escape";
    constexpr std::string_view          backspaceKey     = "BackSpace";
    constexpr std::string_view          buttonName       = "Button1";
    constexpr std::string_view          motionAxis       = "xy";
    constexpr float                     expectedStrokePx = 7.5F;
    constexpr std::uint8_t              expectedRed      = 0X12U;
    constexpr std::uint8_t              expectedGreen    = 0XABU;
    constexpr std::uint8_t              expectedBlue     = 0XCFU;
    constexpr std::uint8_t              expectedAlpha    = 0XFFU;
    constexpr std::uint32_t             arbitraryKeyCode = 99U;
    constexpr std::uint32_t             primaryButton    = 1U;
    constexpr std::uint32_t             firstShapeSlot   = 1U;
    constexpr std::uint32_t             secondShapeSlot  = 2U;
    constexpr std::size_t               noCalls          = 0U;
    constexpr std::size_t               oneCall          = 1U;
    constexpr std::size_t               twoCalls         = 2U;
    constexpr std::size_t               fourPathCommands = 4U;
    constexpr double                    noMotionDelta{};
    constexpr grab::CoordinateSpaceId   testSpace{ 17U };
    constexpr grab::overlay::SceneEpoch testEpoch{ 4U };
    constexpr grab::SpacePoint          origin{
        .x     = 10.0,
        .y     = 20.0,
        .space = testSpace,
    };
    constexpr grab::SpacePoint firstExtent{
        .x     = 30.0,
        .y     = 45.0,
        .space = testSpace,
    };
    constexpr grab::SpacePoint secondOrigin{
        .x     = 50.0,
        .y     = 60.0,
        .space = testSpace,
    };
    constexpr grab::SpacePoint secondExtent{
        .x     = 80.0,
        .y     = 95.0,
        .space = testSpace,
    };
    constexpr grab::SpacePoint pathPointOne{
        .x     = 14.0,
        .y     = 24.0,
        .space = testSpace,
    };
    constexpr grab::SpacePoint pathPointTwo{
        .x     = 18.0,
        .y     = 28.0,
        .space = testSpace,
    };
    constexpr grab::SpacePoint pathPointThree{
        .x     = 22.0,
        .y     = 32.0,
        .space = testSpace,
    };

    class BackendRecorder
    {
        public:

            [[nodiscard]]
            grab::cli::SketchBackend
            backend()
            {
                return grab::cli::SketchBackend{
                    .add_shape =
                        [this]( grab::overlay::Shape shape )
                    {
                        added_shapes.push_back( std::move( shape ) );
                        const grab::overlay::ShapeId id{
                            .epoch = testEpoch,
                            .slot  = next_slot,
                        };
                        ++next_slot;
                        added_ids.push_back( id );
                        return grab::Result<grab::overlay::ShapeId>{ id };
                    },
                    .update_shape =
                        [this]( grab::overlay::ShapeId id,
                                grab::overlay::Shape   shape ) -> grab::Result<void>
                    {
                        updated_ids.push_back( id );
                        updated_shapes.push_back( std::move( shape ) );
                        return {};
                    },
                    .remove_shape =
                        [this]( grab::overlay::ShapeId id ) -> grab::Result<void>
                    {
                        removed_ids.push_back( id );
                        return {};
                    },
                    .begin_edit = [this](
                                      std::span<const grab::overlay::ShapeId> editable
                                  ) -> grab::Result<void>
                    {
                        ++begin_edit_calls;
                        edit_sets.emplace_back( editable.begin(), editable.end() );
                        return {};
                    },
                    .end_edit = [this]() -> grab::Result<void>
                    {
                        ++end_edit_calls;
                        return {};
                    },
                    .capture_pointer = [this]() -> grab::Result<void>
                    {
                        ++capture_calls;
                        captured = true;
                        return {};
                    },
                    .release_pointer = [this]() -> grab::Result<void>
                    {
                        ++release_calls;
                        captured = false;
                        return {};
                    },
                };
            }

            std::uint32_t                                    next_slot{ firstShapeSlot };
            std::vector<grab::overlay::ShapeId>              added_ids;
            std::vector<grab::overlay::Shape>                added_shapes;
            std::vector<grab::overlay::ShapeId>              updated_ids;
            std::vector<grab::overlay::Shape>                updated_shapes;
            std::vector<grab::overlay::ShapeId>              removed_ids;
            std::vector<std::vector<grab::overlay::ShapeId>> edit_sets;
            std::size_t                                      capture_calls{};
            std::size_t                                      release_calls{};
            bool                                             captured{};
            std::size_t                                      begin_edit_calls{};
            std::size_t                                      end_edit_calls{};
    };

    [[nodiscard]]
    grab::Event
    key_event( std::string_view name )
    {
        grab::Event event;
        event.kind     = grab::EventKind::KeyDown;
        event.category = grab::EventCategory::Input;
        event.payload  = grab::InputKey{
            .code = arbitraryKeyCode,
            .name = std::string{ name },
        };
        return event;
    }

    [[nodiscard]]
    grab::Event
    button_event( grab::EventKind  kind,
                  grab::SpacePoint point )
    {
        grab::Event event;
        event.kind     = kind;
        event.category = grab::EventCategory::Input;
        event.payload  = grab::MouseButton{
            .button   = primaryButton,
            .name     = std::string{ buttonName },
            .position = point,
        };
        return event;
    }

    [[nodiscard]]
    grab::Event
    motion_event( grab::SpacePoint point )
    {
        grab::Event event;
        event.kind     = grab::EventKind::MouseMove;
        event.category = grab::EventCategory::Input;
        event.payload  = grab::MouseMove{
            .axis     = std::string{ motionAxis },
            .delta    = noMotionDelta,
            .position = point,
        };
        return event;
    }

    void
    commit_rectangle( grab::cli::SketchController& controller,
                      grab::SpacePoint             pressed_at,
                      grab::SpacePoint             released_at )
    {
        const auto pressed =
            controller.consume( button_event( grab::EventKind::MouseButtonDown,
                                              pressed_at ) );
        ASSERT_TRUE( pressed.has_value() ) << pressed.error().message;
        const auto released =
            controller.consume( button_event( grab::EventKind::MouseButtonUp,
                                              released_at ) );
        ASSERT_TRUE( released.has_value() ) << released.error().message;
    }

    TEST( SketchCommand,
          DefaultsToRectangleAndUnfilled )
    {
        const auto parsed = grab::cli::parse_sketch_options( {} );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_EQ( parsed->kind, grab::overlay::DrawKind::Rectangle );
        EXPECT_FALSE( parsed->style.filled );
        EXPECT_FLOAT_EQ( parsed->style.stroke_px, grab::overlay::defaultDrawStrokePx );
    }

    TEST( SketchCommand,
          ParsesStrokeFillAndColor )
    {
        constexpr auto args   = std::to_array<std::string_view>( {
            strokePxFlag,
            strokeText,
            filledFlag,
            colorFlag,
            colorText,
        } );

        const auto     parsed = grab::cli::parse_sketch_options( args );

        ASSERT_TRUE( parsed.has_value() ) << parsed.error().message;
        EXPECT_FLOAT_EQ( parsed->style.stroke_px, expectedStrokePx );
        EXPECT_TRUE( parsed->style.filled );
        EXPECT_EQ( parsed->style.color.r, expectedRed );
        EXPECT_EQ( parsed->style.color.g, expectedGreen );
        EXPECT_EQ( parsed->style.color.b, expectedBlue );
        EXPECT_EQ( parsed->style.color.a, expectedAlpha );
    }

    TEST( SketchCommand,
          RejectsMalformedOptionWithClearError )
    {
        constexpr auto malformed =
            std::to_array<std::string_view>( { colorFlag, badColorText } );
        constexpr auto missing = std::to_array<std::string_view>( { strokePxFlag } );
        constexpr auto unknown = std::to_array<std::string_view>( { unknownFlag } );
        constexpr auto repeated =
            std::to_array<std::string_view>( { filledFlag, filledFlag } );

        const auto malformed_result = grab::cli::parse_sketch_options( malformed );
        const auto missing_result   = grab::cli::parse_sketch_options( missing );
        const auto unknown_result   = grab::cli::parse_sketch_options( unknown );
        const auto repeated_result  = grab::cli::parse_sketch_options( repeated );

        ASSERT_FALSE( malformed_result.has_value() );
        EXPECT_EQ( malformed_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( malformed_result.error().message.contains( colorFlag ) );
        ASSERT_FALSE( missing_result.has_value() );
        EXPECT_EQ( missing_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( missing_result.error().message.contains( strokePxFlag ) );
        ASSERT_FALSE( unknown_result.has_value() );
        EXPECT_EQ( unknown_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( unknown_result.error().message.contains( unknownFlag ) );
        ASSERT_FALSE( repeated_result.has_value() );
        EXPECT_EQ( repeated_result.error().code, grab::ErrorCode::InvalidArgument );
        EXPECT_TRUE( repeated_result.error().message.contains( filledFlag ) );
    }

    TEST( SketchCommand,
          KeyNamesSelectKindsAndToggleEdit )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };

        ASSERT_TRUE( controller.consume( key_event( rectangleKey ) ).has_value() );
        EXPECT_EQ( controller.kind(), grab::overlay::DrawKind::Rectangle );
        ASSERT_TRUE( controller.consume( key_event( ellipseKey ) ).has_value() );
        EXPECT_EQ( controller.kind(), grab::overlay::DrawKind::Ellipse );
        ASSERT_TRUE( controller.consume( key_event( pathKey ) ).has_value() );
        EXPECT_EQ( controller.kind(), grab::overlay::DrawKind::Path );

        commit_rectangle( controller, origin, firstExtent );

        ASSERT_TRUE( controller.consume( key_event( editKey ) ).has_value() );
        EXPECT_TRUE( controller.editing() );
        EXPECT_EQ( recorder.begin_edit_calls, oneCall );
        ASSERT_EQ( recorder.edit_sets.size(), oneCall );
        ASSERT_EQ( recorder.edit_sets.front().size(), oneCall );
        EXPECT_EQ( recorder.edit_sets.front().front(), recorder.added_ids.back() );
        ASSERT_TRUE( controller.consume( key_event( editKey ) ).has_value() );
        EXPECT_FALSE( controller.editing() );
        EXPECT_EQ( recorder.end_edit_calls, oneCall );
    }

    // Sketch learns about a press from the observation stream, which the X
    // server delivers to non-grabbing clients regardless of who owns the
    // pointer. If the overlay stays click-through, that same press ALSO reaches
    // the desktop underneath, and GNOME starts its own rubber-band selection
    // alongside grab's -- two rubber bands for one drag.
    //
    // Capture has to be armed on entering a draw kind, not on button-press: by
    // then the press has already been delivered elsewhere.
    TEST( SketchCommand,
          SelectingADrawKindCapturesThePointerBeforeAnyButtonPress )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };

        EXPECT_FALSE( recorder.captured );
        EXPECT_EQ( recorder.capture_calls, noCalls );

        ASSERT_TRUE( controller.consume( key_event( rectangleKey ) ).has_value() );

        EXPECT_TRUE( recorder.captured );
        EXPECT_TRUE( controller.capturing() );
        EXPECT_EQ( recorder.capture_calls, oneCall );

        // Still captured once the stroke actually starts, and not re-armed.
        ASSERT_TRUE( controller
                         .consume( button_event( grab::EventKind::MouseButtonDown,
                                                 origin ) )
                         .has_value() );
        EXPECT_TRUE( recorder.captured );
        EXPECT_EQ( recorder.capture_calls, oneCall );
    }

    TEST( SketchCommand,
          EscapeReleasesTheCapturedPointer )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };

        ASSERT_TRUE( controller.consume( key_event( rectangleKey ) ).has_value() );
        ASSERT_TRUE( recorder.captured );

        ASSERT_TRUE( controller.consume( key_event( escapeKey ) ).has_value() );

        EXPECT_FALSE( recorder.captured );
        EXPECT_FALSE( controller.capturing() );
        EXPECT_EQ( recorder.release_calls, oneCall );
    }

    // Edit mode installs its own input region over the editable shapes, so
    // draw mode's full-surface capture must come down first rather than the two
    // fighting over the same region.
    TEST( SketchCommand,
          SwitchingToEditModeReleasesTheDrawCapture )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };

        ASSERT_TRUE( controller.consume( key_event( rectangleKey ) ).has_value() );
        ASSERT_TRUE( recorder.captured );
        commit_rectangle( controller, origin, firstExtent );

        ASSERT_TRUE( controller.consume( key_event( editKey ) ).has_value() );

        EXPECT_TRUE( controller.editing() );
        EXPECT_FALSE( recorder.captured );
        EXPECT_FALSE( controller.capturing() );
        EXPECT_EQ( recorder.release_calls, oneCall );
    }

    // A capture that fails to install must not leave the controller believing
    // it holds one, and must still have issued the release: a pointer grab the
    // owner has forgotten about freezes the user's whole desktop.
    TEST( SketchCommand,
          AFailedCaptureIsReleasedAndReported )
    {
        BackendRecorder recorder;
        auto            backend = recorder.backend();
        backend.capture_pointer = [&recorder]() -> grab::Result<void>
        {
            ++recorder.capture_calls;
            return grab::fail( grab::ErrorCode::CapabilityUnavailable, "no compositor" );
        };
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, std::move( backend ) };

        const auto result = controller.consume( key_event( rectangleKey ) );

        ASSERT_FALSE( result.has_value() );
        EXPECT_EQ( result.error().code, grab::ErrorCode::CapabilityUnavailable );
        EXPECT_FALSE( controller.capturing() );
        EXPECT_EQ( recorder.release_calls, oneCall );
    }

    TEST( SketchCommand,
          EscapeCancelsInProgressDraw )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };

        ASSERT_TRUE( controller
                         .consume( button_event( grab::EventKind::MouseButtonDown,
                                                 origin ) )
                         .has_value() );
        ASSERT_TRUE( controller.consume( motion_event( firstExtent ) ).has_value() );
        ASSERT_TRUE( controller.flush_preview().has_value() );
        ASSERT_TRUE( controller.preview_visible() );

        ASSERT_TRUE( controller.consume( key_event( escapeKey ) ).has_value() );

        EXPECT_FALSE( controller.drawing() );
        EXPECT_FALSE( controller.preview_visible() );
        EXPECT_TRUE( controller.editable_shapes().empty() );
        ASSERT_EQ( recorder.removed_ids.size(), oneCall );
        EXPECT_EQ( recorder.removed_ids.front().slot, firstShapeSlot );
    }

    TEST( SketchCommand,
          BackSpaceRemovesMostRecentAndEmptyIsNoOp )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };
        commit_rectangle( controller, origin, firstExtent );
        commit_rectangle( controller, secondOrigin, secondExtent );
        ASSERT_EQ( controller.editable_shapes().size(), twoCalls );

        ASSERT_TRUE( controller.consume( key_event( backspaceKey ) ).has_value() );
        ASSERT_TRUE( controller.consume( key_event( backspaceKey ) ).has_value() );
        ASSERT_TRUE( controller.consume( key_event( backspaceKey ) ).has_value() );

        EXPECT_TRUE( controller.editable_shapes().empty() );
        ASSERT_EQ( recorder.removed_ids.size(), twoCalls );
        EXPECT_EQ( recorder.removed_ids.front().slot, secondShapeSlot );
        EXPECT_EQ( recorder.removed_ids.back().slot, firstShapeSlot );
    }

    TEST( SketchCommand,
          DegeneratePressReleaseCommitsNothing )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };

        commit_rectangle( controller, origin, firstExtent );
        const auto editable_before = std::vector<grab::overlay::ShapeId>{
            controller.editable_shapes().begin(),
            controller.editable_shapes().end()
        };
        commit_rectangle( controller, secondOrigin, secondOrigin );

        ASSERT_EQ( recorder.added_ids.size(), oneCall );
        ASSERT_EQ( controller.editable_shapes().size(), oneCall );
        EXPECT_TRUE( std::ranges::equal( controller.editable_shapes(),
                                         editable_before ) );
        EXPECT_FALSE( controller.drawing() );
    }

    TEST( SketchCommand,
          MotionBatchCoalescesPreviewWithoutDroppingPathSamples )
    {
        BackendRecorder                recorder;
        const grab::cli::SketchOptions options{};
        grab::cli::SketchController    controller{ options, recorder.backend() };
        ASSERT_TRUE( controller.consume( key_event( pathKey ) ).has_value() );
        ASSERT_TRUE( controller
                         .consume( button_event( grab::EventKind::MouseButtonDown,
                                                 origin ) )
                         .has_value() );

        ASSERT_TRUE( controller.consume( motion_event( pathPointOne ) ).has_value() );
        ASSERT_TRUE( controller.consume( motion_event( pathPointTwo ) ).has_value() );
        ASSERT_TRUE( controller.consume( motion_event( pathPointThree ) ).has_value() );
        EXPECT_EQ( recorder.added_shapes.size(), noCalls );

        ASSERT_TRUE( controller.flush_preview().has_value() );

        ASSERT_EQ( recorder.added_shapes.size(), oneCall );
        const auto* const path =
            std::get_if<grab::overlay::Path>( &recorder.added_shapes.front().geometry );
        ASSERT_NE( path, nullptr );
        EXPECT_EQ( path->commands.size(), fourPathCommands );
    }

}    // namespace
