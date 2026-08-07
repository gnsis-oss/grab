// The enter/tick/exit triple, exercised with FABRICATED TIME and no X server.
//
// Every `now` below is invented, so a five-second wait and a 128 ms drag both
// run in microseconds of real time. That is not a shortcut: the pump is
// caller-driven precisely so the scheduler can be tested without a display,
// and nothing in this file can join the display-backed intermittents.
//
// The pointer and overlay halves of the seat are grab::testing::RecordingSeat.
// The executor also asks for keys BY NAME, for text and for capture, which the
// recorder does not provide, so SequenceSeat wraps it and adds them — every
// pointer event asserted below still comes out of RecordingSeat itself, and so
// does every overlay call.
//
// The overlay section asserts FORWARDING and nothing more, because forwarding
// is this layer's whole contract: the handle-to-ShapeId map and the per-tick
// repositioning that makes overlay.attach look like a carry live in the seat.
// The exception is overlay.grab, which owns a pointer capture that freezes the
// entire desktop if it outlives its run — that one is asserted hard.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/overlay.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "grab/space.hpp"
#include "kernel/sequence/execute.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/step_diag.hpp"
#include "support/recording_seat.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    using Clock = std::chrono::steady_clock;
    using grab::kernel::sequence::CommandState;
    using grab::kernel::sequence::ExecContext;
    using grab::sequence::Status;
    using OverlayEvent = grab::testing::OverlayEvent;
    using SeatEvent    = grab::testing::SeatEvent;

    // A fabricated origin far from zero, so a test that accidentally compares
    // against a default-constructed time_point fails rather than passes.
    constexpr auto fabricatedEpoch  = std::chrono::seconds{ 1'000 };

    constexpr auto zeroMilliseconds = std::chrono::milliseconds{ 0 };

    struct KeyEvent
    {
            std::string       name{};
            bool              pressed{ false };
            Clock::time_point at{};
    };

    class SequenceSeat final
    {
        public:

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t x,
                                   std::int16_t y )
            {
                return pointer_.move_pointer_absolute( x, y );
            }

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t code,
                    bool         pressed )
            {
                return pointer_.button( code, pressed );
            }

            [[nodiscard]]
            grab::Result<void>
            flush()
            {
                return pointer_.flush();
            }

            [[nodiscard]]
            grab::Result<grab::geometry::Point>
            pointer_position()
            {
                return position_;
            }

            [[nodiscard]]
            grab::Result<void>
            key_by_name( std::string_view name,
                         bool             pressed )
            {
                keys_.push_back( KeyEvent{
                    .name    = std::string{ name },
                    .pressed = pressed,
                    .at      = now_,
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            type_text( std::string_view utf8 )
            {
                typed_.emplace_back( utf8 );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            begin_capture( std::string_view output,
                           std::string_view locator )
            {
                capture_output_  = std::string{ output };
                capture_locator_ = std::string{ locator };
                capture_running_ = true;
                return {};
            }

            // nullopt means "still working", which is what makes an Opaque
            // command Running for as long as it actually takes.
            [[nodiscard]]
            std::optional<grab::Result<void>>
            poll_capture()
            {
                if( capture_running_ )
                {
                    return std::nullopt;
                }
                return grab::Result<void>{};
            }

            void
            finish_capture() noexcept
            {
                capture_running_ = false;
            }

            void
            set_position( grab::geometry::Point where ) noexcept
            {
                position_ = where;
            }

            void
            set_now( Clock::time_point now ) noexcept
            {
                now_ = now;
                pointer_.set_now( now );
            }

            [[nodiscard]]
            const std::vector<SeatEvent>&
            events() const noexcept
            {
                return pointer_.events();
            }

            [[nodiscard]]
            const std::vector<KeyEvent>&
            keys() const noexcept
            {
                return keys_;
            }

            [[nodiscard]]
            const std::vector<std::string>&
            typed() const noexcept
            {
                return typed_;
            }

            [[nodiscard]]
            const std::string&
            capture_output() const noexcept
            {
                return capture_output_;
            }

        private:

            grab::testing::RecordingSeat pointer_{};
            std::vector<KeyEvent>        keys_{};
            std::vector<std::string>     typed_{};
            std::string                  capture_output_{};
            std::string                  capture_locator_{};
            bool                         capture_running_{ false };
            grab::geometry::Point        position_{};
            Clock::time_point            now_{};
    };

    [[nodiscard]]
    Clock::time_point
    fabricated_start() noexcept
    {
        return Clock::time_point{ fabricatedEpoch };
    }

    // The seat and the context share one clock, because in production they
    // share the pump's.
    void
    set_clock( ExecContext<SequenceSeat>& context,
               SequenceSeat&              seat,
               Clock::time_point          now ) noexcept
    {
        context.now = now;
        seat.set_now( now );
    }

    // The overlay tests drive RecordingSeat directly — it satisfies OverlaySeat
    // where SequenceSeat, which forwards only the pointer, keyboard, text and
    // capture halves, does not.
    void
    set_clock( ExecContext<grab::testing::RecordingSeat>& context,
               grab::testing::RecordingSeat&              seat,
               Clock::time_point                          now ) noexcept
    {
        context.now = now;
        seat.set_now( now );
    }

    [[nodiscard]]
    std::vector<SeatEvent>
    events_of_kind( const SequenceSeat& seat,
                    SeatEvent::Kind     kind )
    {
        std::vector<SeatEvent> selected;
        for( const auto& event : seat.events() )
        {
            if( event.kind == kind )
            {
                selected.push_back( event );
            }
        }
        return selected;
    }

    [[nodiscard]]
    grab::input::DragOptions
    paced_options( std::int32_t              steps,
                   std::chrono::milliseconds dwell ) noexcept
    {
        return grab::input::DragOptions{
            .interpolation_steps = steps,
            .step_dwell          = dwell,
            .path                = grab::input::DragOptions::Path::Linear,
        };
    }

    // ── time.wait ────────────────────────────────────────────

    constexpr auto waitDuration           = std::chrono::milliseconds{ 5'000 };
    constexpr auto waitJustBeforeDeadline = std::chrono::milliseconds{ 4'999 };
    constexpr auto waitWellAfterDeadline  = std::chrono::milliseconds{ 5'001 };

    TEST( ExecuteWait,
          RunsUntilItsDeadlineThenSucceedsAndEmitsNoInput )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::WaitCommand{
                                        .duration = waitDuration,
                                        }
        };

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Timed );

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );
        EXPECT_EQ( state.deadline, fabricated_start() + waitDuration );

        EXPECT_EQ(
            grab::kernel::sequence::tick( command, state, context, fabricated_start() ),
            Status::Running
        );
        EXPECT_EQ( grab::kernel::sequence::tick( command,
                                                 state,
                                                 context,
                                                 fabricated_start() +
                                                     waitJustBeforeDeadline ),
                   Status::Running );
        EXPECT_EQ( grab::kernel::sequence::tick( command,
                                                 state,
                                                 context,
                                                 fabricated_start() + waitDuration ),
                   Status::Success );
        EXPECT_EQ( grab::kernel::sequence::tick( command,
                                                 state,
                                                 context,
                                                 fabricated_start() +
                                                     waitWellAfterDeadline ),
                   Status::Success );

        grab::kernel::sequence::exit( command, state, context );

        // A wait is time and nothing else: no motion, no button, not even a
        // flush.
        EXPECT_TRUE( seat.events().empty() );
        EXPECT_TRUE( seat.keys().empty() );
    }

    TEST( ExecuteWait,
          RejectsANegativeDurationRatherThanTreatingItAsZero )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::WaitCommand{
                                        .duration = -waitDuration,
                                        }
        };

        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Failure );
    }

    // ── input.move ───────────────────────────────────────────

    constexpr std::int32_t                moveFromX   = 100;
    constexpr std::int32_t                moveFromY   = 200;
    constexpr std::int32_t                moveToX     = 200;
    constexpr std::int32_t                moveSteps   = 4;
    constexpr auto                        moveDwell   = std::chrono::milliseconds{ 8 };
    constexpr auto                        moveEarlyBy = std::chrono::milliseconds{ 4 };

    // (100,200) → (200,200) over four steps: the walk excludes the origin,
    // includes the target, and lands exactly on it with no rounding drift.
    constexpr std::array<std::int16_t, 4> expectedMoveX{ 125, 150, 175, 200 };

    constexpr grab::geometry::Point       moveFrom{ .x = moveFromX, .y = moveFromY };
    constexpr grab::geometry::Point       moveTo{ .x = moveToX, .y = moveFromY };

    TEST( ExecuteMove,
          EmitsWaypointNAtStartPlusNTimesStepDwell )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::MoveCommand{
                                        .from    = moveFrom,
                                        .to      = moveTo,
                                        .options = paced_options( moveSteps, moveDwell ),
                                        }
        };

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Timed );

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );

        // enter() only places the pointer on the declared origin; not one
        // waypoint has been walked yet.
        auto moves = events_of_kind( seat, SeatEvent::Kind::Move );
        ASSERT_EQ( moves.size(), 1U );
        EXPECT_EQ( moves.front().x, static_cast<std::int16_t>( moveFromX ) );
        EXPECT_EQ( moves.front().y, static_cast<std::int16_t>( moveFromY ) );
        EXPECT_EQ( moves.front().at, fabricated_start() );

        // A tick before the first waypoint is due emits nothing. The walk is
        // paced by the clock, not by how often the pump happens to run.
        set_clock( context, seat, fabricated_start() + moveEarlyBy );
        EXPECT_EQ( grab::kernel::sequence::tick( command, state, context, context.now ),
                   Status::Running );
        EXPECT_EQ( events_of_kind( seat, SeatEvent::Kind::Move ).size(), 1U );

        for( std::size_t index = 0U; index < expectedMoveX.size(); ++index )
        {
            const auto ordinal =
                static_cast<std::chrono::milliseconds::rep>( index + 1U );
            const auto due = fabricated_start() + ( moveDwell * ordinal );

            set_clock( context, seat, due );
            const auto status =
                grab::kernel::sequence::tick( command, state, context, context.now );
            EXPECT_EQ( status,
                       index + 1U == expectedMoveX.size() ? Status::Success
                                                          : Status::Running );

            moves = events_of_kind( seat, SeatEvent::Kind::Move );
            ASSERT_EQ( moves.size(), index + 2U );
            EXPECT_EQ( moves.back().x, expectedMoveX.at( index ) );
            EXPECT_EQ( moves.back().y, static_cast<std::int16_t>( moveFromY ) );
            EXPECT_EQ( moves.back().at, due );
        }

        grab::kernel::sequence::exit( command, state, context );

        // A move holds nothing, so unwinding it releases nothing.
        EXPECT_TRUE( events_of_kind( seat, SeatEvent::Kind::Button ).empty() );
    }

    TEST( ExecuteMove,
          WithNoDeclaredOriginStartsWhereThePointerActuallyIs )
    {
        SequenceSeat seat;
        seat.set_position( moveFrom );

        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::MoveCommand{
                                        .from    = std::nullopt,
                                        .to      = moveTo,
                                        .options = paced_options( moveSteps, moveDwell ),
                                        }
        };

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );

        // Nothing is emitted at enter(): the pointer is already there, so
        // warping to it would be a redundant event a target app can see.
        EXPECT_TRUE( seat.events().empty() );

        set_clock( context, seat, fabricated_start() + ( moveDwell * moveSteps ) );
        EXPECT_EQ( grab::kernel::sequence::tick( command, state, context, context.now ),
                   Status::Success );

        const auto moves = events_of_kind( seat, SeatEvent::Kind::Move );
        ASSERT_EQ( moves.size(), expectedMoveX.size() );
        EXPECT_EQ( moves.back().x, expectedMoveX.back() );
    }

    // ── input.drag ───────────────────────────────────────────

    constexpr std::int32_t                dragFromX = 10;
    constexpr std::int32_t                dragFromY = 20;
    constexpr std::int32_t                dragToX   = 50;
    constexpr std::int32_t                dragSteps = 4;
    constexpr auto                        dragDwell = std::chrono::milliseconds{ 8 };

    constexpr std::array<std::int16_t, 4> expectedDragX{ 20, 30, 40, 50 };

    constexpr grab::geometry::Point       dragFrom{ .x = dragFromX, .y = dragFromY };
    constexpr grab::geometry::Point       dragTo{ .x = dragToX, .y = dragFromY };

    // Origin move, its flush, the press, its flush, then a move and a flush per
    // waypoint, then the release and its flush.
    constexpr std::size_t                 expectedDragEventCount = 14U;
    constexpr std::size_t                 dragPressEventIndex    = 2U;
    constexpr std::size_t                 dragReleaseEventIndex  = 12U;

    [[nodiscard]]
    grab::sequence::Command
    paced_drag()
    {
        return grab::sequence::Command{
            grab::sequence::DragCommand{
                                        .from    = dragFrom,
                                        .to      = dragTo,
                                        .button  = grab::input::primaryButton,
                                        .options = paced_options( dragSteps, dragDwell ),
                                        }
        };
    }

    TEST( ExecuteDrag,
          PressesThenWalksPacedWaypointsThenReleases )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command = paced_drag();

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Timed );

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );
        EXPECT_TRUE( state.held );

        {
            const auto buttons = events_of_kind( seat, SeatEvent::Kind::Button );
            ASSERT_EQ( buttons.size(), 1U );
            EXPECT_TRUE( buttons.front().pressed );
            EXPECT_EQ( buttons.front().button, grab::input::primaryButton );
            EXPECT_EQ( buttons.front().at, fabricated_start() );
        }

        for( std::size_t index = 0U; index < expectedDragX.size(); ++index )
        {
            const auto ordinal =
                static_cast<std::chrono::milliseconds::rep>( index + 1U );
            const auto due = fabricated_start() + ( dragDwell * ordinal );

            set_clock( context, seat, due );
            const auto status =
                grab::kernel::sequence::tick( command, state, context, context.now );
            EXPECT_EQ( status,
                       index + 1U == expectedDragX.size() ? Status::Success
                                                          : Status::Running );

            const auto moves = events_of_kind( seat, SeatEvent::Kind::Move );
            ASSERT_EQ( moves.size(), index + 2U );
            EXPECT_EQ( moves.back().x, expectedDragX.at( index ) );
            EXPECT_EQ( moves.back().at, due );
        }

        const auto lastDue =
            fabricated_start() +
            ( dragDwell * static_cast<std::chrono::milliseconds::rep>( dragSteps ) );

        {
            const auto buttons = events_of_kind( seat, SeatEvent::Kind::Button );
            ASSERT_EQ( buttons.size(), 2U );
            EXPECT_FALSE( buttons.back().pressed );
            EXPECT_EQ( buttons.back().button, grab::input::primaryButton );
            EXPECT_EQ( buttons.back().at, lastDue );
        }

        // Nothing is left down, so the exit() that always runs is a no-op.
        EXPECT_FALSE( state.held );
        const auto settled = seat.events().size();
        grab::kernel::sequence::exit( command, state, context );
        EXPECT_EQ( seat.events().size(), settled );

        ASSERT_EQ( seat.events().size(), expectedDragEventCount );
        EXPECT_EQ( seat.events().at( dragPressEventIndex ).kind,
                   SeatEvent::Kind::Button );
        EXPECT_TRUE( seat.events().at( dragPressEventIndex ).pressed );
        EXPECT_EQ( seat.events().at( dragReleaseEventIndex ).kind,
                   SeatEvent::Kind::Button );
        EXPECT_FALSE( seat.events().at( dragReleaseEventIndex ).pressed );
    }

    TEST( ExecuteDrag,
          InterruptedExitReleasesTheHeldButton )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command = paced_drag();

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );

        const auto interruptedAt = fabricated_start() + dragDwell;
        set_clock( context, seat, interruptedAt );
        ASSERT_EQ( grab::kernel::sequence::tick( command, state, context, context.now ),
                   Status::Running );
        ASSERT_TRUE( state.held );

        const auto beforeUnwind = seat.events().size();
        grab::kernel::sequence::interrupt( command, state, context );

        // A button left down survives the process and reaches the next
        // application, so the unwind must put it back up.
        const auto buttons = events_of_kind( seat, SeatEvent::Kind::Button );
        ASSERT_EQ( buttons.size(), 2U );
        EXPECT_FALSE( buttons.back().pressed );
        EXPECT_EQ( buttons.back().button, grab::input::primaryButton );
        EXPECT_EQ( buttons.back().at, interruptedAt );
        EXPECT_GT( seat.events().size(), beforeUnwind );
        EXPECT_FALSE( state.held );

        // Exiting twice must not press anything a second time.
        const auto released = seat.events().size();
        grab::kernel::sequence::exit( command, state, context );
        EXPECT_EQ( seat.events().size(), released );
    }

    // ── input.follow ─────────────────────────────────────────

    constexpr std::int32_t                followSteps = 4;
    constexpr double                      followEndX  = 40.0;
    constexpr auto                        followDwell = std::chrono::milliseconds{ 8 };

    constexpr std::array<std::int16_t, 4> expectedFollowX{ 10, 20, 30, 40 };

    [[nodiscard]]
    grab::geometry::Curve
    straight_path()
    {
        return grab::geometry::Curve::line(
            grab::geometry::PointF{ .x = 0.0, .y = 0.0 },
            grab::geometry::PointF{ .x = followEndX, .y = 0.0 }
        );
    }

    TEST( ExecuteFollow,
          WalksTheCurveOnTheSameStepDwellScheduleWithNoButtonHeld )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::FollowCommand{
                                          .path    = straight_path(),
                                          .options = paced_options( followSteps, followDwell ),
                                          }
        };

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Timed );

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );

        for( std::size_t index = 0U; index < expectedFollowX.size(); ++index )
        {
            const auto ordinal =
                static_cast<std::chrono::milliseconds::rep>( index + 1U );
            const auto due = fabricated_start() + ( followDwell * ordinal );

            set_clock( context, seat, due );
            const auto status =
                grab::kernel::sequence::tick( command, state, context, context.now );
            EXPECT_EQ( status,
                       index + 1U == expectedFollowX.size() ? Status::Success
                                                            : Status::Running );

            const auto moves = events_of_kind( seat, SeatEvent::Kind::Move );
            // One extra move for the curve's own start point.
            ASSERT_EQ( moves.size(), index + 2U );
            EXPECT_EQ( moves.back().x, expectedFollowX.at( index ) );
            EXPECT_EQ( moves.back().at, due );
        }

        EXPECT_TRUE( events_of_kind( seat, SeatEvent::Kind::Button ).empty() );
    }

    // ── input.key_down / input.key_up ────────────────────────

    const std::string chordModifier = "Control_L";

    TEST( ExecuteKeyDown,
          InterruptedExitReleasesTheHeldKey )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::KeyDownCommand{
                                           .key = chordModifier,
                                           }
        };

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );
        ASSERT_EQ( seat.keys().size(), 1U );
        EXPECT_TRUE( seat.keys().front().pressed );
        EXPECT_TRUE( state.document_hold );

        const auto interruptedAt = fabricated_start() + waitDuration;
        set_clock( context, seat, interruptedAt );
        grab::kernel::sequence::interrupt( command, state, context );

        ASSERT_EQ( seat.keys().size(), 2U );
        EXPECT_EQ( seat.keys().back().name, chordModifier );
        EXPECT_FALSE( seat.keys().back().pressed );
        EXPECT_EQ( seat.keys().back().at, interruptedAt );
        EXPECT_FALSE( state.document_hold );
    }

    TEST( ExecuteKeyDown,
          CompletedExitLeavesTheKeyDownSoAChordCanBeSpelled )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        set_clock( context, seat, fabricated_start() );

        // Ctrl+C is three steps, and it only works if the modifier survives
        // the exit() of the step that pressed it.
        CommandState                  down_state;
        const grab::sequence::Command down{
            grab::sequence::KeyDownCommand{
                                           .key = chordModifier,
                                           }
        };
        ASSERT_EQ( grab::kernel::sequence::enter( down, down_state, context ),
                   Status::Success );
        grab::kernel::sequence::exit( down, down_state, context );
        ASSERT_EQ( seat.keys().size(), 1U );
        EXPECT_TRUE( seat.keys().front().pressed );

        CommandState                  up_state;
        const grab::sequence::Command up{
            grab::sequence::KeyUpCommand{
                                         .key = chordModifier,
                                         }
        };
        ASSERT_EQ( grab::kernel::sequence::enter( up, up_state, context ),
                   Status::Success );
        grab::kernel::sequence::exit( up, up_state, context );

        ASSERT_EQ( seat.keys().size(), 2U );
        EXPECT_FALSE( seat.keys().back().pressed );
    }

    TEST( ExecutePress,
          InterruptedExitReleasesTheHeldButton )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::PressCommand{
                                         .button = grab::input::primaryButton,
                                         }
        };

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );
        ASSERT_EQ( events_of_kind( seat, SeatEvent::Kind::Button ).size(), 1U );

        // A completed press keeps its button down: a later input.release is
        // what lifts it, which is the whole point of a realistic hold.
        grab::kernel::sequence::exit( command, state, context );
        EXPECT_EQ( events_of_kind( seat, SeatEvent::Kind::Button ).size(), 1U );

        grab::kernel::sequence::interrupt( command, state, context );
        const auto buttons = events_of_kind( seat, SeatEvent::Kind::Button );
        ASSERT_EQ( buttons.size(), 2U );
        EXPECT_FALSE( buttons.back().pressed );
    }

    // ── the Instant class ────────────────────────────────────

    constexpr std::int32_t warpX                 = 640;
    constexpr std::int32_t warpY                 = 400;
    constexpr std::int32_t scrollDown            = 2;
    constexpr std::size_t  clickEventCount       = 3U;
    constexpr std::size_t  scrollNotchEventCount = 4U;

    TEST( ExecuteInstant,
          ClickSucceedsFromEnterAndNeverNeedsATick )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::ClickCommand{
                                         .button = grab::input::primaryButton,
                                         }
        };

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Instant );

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        // Press, release, one flush — and no waypoints, so there is nothing
        // for a tick to advance.
        ASSERT_EQ( seat.events().size(), clickEventCount );
        EXPECT_TRUE( state.waypoints.empty() );

        const auto buttons = events_of_kind( seat, SeatEvent::Kind::Button );
        ASSERT_EQ( buttons.size(), 2U );
        EXPECT_TRUE( buttons.front().pressed );
        EXPECT_FALSE( buttons.back().pressed );

        grab::kernel::sequence::exit( command, state, context );
        EXPECT_EQ( seat.events().size(), clickEventCount );
    }

    TEST( ExecuteInstant,
          WarpPlacesThePointerWithoutInterpolating )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::WarpCommand{
                                        .to = grab::geometry::Point{ .x = warpX, .y = warpY },
                                        }
        };

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Instant );

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        const auto moves = events_of_kind( seat, SeatEvent::Kind::Move );
        ASSERT_EQ( moves.size(), 1U );
        EXPECT_EQ( moves.front().x, static_cast<std::int16_t>( warpX ) );
        EXPECT_EQ( moves.front().y, static_cast<std::int16_t>( warpY ) );
    }

    TEST( ExecuteInstant,
          ScrollEmitsAPressAndReleaseOfTheWheelButtonPerNotch )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::ScrollCommand{
                                          .dx = 0,
                                          .dy = scrollDown,
                                          }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        const auto buttons = events_of_kind( seat, SeatEvent::Kind::Button );
        ASSERT_EQ( buttons.size(), scrollNotchEventCount );
        for( const auto& event : buttons )
        {
            EXPECT_EQ(
                event.button,
                grab::input::button_code( grab::input::PointerButton::WheelDown )
            );
        }
    }

    TEST( ExecuteInstant,
          TypeGoesThroughTheSeatTextCapability )
    {
        const std::string         typedText = "hi";

        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::TypeCommand{
                                        .text = typedText,
                                        }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );
        ASSERT_EQ( seat.typed().size(), 1U );
        EXPECT_EQ( seat.typed().front(), typedText );
    }

    // ── the Opaque class ─────────────────────────────────────

    constexpr auto captureTakes = std::chrono::milliseconds{ 80 };
    constexpr auto captureEarly = std::chrono::milliseconds{ 1 };

    TEST( ExecuteCapture,
          StaysRunningUntilTheWorkActuallyFinishes )
    {
        const std::string         captureFile = "a.png";

        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::CaptureCommand{
                                           .output  = captureFile,
                                           .locator = {},
                                           }
        };

        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Opaque );
        ASSERT_TRUE( grab::kernel::sequence::is_blocking( command ) );

        set_clock( context, seat, fabricated_start() );

        // Declared at nothing, so it must NOT report Success from enter(): an
        // Opaque step that claimed to be instantaneous would fire its
        // successors while it was still writing its buffer.
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );
        EXPECT_EQ( seat.capture_output(), captureFile );

        set_clock( context, seat, fabricated_start() + captureEarly );
        EXPECT_EQ( grab::kernel::sequence::tick( command, state, context, context.now ),
                   Status::Running );

        seat.finish_capture();
        set_clock( context, seat, fabricated_start() + captureTakes );
        EXPECT_EQ( grab::kernel::sequence::tick( command, state, context, context.now ),
                   Status::Success );
    }

    TEST( ExecuteCapture,
          RejectsATargetThatIsNeitherExactlyAPathNorExactlyALocator )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::CaptureCommand{
                                           .output  = {},
                                           .locator = {},
                                           }
        };

        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Failure );
    }

    // ── seat capabilities ────────────────────────────────────

    TEST( ExecuteSeat,
          AKeyCommandOnAPointerOnlySeatFailsInsteadOfSilentlyDoingNothing )
    {
        // The recorder is the pointer and overlay halves of the contract and no
        // more — no keys by name — which is exactly the seat this has to be
        // honest about.
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::KeyDownCommand{
                                           .key = chordModifier,
                                           }
        };

        seat.set_now( fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Failure );
        EXPECT_TRUE( seat.events().empty() );

        grab::kernel::sequence::interrupt( command, state, context );
        EXPECT_TRUE( seat.events().empty() );
    }

    TEST( ExecuteSeat,
          APointerOnlySeatStillRunsAPacedDrag )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command = paced_drag();

        seat.set_now( fabricated_start() );
        context.now = fabricated_start();
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );

        const auto lastDue =
            fabricated_start() +
            ( dragDwell * static_cast<std::chrono::milliseconds::rep>( dragSteps ) );
        seat.set_now( lastDue );
        context.now = lastDue;
        EXPECT_EQ( grab::kernel::sequence::tick( command, state, context, context.now ),
                   Status::Success );
        EXPECT_FALSE( state.held );
    }

    TEST( ExecuteTick,
          AStepThatWasNeverEnteredCannotBeAdvanced )
    {
        SequenceSeat              seat;
        ExecContext<SequenceSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::WaitCommand{
                                        .duration = zeroMilliseconds,
                                        }
        };

        EXPECT_EQ(
            grab::kernel::sequence::tick( command, state, context, fabricated_start() ),
            Status::Failure
        );

        // exit() on a step that never entered is a no-op, not a stray release.
        grab::kernel::sequence::exit( command, state, context );
        EXPECT_TRUE( seat.events().empty() );
    }

    // ── overlay steps ────────────────────────────────────────

    // The pointer half and nothing else. RecordingSeat cannot play this part
    // any more — it grew the overlay capability — and the missing-capability
    // path needs a seat that genuinely lacks the concept rather than one that
    // merely fails.
    class OverlaylessSeat final
    {
        public:

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t,
                                   std::int16_t )
            {
                ++calls_;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t,
                    bool )
            {
                ++calls_;
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            flush()
            {
                ++calls_;
                return {};
            }

            [[nodiscard]]
            std::size_t
            calls() const noexcept
            {
                return calls_;
            }

        private:

            std::size_t calls_{ 0U };
    };

    // Every overlay call refuses, and records that it was asked. This is what
    // proves the grab arm marks the capture held BEFORE the round trip: a grab
    // that reports failure may still have been granted, and the unwind has to
    // treat a capture that MIGHT be held like one that is.
    class FailingOverlaySeat final
    {
        public:

            [[nodiscard]]
            grab::Result<void>
            overlay_add( std::string_view,
                         const grab::overlay::Shape& )
            {
                return refuse( OverlayEvent::Op::Add );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_update( std::string_view,
                            const grab::overlay::Shape& )
            {
                return refuse( OverlayEvent::Op::Update );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_remove( std::string_view )
            {
                return refuse( OverlayEvent::Op::Remove );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_clear()
            {
                return refuse( OverlayEvent::Op::Clear );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_grab()
            {
                return refuse( OverlayEvent::Op::Grab );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_release()
            {
                return refuse( OverlayEvent::Op::Release );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_attach( std::string_view,
                            std::optional<grab::geometry::Point> )
            {
                return refuse( OverlayEvent::Op::Attach );
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_detach( std::string_view )
            {
                return refuse( OverlayEvent::Op::Detach );
            }

            [[nodiscard]]
            const std::vector<OverlayEvent::Op>&
            attempts() const noexcept
            {
                return attempts_;
            }

        private:

            [[nodiscard]]
            grab::Result<void>
            refuse( OverlayEvent::Op op )
            {
                attempts_.push_back( op );
                return grab::fail( grab::ErrorCode::ProviderFailed,
                                   "the overlay refused this mutation" );
            }

            std::vector<OverlayEvent::Op> attempts_{};
    };

    static_assert( grab::kernel::sequence::OverlaySeat<grab::testing::RecordingSeat> );
    static_assert( grab::kernel::sequence::OverlaySeat<FailingOverlaySeat> );
    static_assert( !grab::kernel::sequence::OverlaySeat<OverlaylessSeat> );
    static_assert( grab::kernel::sequence::PointerSeat<OverlaylessSeat> );

    constexpr double       overlayEllipseCenterX = 300.0;
    constexpr double       overlayEllipseCenterY = 200.0;
    constexpr double       overlayEllipseRadius  = 48.0;

    constexpr double       overlayRectX          = 100.0;
    constexpr double       overlayRectY          = 110.0;
    constexpr double       overlayRectWidth      = 90.0;
    constexpr double       overlayRectHeight     = 70.0;

    constexpr double       overlayPolygonX       = 12.0;
    constexpr double       overlayPolygonY       = 34.0;
    constexpr double       overlayPolygonSpan    = 22.0;

    constexpr double       overlayPathX          = 7.0;
    constexpr double       overlayPathY          = 9.0;
    constexpr double       overlayPathEndX       = 17.0;
    constexpr double       overlayPathEndY       = 19.0;

    constexpr std::uint8_t overlayStrokeRed      = 78U;
    constexpr std::uint8_t overlayStrokeGreen    = 206U;
    constexpr std::uint8_t overlayStrokeBlue     = 169U;
    constexpr std::uint8_t overlayStrokeAlpha    = 255U;
    constexpr float        overlayStrokeWidth    = 3.0F;
    constexpr std::int32_t overlayZ              = 10;

    // Negative on one axis on purpose: a square picked up by its corner is held
    // by that corner, so the offset is a real vector and not a magnitude.
    constexpr std::int32_t overlayOffsetX     = -12;
    constexpr std::int32_t overlayOffsetY     = 5;

    constexpr std::size_t  overlayOpCount     = 8U;
    constexpr std::size_t  oneCall            = 1U;
    constexpr std::size_t  twoCalls           = 2U;
    constexpr std::size_t  threeCalls         = 3U;
    constexpr std::size_t  fourCalls          = 4U;
    constexpr std::size_t  noCalls            = 0U;

    const std::string      overlayHandle      = "c01";
    const std::string      overlayOtherHandle = "r02";
    const std::string      overlayNoHandle;

    [[nodiscard]]
    grab::SpacePoint
    space_point( double x,
                 double y )
    {
        return grab::SpacePoint{ .x = x, .y = y, .space = {} };
    }

    // The alternative's index rather than a literal, so a new Geometry
    // alternative cannot silently renumber these assertions into passing
    // against the wrong figure.
    template<typename FigureT>
    [[nodiscard]]
    std::size_t
    geometry_index()
    {
        return grab::overlay::Geometry{ FigureT{} }.index();
    }

    [[nodiscard]]
    grab::overlay::Shape
    ellipse_shape()
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Ellipse{
                                       .center =
                        space_point( overlayEllipseCenterX, overlayEllipseCenterY ),
                                       .radius_x = overlayEllipseRadius,
                                       .radius_y = overlayEllipseRadius
                },
            .stroke =
                grab::overlay::StrokeStyle{
                                       .color =
                        grab::overlay::Color{
                            .r = overlayStrokeRed,
                            .g = overlayStrokeGreen,
                            .b = overlayStrokeBlue,
                            .a = overlayStrokeAlpha
                        }, .width_px = overlayStrokeWidth
                },
            .fill      = std::nullopt,
            .lifetime  = grab::overlay::Persistent{},
            .band      = grab::overlay::Band::Annotation,
            .z         = overlayZ,
            .animation = std::nullopt,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    rect_shape()
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Rect{
                    .bounds =
                        grab::SpaceRect{
                            .x     = overlayRectX,
                            .y     = overlayRectY,
                            .w     = overlayRectWidth,
                            .h     = overlayRectHeight,
                            .space = {}
                        }
                },
            // A shape with neither stroke nor fill is legal and invisible,
            // which is a useful thing to be able to say.
            .stroke    = std::nullopt,
            .fill      = std::nullopt,
            .lifetime  = grab::overlay::Persistent{},
            .band      = grab::overlay::Band::Annotation,
            .z         = overlayZ,
            .animation = std::nullopt,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    polygon_shape()
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Polygon{
                    .points =
                        { space_point( overlayPolygonX, overlayPolygonY ),
                          space_point( overlayPolygonX + overlayPolygonSpan,
                                       overlayPolygonY ),
                          space_point( overlayPolygonX,
                                       overlayPolygonY + overlayPolygonSpan ) }
                },
            .stroke    = std::nullopt,
            .fill      = std::nullopt,
            .lifetime  = grab::overlay::Persistent{},
            .band      = grab::overlay::Band::Trail,
            .z         = overlayZ,
            .animation = std::nullopt,
        };
    }

    [[nodiscard]]
    grab::overlay::Shape
    path_shape()
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Path{
                                    .commands =
                        { grab::overlay::MoveTo{
                              .point = space_point( overlayPathX, overlayPathY )
                          },
                          grab::overlay::LineTo{
                              .point = space_point( overlayPathEndX, overlayPathEndY )
                          } },
                                    .closed = false
                },
            .stroke    = std::nullopt,
            .fill      = std::nullopt,
            .lifetime  = grab::overlay::Persistent{},
            .band      = grab::overlay::Band::Annotation,
            .z         = overlayZ,
            .animation = std::nullopt,
        };
    }

    // All eight, so a ninth overlay op cannot be added without this list — and
    // therefore the missing-capability assertion — noticing.
    [[nodiscard]]
    std::vector<grab::sequence::Command>
    every_overlay_command()
    {
        std::vector<grab::sequence::Command> commands;
        commands.emplace_back( grab::sequence::OverlayAddCommand{
            .handle = overlayHandle,
            .shape  = ellipse_shape(),
        } );
        commands.emplace_back( grab::sequence::OverlayUpdateCommand{
            .handle = overlayHandle,
            .shape  = rect_shape(),
        } );
        commands.emplace_back( grab::sequence::OverlayRemoveCommand{
            .handle = overlayHandle,
        } );
        commands.emplace_back( grab::sequence::OverlayClearCommand{} );
        commands.emplace_back( grab::sequence::OverlayGrabCommand{} );
        commands.emplace_back( grab::sequence::OverlayReleaseCommand{} );
        commands.emplace_back( grab::sequence::OverlayAttachCommand{
            .handle = overlayHandle,
            .offset = std::nullopt,
        } );
        commands.emplace_back( grab::sequence::OverlayDetachCommand{
            .handle = overlayHandle,
        } );
        return commands;
    }

    [[nodiscard]]
    std::size_t
    count_overlay_ops( const grab::testing::RecordingSeat& seat,
                       OverlayEvent::Op                    op )
    {
        std::size_t counted = 0U;
        for( const auto& event : seat.overlay_events() )
        {
            if( event.op == op )
            {
                ++counted;
            }
        }
        return counted;
    }

    // The descriptor row is the authority on idempotence, so the test reads it
    // rather than restating it.
    [[nodiscard]]
    bool
    descriptor_is_idempotent( grab::CommandKind kind )
    {
        for( const auto& descriptor : grab::list_commands() )
        {
            if( descriptor.kind == kind )
            {
                return descriptor.idempotent;
            }
        }
        return false;
    }

    TEST( ExecuteOverlay,
          AddForwardsTheHandleTheGeometryAlternativeAndItsPrincipalPoint )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayAddCommand{
                                              .handle = overlayHandle,
                                              .shape  = ellipse_shape(),
                                              }
        };

        // Instant and not blocking: a reactor-thread mutation averages 0.02 ms,
        // and the frame is paid by the player's per-tick flush.
        ASSERT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Instant );
        ASSERT_FALSE( grab::kernel::sequence::is_blocking( command ) );

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        ASSERT_EQ( seat.overlay_events().size(), oneCall );
        const auto& recorded = seat.overlay_events().front();
        EXPECT_EQ( recorded.op, OverlayEvent::Op::Add );
        EXPECT_EQ( recorded.handle, overlayHandle );
        EXPECT_EQ( recorded.geometry, geometry_index<grab::overlay::Ellipse>() );
        EXPECT_DOUBLE_EQ( recorded.x, overlayEllipseCenterX );
        EXPECT_DOUBLE_EQ( recorded.y, overlayEllipseCenterY );
        EXPECT_FALSE( recorded.offset.has_value() );
        EXPECT_EQ( recorded.at, fabricated_start() );

        // Instant means enter() is the whole step: there is no Running state to
        // invent, nothing for a tick to advance, and nothing to unwind.
        EXPECT_EQ(
            grab::kernel::sequence::tick( command, state, context, fabricated_start() ),
            Status::Success
        );
        grab::kernel::sequence::interrupt( command, state, context );
        EXPECT_EQ( seat.overlay_events().size(), oneCall );
        EXPECT_FALSE( state.overlay_grab_held );
        EXPECT_FALSE( state.document_hold );
        EXPECT_FALSE( state.held );
    }

    TEST( ExecuteOverlay,
          AddWithNoHandleIsFireAndForgetRatherThanAnError )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayAddCommand{
                                              .handle = overlayNoHandle,
                                              .shape  = rect_shape(),
                                              }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        ASSERT_EQ( seat.overlay_events().size(), oneCall );
        const auto& recorded = seat.overlay_events().front();
        EXPECT_TRUE( recorded.handle.empty() );
        EXPECT_EQ( recorded.geometry, geometry_index<grab::overlay::Rect>() );
        EXPECT_DOUBLE_EQ( recorded.x, overlayRectX );
        EXPECT_DOUBLE_EQ( recorded.y, overlayRectY );
    }

    TEST( ExecuteOverlay,
          UpdateReplacesTheShapeUnderTheHandleItWasGiven )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayUpdateCommand{
                                                 .handle = overlayOtherHandle,
                                                 .shape  = polygon_shape(),
                                                 }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        ASSERT_EQ( seat.overlay_events().size(), oneCall );
        const auto& recorded = seat.overlay_events().front();
        EXPECT_EQ( recorded.op, OverlayEvent::Op::Update );
        EXPECT_EQ( recorded.handle, overlayOtherHandle );
        EXPECT_EQ( recorded.geometry, geometry_index<grab::overlay::Polygon>() );
        EXPECT_DOUBLE_EQ( recorded.x, overlayPolygonX );
        EXPECT_DOUBLE_EQ( recorded.y, overlayPolygonY );
    }

    TEST( ExecuteOverlay,
          APathShapeArrivesWithItsFirstNamedPoint )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayAddCommand{
                                              .handle = overlayHandle,
                                              .shape  = path_shape(),
                                              }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        ASSERT_EQ( seat.overlay_events().size(), oneCall );
        const auto& recorded = seat.overlay_events().front();
        EXPECT_EQ( recorded.geometry, geometry_index<grab::overlay::Path>() );
        EXPECT_DOUBLE_EQ( recorded.x, overlayPathX );
        EXPECT_DOUBLE_EQ( recorded.y, overlayPathY );
    }

    TEST( ExecuteOverlay,
          RemoveAndDetachCarryTheirHandleAndClearCarriesNone )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        set_clock( context, seat, fabricated_start() );

        CommandState                  remove_state;
        const grab::sequence::Command remove{
            grab::sequence::OverlayRemoveCommand{
                                                 .handle = overlayHandle,
                                                 }
        };
        EXPECT_EQ( grab::kernel::sequence::enter( remove, remove_state, context ),
                   Status::Success );

        CommandState                  detach_state;
        const grab::sequence::Command detach{
            grab::sequence::OverlayDetachCommand{
                                                 .handle = overlayOtherHandle,
                                                 }
        };
        EXPECT_EQ( grab::kernel::sequence::enter( detach, detach_state, context ),
                   Status::Success );

        CommandState                  clear_state;
        const grab::sequence::Command clear{ grab::sequence::OverlayClearCommand{} };
        EXPECT_EQ( grab::kernel::sequence::enter( clear, clear_state, context ),
                   Status::Success );

        ASSERT_EQ( seat.overlay_events().size(), threeCalls );
        EXPECT_EQ( seat.overlay_events().at( 0U ).op, OverlayEvent::Op::Remove );
        EXPECT_EQ( seat.overlay_events().at( 0U ).handle, overlayHandle );
        EXPECT_EQ( seat.overlay_events().at( 0U ).geometry, OverlayEvent::noGeometry );
        EXPECT_EQ( seat.overlay_events().at( 1U ).op, OverlayEvent::Op::Detach );
        EXPECT_EQ( seat.overlay_events().at( 1U ).handle, overlayOtherHandle );
        EXPECT_EQ( seat.overlay_events().at( 2U ).op, OverlayEvent::Op::Clear );
        EXPECT_TRUE( seat.overlay_events().at( 2U ).handle.empty() );
    }

    TEST( ExecuteOverlay,
          AttachForwardsTheOffsetItWasGiven )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayAttachCommand{
                                                 .handle = overlayHandle,
                                                 .offset =
                    grab::geometry::Point{ .x = overlayOffsetX, .y = overlayOffsetY },
                                                 }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        ASSERT_EQ( seat.overlay_events().size(), oneCall );
        const auto& recorded = seat.overlay_events().front();
        EXPECT_EQ( recorded.op, OverlayEvent::Op::Attach );
        EXPECT_EQ( recorded.handle, overlayHandle );
        ASSERT_TRUE( recorded.offset.has_value() );
        EXPECT_EQ( recorded.offset->x, overlayOffsetX );
        EXPECT_EQ( recorded.offset->y, overlayOffsetY );
    }

    TEST( ExecuteOverlay,
          AttachWithNoOffsetForwardsAbsenceRatherThanZero )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayAttachCommand{
                                                 .handle = overlayHandle,
                                                 .offset = std::nullopt,
                                                 }
        };

        set_clock( context, seat, fabricated_start() );
        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );

        // Absent means "keep the gap the shape already has", which only the
        // seat can compute. Substituting (0,0) here would teleport every
        // picked-up shape onto the pointer's hotspot.
        ASSERT_EQ( seat.overlay_events().size(), oneCall );
        EXPECT_FALSE( seat.overlay_events().front().offset.has_value() );
    }

    TEST( ExecuteOverlay,
          EveryOpThatNeedsAHandleRejectsAnEmptyOneBeforeReachingTheSeat )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        set_clock( context, seat, fabricated_start() );

        std::vector<grab::sequence::Command> unnamed;
        unnamed.emplace_back( grab::sequence::OverlayUpdateCommand{
            .handle = overlayNoHandle,
            .shape  = rect_shape(),
        } );
        unnamed.emplace_back( grab::sequence::OverlayRemoveCommand{
            .handle = overlayNoHandle,
        } );
        unnamed.emplace_back( grab::sequence::OverlayAttachCommand{
            .handle = overlayNoHandle,
            .offset = std::nullopt,
        } );
        unnamed.emplace_back( grab::sequence::OverlayDetachCommand{
            .handle = overlayNoHandle,
        } );
        ASSERT_EQ( unnamed.size(), fourCalls );

        for( const auto& command : unnamed )
        {
            CommandState state;
            EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                       Status::Failure );
        }

        // Only overlay.add may omit a handle, so none of these reached the seat
        // at all — a step that silently targeted nothing is the failure mode
        // this rules out.
        EXPECT_TRUE( seat.overlay_events().empty() );
    }

    TEST( ExecuteOverlay,
          ASeatWithoutTheOverlayCapabilityFailsEveryOverlayStepCleanly )
    {
        OverlaylessSeat              seat;
        ExecContext<OverlaylessSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };

        const auto commands = every_overlay_command();
        ASSERT_EQ( commands.size(), overlayOpCount );

        for( const auto& command : commands )
        {
            CommandState state;
            EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                       Status::Failure );
            // Including overlay.grab: the unwind of a step that never grabbed
            // anything must not crash and must not release a capture nobody
            // owns.
            grab::kernel::sequence::interrupt( command, state, context );
            EXPECT_FALSE( state.overlay_grab_held );
        }

        // Failure is reported, not simulated: the pointer half of this seat was
        // never touched either.
        EXPECT_EQ( seat.calls(), noCalls );
    }

    TEST( ExecuteOverlay,
          TheFourIdempotentOpsRunTwiceWithTheSameResult )
    {
        EXPECT_TRUE( descriptor_is_idempotent( grab::CommandKind::OverlayRemove ) );
        EXPECT_TRUE( descriptor_is_idempotent( grab::CommandKind::OverlayClear ) );
        EXPECT_TRUE( descriptor_is_idempotent( grab::CommandKind::OverlayRelease ) );
        EXPECT_TRUE( descriptor_is_idempotent( grab::CommandKind::OverlayDetach ) );
        // The other four are Never: a second add draws a second shape.
        EXPECT_FALSE( descriptor_is_idempotent( grab::CommandKind::OverlayAdd ) );
        EXPECT_FALSE( descriptor_is_idempotent( grab::CommandKind::OverlayUpdate ) );
        EXPECT_FALSE( descriptor_is_idempotent( grab::CommandKind::OverlayAttach ) );
        EXPECT_FALSE( descriptor_is_idempotent( grab::CommandKind::OverlayGrab ) );

        std::vector<grab::sequence::Command> repeatable;
        repeatable.emplace_back( grab::sequence::OverlayRemoveCommand{
            .handle = overlayHandle,
        } );
        repeatable.emplace_back( grab::sequence::OverlayClearCommand{} );
        repeatable.emplace_back( grab::sequence::OverlayReleaseCommand{} );
        repeatable.emplace_back( grab::sequence::OverlayDetachCommand{
            .handle = overlayHandle,
        } );
        ASSERT_EQ( repeatable.size(), fourCalls );

        for( const auto& command : repeatable )
        {
            grab::testing::RecordingSeat              seat;
            ExecContext<grab::testing::RecordingSeat> context{
                .seat   = &seat,
                .timers = nullptr,
                .now    = fabricated_start(),
            };
            set_clock( context, seat, fabricated_start() );

            CommandState first;
            EXPECT_EQ( grab::kernel::sequence::enter( command, first, context ),
                       Status::Success );
            grab::kernel::sequence::exit( command, first, context );

            // Removing an already-removed handle, clearing an empty scene,
            // releasing an ungrabbed pointer and detaching an unattached shape
            // all converge on the same end state, so the second run succeeds
            // exactly as the first did.
            CommandState second;
            EXPECT_EQ( grab::kernel::sequence::enter( command, second, context ),
                       Status::Success );
            grab::kernel::sequence::exit( command, second, context );
            EXPECT_EQ( seat.overlay_events().size(), twoCalls );
        }
    }

    // ── overlay.grab: the capture that must never outlive its run ──

    TEST( ExecuteOverlayGrab,
          AnInterruptBetweenTheGrabAndItsReleaseStillReleasesTheCapture )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{ grab::sequence::OverlayGrabCommand{} };

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Success );
        EXPECT_TRUE( state.overlay_grab_held );

        // Deliberately NOT document_hold and NOT held: play_command.hpp
        // classifies a step carrying either as ErrorCode::PossiblyCommitted,
        // which is the right verdict for a half-committed button press and a
        // lie about a grab, which commits no input at all.
        EXPECT_FALSE( state.document_hold );
        EXPECT_FALSE( state.held );

        const auto interruptedAt = fabricated_start() + waitDuration;
        set_clock( context, seat, interruptedAt );
        grab::kernel::sequence::interrupt( command, state, context );

        // A pointer grab that outlives its owner freezes the entire desktop,
        // and recovery needs another X client or a VT switch.
        ASSERT_EQ( seat.overlay_events().size(), twoCalls );
        EXPECT_EQ( seat.overlay_events().front().op, OverlayEvent::Op::Grab );
        EXPECT_EQ( seat.overlay_events().back().op, OverlayEvent::Op::Release );
        EXPECT_EQ( seat.overlay_events().back().at, interruptedAt );
        EXPECT_FALSE( state.overlay_grab_held );

        // Unwinding twice must not issue a second release.
        grab::kernel::sequence::interrupt( command, state, context );
        EXPECT_EQ( seat.overlay_events().size(), twoCalls );
    }

    TEST( ExecuteOverlayGrab,
          ACompletedGrabKeepsTheCaptureAndItsReleaseStepLiftsItExactlyOnce )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        set_clock( context, seat, fabricated_start() );

        CommandState                  grab_state;
        const grab::sequence::Command grabbed{ grab::sequence::OverlayGrabCommand{} };
        ASSERT_EQ( grab::kernel::sequence::enter( grabbed, grab_state, context ),
                   Status::Success );

        // The pair is overlay.grab → overlay.release, and the first step's own
        // exit() must not break it — the same rule that keeps a key_down from
        // lifting the modifier a chord is built on.
        grab::kernel::sequence::exit( grabbed, grab_state, context );
        EXPECT_EQ( seat.overlay_events().size(), oneCall );
        EXPECT_TRUE( grab_state.overlay_grab_held );

        CommandState                  release_state;
        const grab::sequence::Command released{
            grab::sequence::OverlayReleaseCommand{}
        };
        ASSERT_EQ( grab::kernel::sequence::enter( released, release_state, context ),
                   Status::Success );
        grab::kernel::sequence::exit( released, release_state, context );

        // Exactly one release: the normal path must not double-release.
        EXPECT_EQ( seat.overlay_events().size(), twoCalls );
        EXPECT_EQ( count_overlay_ops( seat, OverlayEvent::Op::Release ), oneCall );
        EXPECT_FALSE( release_state.overlay_grab_held );
    }

    TEST( ExecuteOverlayGrab,
          AnUnwindAfterACompletedPairReleasesOnceMoreBecauseThatIsTheSafeSide )
    {
        grab::testing::RecordingSeat              seat;
        ExecContext<grab::testing::RecordingSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        set_clock( context, seat, fabricated_start() );

        CommandState                  grab_state;
        const grab::sequence::Command grabbed{ grab::sequence::OverlayGrabCommand{} };
        ASSERT_EQ( grab::kernel::sequence::enter( grabbed, grab_state, context ),
                   Status::Success );

        CommandState                  release_state;
        const grab::sequence::Command released{
            grab::sequence::OverlayReleaseCommand{}
        };
        ASSERT_EQ( grab::kernel::sequence::enter( released, release_state, context ),
                   Status::Success );

        // Pinned because it is a design consequence rather than an accident:
        // the two steps hold two different CommandStates, so the release step
        // cannot clear the grab step's flag, and a later unwind of the grab
        // step issues one further release. overlay.release is Idempotent — it
        // is a no-op that succeeds — while the alternative, clearing the flag
        // on the success path, would strand the capture in every run whose
        // release step never got to run. The asymmetry decides.
        grab::kernel::sequence::interrupt( grabbed, grab_state, context );
        EXPECT_EQ( count_overlay_ops( seat, OverlayEvent::Op::Release ), twoCalls );
        EXPECT_FALSE( grab_state.overlay_grab_held );
    }

    TEST( ExecuteOverlayGrab,
          AGrabWhoseSeatCallFailedIsStillReleasedOnTheUnwind )
    {
        FailingOverlaySeat              seat;
        ExecContext<FailingOverlaySeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{ grab::sequence::OverlayGrabCommand{} };

        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Failure );

        // Marked before the round trip: the server may have handed this process
        // the pointer whatever the call reported, and a capture that MIGHT be
        // held has to be released like one that is.
        EXPECT_TRUE( state.overlay_grab_held );

        grab::kernel::sequence::interrupt( command, state, context );
        ASSERT_EQ( seat.attempts().size(), twoCalls );
        EXPECT_EQ( seat.attempts().front(), OverlayEvent::Op::Grab );
        EXPECT_EQ( seat.attempts().back(), OverlayEvent::Op::Release );

        // Cleared before the attempt, so a seat that keeps refusing cannot turn
        // exit() into an unbounded retry. The failure is logged instead.
        EXPECT_FALSE( state.overlay_grab_held );
        grab::kernel::sequence::interrupt( command, state, context );
        EXPECT_EQ( seat.attempts().size(), twoCalls );
    }

    TEST( ExecuteOverlay,
          ASeatThatRefusesAMutationIsReportedAsFailureRatherThanSuccess )
    {
        FailingOverlaySeat              seat;
        ExecContext<FailingOverlaySeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::OverlayAddCommand{
                                              .handle = overlayHandle,
                                              .shape  = ellipse_shape(),
                                              }
        };

        EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Failure );
        ASSERT_EQ( seat.attempts().size(), oneCall );
        EXPECT_EQ( seat.attempts().front(), OverlayEvent::Op::Add );
    }

    // ── Introspection ────────────────────────────────────────
    //
    // Everything below asserts STRUCTURED VALUES — tally names, tally counts,
    // CommandState::worst_overshoot, the capability table — and never log
    // prose. A log line is a diagnostic for a human; a test that pattern-matched
    // one would make the wording the contract, which CLAUDE.md §5 forbids.
    //
    // Time is fabricated, as everywhere else in this file, so "the pacing error
    // is exactly three milliseconds" is a statement about the executor's
    // arithmetic rather than about the machine the suite happened to run on.

    namespace exec_detail = grab::kernel::sequence::detail;

    using Instrument      = grab::diag::Instrument;
    using Phase           = exec_detail::Phase;

    // The verbatim tally names. Pinned here because the CLI unit prints them
    // and the Player unit tallies alongside them: a silent rename would leave
    // two reports that no longer add up.
    constexpr std::string_view enterMoveTally  = "enter:input.move";
    constexpr std::string_view tickMoveTally   = "tick:input.move";
    constexpr std::string_view exitMoveTally   = "exit:input.move";
    constexpr std::string_view enterDragTally  = "enter:input.drag";
    constexpr std::string_view tickDragTally   = "tick:input.drag";
    constexpr std::string_view seatMoveTally   = "seat.move_pointer_absolute";
    constexpr std::string_view seatFlushTally  = "seat.flush";
    constexpr std::string_view seatButtonTally = "seat.button";

    static_assert( exec_detail::tally_name( Phase::Enter,
                                            grab::CommandKind::Move ) ==
                   enterMoveTally );
    static_assert( exec_detail::tally_name( Phase::Tick,
                                            grab::CommandKind::Move ) == tickMoveTally );
    static_assert( exec_detail::tally_name( Phase::Exit,
                                            grab::CommandKind::Move ) == exitMoveTally );
    static_assert( exec_detail::tally_name( Phase::Enter,
                                            grab::CommandKind::Drag ) ==
                   enterDragTally );
    static_assert( exec_detail::tallies::seatMove == seatMoveTally );
    static_assert( exec_detail::tallies::seatFlush == seatFlushTally );
    static_assert( exec_detail::tallies::seatButton == seatButtonTally );

    // Instrument compares the POINTER before the contents, so every call for a
    // given phase and kind must hand it the same object. A table that returned
    // a fresh view each time would still be correct and would silently cost a
    // string compare on the hottest path in the executor.
    static_assert( exec_detail::tally_name( Phase::Tick,
                                            grab::CommandKind::Drag )
                       .data() == exec_detail::tally_name( Phase::Tick,
                                                           grab::CommandKind::Drag )
                                      .data() );

    // The compile gate is the one thing that makes "it compiles out" checkable
    // rather than asserted, so it is asserted against log::enabled directly
    // instead of being restated.
    static_assert( grab::diag::Measure<exec_detail::measureLevel>::enabled ==
                   grab::log::enabled( exec_detail::measureLevel ) );
    static_assert( exec_detail::Sample<exec_detail::measureLevel>::enabled ==
                   grab::diag::Measure<exec_detail::measureLevel>::enabled );

    // Compiled out, a Measure keeps its instrument pointer and its name but not
    // its clock reading — the time_point member vanishes under
    // [[no_unique_address]] — and a Sample around one is empty outright. The
    // `enabled ||` prefix is what lets this assertion hold at every ceiling:
    // above the gate there is nothing to claim.
    constexpr std::size_t measureFieldBytes =
        sizeof( Instrument* ) + sizeof( std::string_view );
    static_assert( grab::diag::Measure<exec_detail::measureLevel>::enabled ||
                   sizeof( grab::diag::Measure<exec_detail::measureLevel> ) <=
                   measureFieldBytes );
    static_assert( exec_detail::Sample<exec_detail::measureLevel>::enabled ||
                   sizeof( exec_detail::Sample<exec_detail::measureLevel> ) == 1U );

    // A context that never names an instrument is the default, and the whole
    // point of defaulting it is that every existing caller keeps compiling.
    static_assert( ExecContext<SequenceSeat>{}.instrument == nullptr );

    // The concept each command needs, asserted at compile time so a rejection
    // that "names the missing concept" is a checkable claim rather than a
    // sentence in a log.
    static_assert( exec_detail::required_capability( grab::CommandKind::Move ) ==
                   exec_detail::pointerSeatConcept );
    static_assert( exec_detail::required_capability( grab::CommandKind::Drag ) ==
                   exec_detail::pointerSeatConcept );
    static_assert( exec_detail::required_capability( grab::CommandKind::KeyDown ) ==
                   exec_detail::keyboardSeatConcept );
    static_assert( exec_detail::required_capability( grab::CommandKind::Type ) ==
                   exec_detail::textSeatConcept );
    static_assert( exec_detail::required_capability( grab::CommandKind::Capture ) ==
                   exec_detail::capturingSeatConcept );
    static_assert( exec_detail::required_capability( grab::CommandKind::OverlayGrab ) ==
                   exec_detail::overlaySeatConcept );
    // time.wait touches no seat at all, so it can never be rejected for one.
    static_assert( exec_detail::required_capability( grab::CommandKind::Wait ).empty() );

    [[nodiscard]]
    const grab::diag::Tally*
    tally_for( const Instrument& instrument,
               std::string_view  name )
    {
        for( const auto& tally : instrument.tallies() )
        {
            if( tally.name == name )
            {
                return &tally;
            }
        }
        return nullptr;
    }

    [[nodiscard]]
    std::uint64_t
    calls_of( const Instrument& instrument,
              std::string_view  name )
    {
        const auto* const tally = tally_for( instrument, name );
        return tally == nullptr ? 0U : tally->calls;
    }

    // Below measureLevel there is no timer to construct and therefore nothing
    // to tally. Asserting the counts unconditionally would turn "the
    // instrumentation compiles out" — the exact property a GRAB_LOG_LEVEL=off
    // build exists to prove — into a suite failure, so the expectation follows
    // the gate instead of contradicting it.
    constexpr bool timingCompiledIn =
        exec_detail::Sample<exec_detail::measureLevel>::enabled;

    [[nodiscard]]
    std::uint64_t
    expected_calls( std::uint64_t when_compiled_in ) noexcept
    {
        return timingCompiledIn ? when_compiled_in : 0U;
    }

    // One paced move, walked exactly on its deadlines. Shared by the tally test
    // and the on-time half of the pacing test so neither can drift from the
    // other's idea of what "on time" means.
    void
    walk_a_paced_move( SequenceSeat&              seat,
                       ExecContext<SequenceSeat>& context,
                       CommandState&              state,
                       std::chrono::milliseconds  lateBy )
    {
        const grab::sequence::Command command{
            grab::sequence::MoveCommand{
                                        .from    = moveFrom,
                                        .to      = moveTo,
                                        .options = paced_options( moveSteps, moveDwell ),
                                        }
        };

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );

        for( std::size_t index = 0U; index < expectedMoveX.size(); ++index )
        {
            const auto ordinal =
                static_cast<std::chrono::milliseconds::rep>( index + 1U );
            set_clock( context,
                       seat,
                       fabricated_start() + ( moveDwell * ordinal ) + lateBy );
            ( void )grab::kernel::sequence::tick( command, state, context, context.now );
        }

        grab::kernel::sequence::exit( command, state, context );
    }

    constexpr auto          onTime     = std::chrono::milliseconds{ 0 };
    constexpr auto          moveLateBy = std::chrono::milliseconds{ 3 };
    constexpr std::uint64_t oneTally   = 1U;
    // Four ticks, one per waypoint.
    constexpr std::uint64_t moveTickCount = 4U;
    // The origin warp plus one per waypoint, each of which is a move and a
    // flush: the flush is not optional, or the waypoint sits in the output
    // buffer and lands whenever it next drains.
    constexpr std::uint64_t moveSeatCalls = 5U;

    TEST( ExecuteInstrument,
          APacedMoveTalliesEveryPhaseOnceAndEverySeatRoundTripItMade )
    {
        SequenceSeat              seat;
        Instrument                instrument;
        ExecContext<SequenceSeat> context{
            .seat       = &seat,
            .timers     = nullptr,
            .now        = fabricated_start(),
            .instrument = &instrument,
        };
        CommandState state;

        walk_a_paced_move( seat, context, state, onTime );

        // The walk itself: one waypoint per interpolation step, which is what
        // every tally below is counted against.
        EXPECT_EQ( state.emitted, static_cast<std::size_t>( moveSteps ) );
        EXPECT_EQ( state.waypoints.size(), static_cast<std::size_t>( moveSteps ) );

        // One tally per phase. enter() and exit() run once each; tick() ran
        // once per waypoint.
        EXPECT_EQ( calls_of( instrument, enterMoveTally ), expected_calls( oneTally ) );
        EXPECT_EQ( calls_of( instrument, tickMoveTally ),
                   expected_calls( moveTickCount ) );
        EXPECT_EQ( calls_of( instrument, exitMoveTally ), expected_calls( oneTally ) );

        // ...and the seat round trips underneath them, tallied apart. This is
        // the split the exercise exists for: a slow move and a slow server are
        // different bugs with the same symptom.
        EXPECT_EQ( calls_of( instrument, seatMoveTally ),
                   expected_calls( moveSeatCalls ) );
        EXPECT_EQ( calls_of( instrument, seatFlushTally ),
                   expected_calls( moveSeatCalls ) );

        // A move presses nothing, so no button round trip was made and no slot
        // was claimed for one.
        EXPECT_EQ( tally_for( instrument, seatButtonTally ), nullptr );

        // Every name this run produced fitted, so the report is a full
        // accounting rather than a partial one.
        EXPECT_FALSE( instrument.overflowed() );
        if constexpr( timingCompiledIn )
        {
            EXPECT_GT( instrument.total(), std::chrono::nanoseconds::zero() );
        }
    }

    TEST( ExecuteInstrument,
          ThePacingErrorIsZeroOnTheDeadlineAndExactlyTheLatenessWhenTheTickIsLate )
    {
        {
            SequenceSeat              seat;
            ExecContext<SequenceSeat> context{
                .seat       = &seat,
                .timers     = nullptr,
                .now        = fabricated_start(),
                .instrument = nullptr,
            };
            CommandState state;
            walk_a_paced_move( seat, context, state, onTime );

            // Fabricated exactly on each deadline, so the executor owes zero.
            // A number that could not be zero would not be measuring anything.
            EXPECT_EQ( state.worst_overshoot, std::chrono::nanoseconds::zero() );
        }
        {
            SequenceSeat              seat;
            ExecContext<SequenceSeat> context{
                .seat       = &seat,
                .timers     = nullptr,
                .now        = fabricated_start(),
                .instrument = nullptr,
            };
            CommandState state;
            walk_a_paced_move( seat, context, state, moveLateBy );

            // Every tick was late by the same amount, so the worst single
            // overshoot is exactly that — not a multiple of it. The walk never
            // catches up by firing the remainder early, and it never
            // accumulates the error either.
            EXPECT_EQ( state.worst_overshoot, moveLateBy );
            EXPECT_EQ( state.emitted, static_cast<std::size_t>( moveSteps ) );
        }
    }

    TEST( ExecuteInstrument,
          ANullInstrumentChangesNothingAboutWhatTheRunDoes )
    {
        SequenceSeat              measured_seat;
        Instrument                instrument;
        ExecContext<SequenceSeat> measured_context{
            .seat       = &measured_seat,
            .timers     = nullptr,
            .now        = fabricated_start(),
            .instrument = &instrument,
        };
        CommandState measured_state;
        walk_a_paced_move( measured_seat, measured_context, measured_state, onTime );

        // The same run with the field left off entirely — which is how every
        // caller that predates the instrument spells it.
        SequenceSeat              plain_seat;
        ExecContext<SequenceSeat> plain_context{
            .seat   = &plain_seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };
        CommandState plain_state;
        walk_a_paced_move( plain_seat, plain_context, plain_state, onTime );

        ASSERT_EQ( plain_context.instrument, nullptr );
        EXPECT_EQ( plain_state.emitted, measured_state.emitted );
        EXPECT_EQ( plain_state.worst_overshoot, measured_state.worst_overshoot );
        ASSERT_EQ( plain_seat.events().size(), measured_seat.events().size() );
        for( std::size_t index = 0U; index < plain_seat.events().size(); ++index )
        {
            const auto& plain    = plain_seat.events().at( index );
            const auto& measured = measured_seat.events().at( index );
            EXPECT_EQ( plain.kind, measured.kind );
            EXPECT_EQ( plain.x, measured.x );
            EXPECT_EQ( plain.y, measured.y );
            EXPECT_EQ( plain.at, measured.at );
        }
    }

    constexpr std::uint64_t dragButtonCalls = 2U;

    TEST( ExecuteInstrument,
          ADragTalliesItsButtonRoundTripsApartFromItsWaypoints )
    {
        SequenceSeat              seat;
        Instrument                instrument;
        ExecContext<SequenceSeat> context{
            .seat       = &seat,
            .timers     = nullptr,
            .now        = fabricated_start(),
            .instrument = &instrument,
        };
        CommandState                  state;
        const grab::sequence::Command command{
            grab::sequence::DragCommand{
                                        .from    = moveFrom,
                                        .to      = moveTo,
                                        .button  = grab::input::primaryButton,
                                        .options = paced_options( moveSteps, moveDwell ),
                                        }
        };

        set_clock( context, seat, fabricated_start() );
        ASSERT_EQ( grab::kernel::sequence::enter( command, state, context ),
                   Status::Running );
        for( std::size_t index = 0U; index < expectedMoveX.size(); ++index )
        {
            const auto ordinal =
                static_cast<std::chrono::milliseconds::rep>( index + 1U );
            set_clock( context, seat, fabricated_start() + ( moveDwell * ordinal ) );
            ( void )grab::kernel::sequence::tick( command, state, context, context.now );
        }
        grab::kernel::sequence::exit( command, state, context );

        EXPECT_EQ( calls_of( instrument, enterDragTally ), expected_calls( oneTally ) );
        EXPECT_EQ( calls_of( instrument, tickDragTally ),
                   expected_calls( moveTickCount ) );
        // Press at enter(), release on the tick that finished the walk. Both go
        // through the same seat verb and neither is charged to a waypoint.
        EXPECT_EQ( calls_of( instrument, seatButtonTally ),
                   expected_calls( dragButtonCalls ) );
        EXPECT_EQ( calls_of( instrument, seatMoveTally ),
                   expected_calls( moveSeatCalls ) );
        EXPECT_FALSE( instrument.overflowed() );
    }

    // ── Capability rejections ────────────────────────────────

    // Named payloads for the three commands a pointer-only seat must refuse.
    // Their contents never reach anything — the point is that they do not.
    const std::string rejectedKeyName     = "Control_L";
    const std::string rejectedText        = "hi";
    const std::string rejectedCaptureFile = "a.png";

    TEST( ExecuteCapability,
          EveryOverlayStepAgainstAPointerOnlySeatIsRejectedForOverlaySeat )
    {
        OverlaylessSeat              seat;
        ExecContext<OverlaylessSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };

        const auto commands = every_overlay_command();
        ASSERT_EQ( commands.size(), overlayOpCount );

        for( const auto& command : commands )
        {
            CommandState state;
            EXPECT_EQ( grab::kernel::sequence::enter( command, state, context ),
                       Status::Failure );
            // The rejection is not merely "failed": the concept the seat did
            // not satisfy is derivable from the command alone, which is what
            // makes the logged record actionable and this assertion possible
            // without reading it.
            const auto missing =
                exec_detail::required_capability( grab::sequence::kind_of( command ) );
            EXPECT_EQ( missing, exec_detail::overlaySeatConcept );
            EXPECT_FALSE( missing.empty() );
        }

        EXPECT_EQ( seat.calls(), noCalls );
    }

    TEST( ExecuteCapability,
          TheKeyboardTextAndCaptureHalvesNameTheirOwnConceptsRatherThanOverlays )
    {
        OverlaylessSeat              seat;
        ExecContext<OverlaylessSeat> context{
            .seat   = &seat,
            .timers = nullptr,
            .now    = fabricated_start(),
        };

        struct Expectation
        {
                grab::sequence::Command command;
                std::string_view        concept_name;
        };

        std::vector<Expectation> expectations;
        expectations.push_back( Expectation{
            .command      = grab::sequence::KeyDownCommand{ .key = rejectedKeyName },
            .concept_name = exec_detail::keyboardSeatConcept,
        } );
        expectations.push_back( Expectation{
            .command      = grab::sequence::TypeCommand{ .text = rejectedText },
            .concept_name = exec_detail::textSeatConcept,
        } );
        expectations.push_back( Expectation{
            .command =
                grab::sequence::CaptureCommand{
                                               .output  = rejectedCaptureFile,
                                               .locator = {},
                                               },
            .concept_name = exec_detail::capturingSeatConcept,
        } );

        for( const auto& expected : expectations )
        {
            CommandState state;
            EXPECT_EQ( grab::kernel::sequence::enter( expected.command, state, context ),
                       Status::Failure );
            EXPECT_EQ( exec_detail::required_capability(
                           grab::sequence::kind_of( expected.command )
                       ),
                       expected.concept_name );
        }

        // A rejected step never reached the pointer half either: a command the
        // seat cannot run does nothing at all rather than doing half of it.
        EXPECT_EQ( seat.calls(), noCalls );
    }

}    // namespace
