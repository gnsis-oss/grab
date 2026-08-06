// The enter/tick/exit triple, exercised with FABRICATED TIME and no X server.
//
// Every `now` below is invented, so a five-second wait and a 128 ms drag both
// run in microseconds of real time. That is not a shortcut: the pump is
// caller-driven precisely so the scheduler can be tested without a display,
// and nothing in this file can join the display-backed intermittents.
//
// The pointer half of the seat is grab::testing::RecordingSeat. The executor
// also asks for keys BY NAME, for text and for capture, which the Phase 0
// double does not provide, so SequenceSeat wraps the recorder and adds them —
// every pointer event asserted below still comes out of RecordingSeat itself.

#include "grab/command.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/curve.hpp"
#include "grab/geometry/point.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/execute.hpp"
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
    using SeatEvent = grab::testing::SeatEvent;

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
        // The Phase 0 double is the pointer half of the contract and no more,
        // which is exactly the seat this has to be honest about.
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

}    // namespace
