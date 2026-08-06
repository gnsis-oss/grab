#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The enter/tick/exit triple every command runs through, ported in shape from
// bead::Node.
//
// THE SEAT IS A TEMPLATE PARAMETER, NOT A grab::Input*. grab::Input is a pimpl
// whose only construction path is open() against a live X display, so a
// context holding one would make every display-free scheduler test
// unwritable — and the display-free tests are the whole point of a
// caller-driven pump. Anything exposing move_pointer_absolute, button and
// flush qualifies, which includes tests/support/recording_seat.hpp.
//
// exit() ALWAYS runs, including on interrupt, and is what releases a held
// button or key. The caller owns what it presses: a button left down survives
// the process and reaches the next application.
//
// THE GOVERNING RULE: NO DURATION DEFAULTS TO ZERO. A command reports Running
// until it is actually done, so a five-second wait and an eighty-millisecond
// screenshot are the same mechanism — "Running until not Running". Only the
// Instant class returns Success from enter(), and even there the pump measures
// the call, because an XTest round trip and a flush are not free. There is no
// path here that assumes an operation is instantaneous:
//
//   Instant   warp, click, click_at, press, release, scroll, type, key,
//             key_down, key_up   — enter() does the work and returns Success.
//   Timed     wait, move, follow, drag  — enter() returns Running; tick()
//             returns Running until the deadline, emitting paced waypoints.
//   Opaque    capture — enter() returns Running and tick() polls; it finishes
//             when it finishes, however long that is.
//
// NOTHING HERE SLEEPS. Pacing is expressed as deadlines compared against the
// `now` the caller passes in, which is what lets a test walk a 128 ms drag in
// microseconds of real time and what keeps src/ free of raw sleeps.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/input/waypoints.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::scheduling
{

    class TimerThread;

}

namespace grab::kernel::sequence
{

    // What a seat must expose to move a pointer. This is the low half of the
    // input stack — grab::input::Seat and X11InputSeat both satisfy it, and so
    // does grab::testing::RecordingSeat, which is the point.
    template<typename SeatT>
    concept PointerSeat = requires( SeatT&       seat,
                                    std::int16_t coordinate,
                                    std::uint8_t code,
                                    bool         pressed ) {
        {
            seat.move_pointer_absolute( coordinate, coordinate )
        } -> std::same_as<grab::Result<void>>;
        {
            seat.button( code, pressed )
        } -> std::same_as<grab::Result<void>>;
        {
            seat.flush()
        } -> std::same_as<grab::Result<void>>;
    };

    // Reading back where the pointer actually is. Required only by a move that
    // declares no origin, because "wherever the pointer already is" is
    // knowable at run time and nowhere else — and never inferred from
    // previously injected motion, which the physical mouse can override.
    template<typename SeatT>
    concept LocatingSeat = requires( SeatT& seat ) {
        {
            seat.pointer_position()
        } -> std::same_as<grab::Result<grab::geometry::Point>>;
    };

    // Keys BY NAME, not by keycode. The name-to-keycode step needs a Keymap,
    // which lives above grab::input::Seat, so this capability is deliberately a
    // rung higher than the pointer half: an adapter over grab::Input (which
    // already has key_down/key_up by name) satisfies it directly.
    template<typename SeatT>
    concept KeyboardSeat = requires( SeatT& seat, std::string_view name, bool pressed ) {
        {
            seat.key_by_name( name, pressed )
        } -> std::same_as<grab::Result<void>>;
    };

    template<typename SeatT>
    concept TextSeat = requires( SeatT& seat, std::string_view utf8 ) {
        {
            seat.type_text( utf8 )
        } -> std::same_as<grab::Result<void>>;
    };

    // The Opaque capability, and the reason it is split in two: a capture that
    // reported its result from one synchronous call would force this layer to
    // pretend the work took no time. begin_capture starts it, poll_capture
    // reports nullopt until it is done.
    template<typename SeatT>
    concept CapturingSeat =
        requires( SeatT& seat, std::string_view output, std::string_view locator ) {
            {
                seat.begin_capture( output, locator )
            } -> std::same_as<grab::Result<void>>;
            {
                seat.poll_capture()
            } -> std::same_as<std::optional<grab::Result<void>>>;
        };

    template<typename SeatT>
    struct ExecContext
    {
            SeatT*                                 seat{ nullptr };
            grab::kernel::scheduling::TimerThread* timers{ nullptr };
            std::chrono::steady_clock::time_point  now{};
    };

    // Per-step scratch, owned by whoever is running the step. It is separate
    // from the Command because the Command belongs to the immutable document
    // and a run must not write into it.
    struct CommandState
    {
            std::chrono::steady_clock::time_point started{};
            std::chrono::steady_clock::time_point deadline{};
            // How many waypoints (or characters, or notches) have been emitted
            // so far; a multi-tick command resumes from here.
            std::size_t                           emitted{ 0U };
            bool                                  entered{ false };
            // A button or key is down and exit() must release it.
            //
            // This is an IMPLICIT hold — one the command created for its own
            // purposes, like the button a drag presses before it walks. It is
            // released unconditionally, because no later step knows about it.
            bool                                  held{ false };
            // An EXPLICIT hold, owned by the document rather than by the
            // command: input.press and input.key_down exist precisely to leave
            // a button or key down for a later step to release, which is the
            // only way to spell a chord (Ctrl+C is key_down, key, key_up).
            // Releasing it on the success path would make chords impossible,
            // so exit() releases it only when `interrupted` says no later step
            // will ever run.
            bool                                  document_hold{ false };
            // Set by the runner before exit() when the run is being UNWOUND —
            // interrupt, abort, or a skip over a running step — rather than
            // completing normally. Use interrupt() below and it is set for
            // you; a runner that forgets it strands the key a chord pressed.
            bool                                  interrupted{ false };
            // The waypoints a Timed motion command walks, resolved ONCE at
            // enter(). They cannot be recomputed in tick(): a move with no
            // declared origin starts wherever the pointer was when the step
            // entered, and that is gone by the next tick.
            std::vector<grab::geometry::Point>    waypoints{};
    };

    // Where a command's duration comes from, and whether its body has to run
    // off the timing thread. Both read the descriptor table rather than
    // carrying a second copy of the policy.
    [[nodiscard]]
    grab::sequence::TimingClass
    timing_class_of( const grab::sequence::Command& command ) noexcept;

    [[nodiscard]]
    bool
    is_blocking( const grab::sequence::Command& command ) noexcept;

    namespace detail
    {

        // The seat speaks int16 because the X wire format does. A coordinate
        // outside that range is a load error, not a wrap-around.
        inline constexpr std::int32_t coordinateMinimum =
            std::numeric_limits<std::int16_t>::min();
        inline constexpr std::int32_t coordinateMaximum =
            std::numeric_limits<std::int16_t>::max();

        // A curve needs a start and an end before it names a path at all.
        inline constexpr std::size_t minimumCurveControlPoints = 2U;

        // Everything below this line is ordinary code that does not depend on
        // the seat, so it lives in execute.cpp: grab is a library plus a CLI,
        // not a header-only project, and a header included by the player, the
        // tests and every future runner should carry the templates and no more.

        [[nodiscard]]
        bool
        in_seat_range( grab::geometry::Point point ) noexcept;

        void
        note_failure( grab::CommandKind  kind,
                      std::string_view   stage,
                      const grab::Error& error );

        // A seat that cannot do what the step asks is a configuration fault,
        // not a transient one: say which capability was missing, because the
        // alternative is a step that silently does nothing.
        [[nodiscard]]
        grab::sequence::Status
        note_unavailable( grab::CommandKind kind,
                          std::string_view  capability );

        [[nodiscard]]
        grab::sequence::Status
        note_invalid( grab::CommandKind kind,
                      std::string_view  stage,
                      std::string       message );

        void
        note_release( grab::CommandKind kind,
                      std::string_view  what,
                      bool              succeeded );

        [[nodiscard]]
        grab::Result<void>
        validate_options( const grab::input::DragOptions& options );

        [[nodiscard]]
        grab::Result<void>
        validate_reachable( const std::vector<grab::geometry::Point>& points );

        // One pointer position, plus the flush that makes it leave the
        // process. The flush is not optional: without it the waypoint sits in
        // the connection's output buffer and lands whenever the buffer next
        // drains, which is exactly the pacing this layer exists to provide.
        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        warp_to( SeatT&                seat,
                 grab::geometry::Point point )
        {
            if( !in_seat_range( point ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "pointer coordinate is outside int16 range" );
            }
            auto moved =
                seat.move_pointer_absolute( static_cast<std::int16_t>( point.x ),
                                            static_cast<std::int16_t>( point.y ) );
            if( !moved )
            {
                return moved;
            }
            return seat.flush();
        }

        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        drive_button( SeatT&       seat,
                      std::uint8_t code,
                      bool         pressed )
        {
            auto driven = seat.button( code, pressed );
            if( !driven )
            {
                return driven;
            }
            return seat.flush();
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::Result<void>
        drive_key( SeatT&           seat,
                   std::string_view name,
                   bool             pressed )
        requires KeyboardSeat<SeatT>
        {
            auto driven = seat.key_by_name( name, pressed );
            if( !driven )
            {
                return driven;
            }
            return seat.flush();
        }

        // Where a move starts. An absent origin is not zero: it means
        // "wherever the pointer already is", which only the seat can answer.
        template<typename SeatT>
        [[nodiscard]]
        grab::Result<grab::geometry::Point>
        resolve_origin( const std::optional<grab::geometry::Point>& declared,
                        SeatT&                                      seat )
        {
            if( declared.has_value() )
            {
                return *declared;
            }
            if constexpr( LocatingSeat<SeatT> )
            {
                return seat.pointer_position();
            }
            else
            {
                return grab::fail(
                    grab::ErrorCode::CapabilityUnavailable,
                    "a move with no declared origin needs a seat that reports the "
                    "pointer position"
                );
            }
        }

        // Emit every waypoint whose instant has arrived, and no more.
        //
        // Waypoint N (one-based) is due at started + N * step_dwell. Several
        // become due in one tick when the pump is late or when the dwell is
        // zero, and none does when it is early; neither case is special-cased,
        // because "how many are due" is a question about the clock rather than
        // about the caller's cadence. THE WALK NEVER CATCHES UP by firing the
        // remainder early — the deadline is absolute, so a late tick emits
        // exactly what is owed and nothing more.
        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::sequence::Status
        advance_walk( grab::CommandKind                     kind,
                      CommandState&                         state,
                      SeatT&                                seat,
                      std::chrono::milliseconds             dwell,
                      std::chrono::steady_clock::time_point now )
        {
            while( state.emitted < state.waypoints.size() )
            {
                const auto ordinal =
                    static_cast<std::chrono::milliseconds::rep>( state.emitted + 1U );
                if( now < state.started + ( dwell * ordinal ) )
                {
                    break;
                }

                const auto point = state.waypoints.at( state.emitted );
                auto       moved = warp_to( seat, point );
                if( !moved )
                {
                    note_failure( kind, "waypoint", moved.error() );
                    return grab::sequence::Status::Failure;
                }
                ++state.emitted;

                log::debug(
                    [kind, &state, point]( auto& event )
                    {
                        event.tag( log::tags::sequence )
                            .value( "command", grab::command_name( kind ) )
                            .value( "waypoint", state.emitted )
                            .value( "of", state.waypoints.size() )
                            .value( "x", point.x )
                            .value( "y", point.y );
                    }
                );
            }

            return state.emitted == state.waypoints.size()
                     ? grab::sequence::Status::Success
                     : grab::sequence::Status::Running;
        }

        // Shared preamble for the three motion commands: validate the
        // options, resolve and range-check the walk, put the pointer on the
        // path's first point, and set the deadline the walk will finish by.
        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::sequence::Status
        begin_walk( grab::CommandKind                    kind,
                    CommandState&                        state,
                    SeatT&                               seat,
                    const grab::input::DragOptions&      options,
                    std::optional<grab::geometry::Point> origin,
                    std::vector<grab::geometry::Point>   walk )
        {
            auto reachable = validate_reachable( walk );
            if( !reachable )
            {
                note_failure( kind, "waypoints", reachable.error() );
                return grab::sequence::Status::Failure;
            }

            if( origin.has_value() )
            {
                auto placed = warp_to( seat, *origin );
                if( !placed )
                {
                    note_failure( kind, "origin", placed.error() );
                    return grab::sequence::Status::Failure;
                }
            }

            state.waypoints = std::move( walk );
            state.deadline =
                state.started +
                ( options.step_dwell * static_cast<std::chrono::milliseconds::rep>(
                                           state.waypoints.size()
                                       ) );
            return grab::sequence::Status::Running;
        }

        // ---------------------------------------------------------------
        // enter(): one overload per alternative of the Command variant.
        //
        // There is deliberately NO generic fallback. A new alternative must
        // fail to compile here rather than quietly acquiring the behaviour of
        // whichever command happened to be nearest.
        // ---------------------------------------------------------------

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::WaitCommand& command,
                       CommandState&                      state,
                       ExecContext<SeatT>& )
        {
            if( command.duration < std::chrono::nanoseconds::zero() )
            {
                return note_invalid( grab::CommandKind::Wait,
                                     "enter",
                                     "wait duration must not be negative" );
            }
            // Running, never Success, even for a zero-length wait: the
            // deadline is what decides, and it is the same code path for 0 ns
            // and for five seconds.
            state.deadline = state.started + command.duration;
            return grab::sequence::Status::Running;
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::WarpCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Warp, "pointer" );
            }
            else
            {
                auto placed = warp_to( *context.seat, command.to );
                if( !placed )
                {
                    note_failure( grab::CommandKind::Warp, "enter", placed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::ClickCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Click, "pointer" );
            }
            else
            {
                auto pressed = context.seat->button( command.button, true );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::Click, "press", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                auto released = context.seat->button( command.button, false );
                if( !released )
                {
                    note_failure( grab::CommandKind::Click,
                                  "release",
                                  released.error() );
                    return grab::sequence::Status::Failure;
                }
                auto flushed = context.seat->flush();
                if( !flushed )
                {
                    note_failure( grab::CommandKind::Click, "flush", flushed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::ClickAtCommand& command,
                       CommandState&                         state,
                       ExecContext<SeatT>&                   context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::ClickAt, "pointer" );
            }
            else
            {
                auto placed = warp_to( *context.seat, command.at );
                if( !placed )
                {
                    note_failure( grab::CommandKind::ClickAt, "warp", placed.error() );
                    return grab::sequence::Status::Failure;
                }
                return enter_command(
                    grab::sequence::ClickCommand{ .button = command.button },
                    state,
                    context
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::PressCommand& command,
                       CommandState&                       state,
                       ExecContext<SeatT>&                 context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Press, "pointer" );
            }
            else
            {
                // Marked held BEFORE the call: XTest may have committed the
                // press even when the round trip reports failure, and a button
                // that might be down has to be released like one that is.
                state.document_hold = true;
                auto pressed = drive_button( *context.seat, command.button, true );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::Press, "enter", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::ReleaseCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Release, "pointer" );
            }
            else
            {
                auto released = drive_button( *context.seat, command.button, false );
                if( !released )
                {
                    note_failure( grab::CommandKind::Release,
                                  "enter",
                                  released.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::ScrollCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Scroll, "pointer" );
            }
            else
            {
                // A wheel notch is a press and a release of the direction's
                // button, exactly as Input::scroll spells it out. Negating in
                // a wider type keeps INT32_MIN a large scroll rather than
                // undefined behaviour.
                const auto magnitude = []( std::int32_t value ) noexcept -> std::int64_t
                {
                    const std::int64_t widened = value;
                    return widened < 0 ? -widened : widened;
                };

                const auto notches =
                    [&context]( std::int64_t               count,
                                grab::input::PointerButton wheel ) -> grab::Result<void>
                {
                    const auto code = grab::input::button_code( wheel );
                    for( std::int64_t sent = 0; sent < count; ++sent )
                    {
                        auto pressed = context.seat->button( code, true );
                        if( !pressed )
                        {
                            return pressed;
                        }
                        auto released = context.seat->button( code, false );
                        if( !released )
                        {
                            return released;
                        }
                    }
                    return {};
                };

                if( command.dy != 0 )
                {
                    auto sent =
                        notches( magnitude( command.dy ),
                                 command.dy > 0 ? grab::input::PointerButton::WheelDown
                                                : grab::input::PointerButton::WheelUp );
                    if( !sent )
                    {
                        note_failure( grab::CommandKind::Scroll,
                                      "vertical",
                                      sent.error() );
                        return grab::sequence::Status::Failure;
                    }
                }
                if( command.dx != 0 )
                {
                    auto sent = notches( magnitude( command.dx ),
                                         command.dx > 0
                                             ? grab::input::PointerButton::WheelRight
                                             : grab::input::PointerButton::WheelLeft );
                    if( !sent )
                    {
                        note_failure( grab::CommandKind::Scroll,
                                      "horizontal",
                                      sent.error() );
                        return grab::sequence::Status::Failure;
                    }
                }

                auto flushed = context.seat->flush();
                if( !flushed )
                {
                    note_failure( grab::CommandKind::Scroll, "flush", flushed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::TypeCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !TextSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Type, "text" );
            }
            else
            {
                auto typed = context.seat->type_text( command.text );
                if( !typed )
                {
                    note_failure( grab::CommandKind::Type, "enter", typed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::KeyCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !KeyboardSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Key, "keyboard" );
            }
            else
            {
                auto pressed = context.seat->key_by_name( command.key, true );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::Key, "press", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                auto released = drive_key( *context.seat, command.key, false );
                if( !released )
                {
                    note_failure( grab::CommandKind::Key, "release", released.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::KeyDownCommand& command,
                       CommandState&                         state,
                       ExecContext<SeatT>&                   context )
        {
            if constexpr( !KeyboardSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::KeyDown, "keyboard" );
            }
            else
            {
                // The document owns this hold — a matching input.key_up
                // releases it, which is the only way to spell Ctrl+C. Marked
                // before the call for the same reason as Press.
                state.document_hold = true;
                auto pressed        = drive_key( *context.seat, command.key, true );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::KeyDown, "enter", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::KeyUpCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !KeyboardSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::KeyUp, "keyboard" );
            }
            else
            {
                auto released = drive_key( *context.seat, command.key, false );
                if( !released )
                {
                    note_failure( grab::CommandKind::KeyUp, "enter", released.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::MoveCommand& command,
                       CommandState&                      state,
                       ExecContext<SeatT>&                context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Move, "pointer" );
            }
            else
            {
                auto options = validate_options( command.options );
                if( !options )
                {
                    note_failure( grab::CommandKind::Move, "options", options.error() );
                    return grab::sequence::Status::Failure;
                }

                auto origin = resolve_origin( command.from, *context.seat );
                if( !origin )
                {
                    note_failure( grab::CommandKind::Move, "origin", origin.error() );
                    return grab::sequence::Status::Failure;
                }

                return begin_walk( grab::CommandKind::Move,
                                   state,
                                   *context.seat,
                                   command.options,
                                   command.from,
                                   grab::kernel::input::waypoints( *origin,
                                                                   command.to,
                                                                   command.options ) );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::FollowCommand& command,
                       CommandState&                        state,
                       ExecContext<SeatT>&                  context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Follow, "pointer" );
            }
            else
            {
                auto options = validate_options( command.options );
                if( !options )
                {
                    note_failure( grab::CommandKind::Follow,
                                  "options",
                                  options.error() );
                    return grab::sequence::Status::Failure;
                }
                if( command.path.control.size() < minimumCurveControlPoints )
                {
                    return note_invalid(
                        grab::CommandKind::Follow,
                        "path",
                        "a follow path needs at least two control points"
                    );
                }

                // Sampling steps+1 points and dropping the first is the same
                // shape the cubic branch of waypoints() uses: the curve's own
                // start is where the walk begins, not a step of it.
                auto sampled = command.path.sample(
                    static_cast<std::size_t>( command.options.interpolation_steps ) + 1U
                );
                const auto origin = sampled.front();
                auto       walk   = std::vector<grab::geometry::Point>{
                    sampled.begin() + 1,
                    sampled.end()
                };

                return begin_walk( grab::CommandKind::Follow,
                                   state,
                                   *context.seat,
                                   command.options,
                                   origin,
                                   std::move( walk ) );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::DragCommand& command,
                       CommandState&                      state,
                       ExecContext<SeatT>&                context )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Drag, "pointer" );
            }
            else
            {
                auto options = validate_options( command.options );
                if( !options )
                {
                    note_failure( grab::CommandKind::Drag, "options", options.error() );
                    return grab::sequence::Status::Failure;
                }

                const auto begun =
                    begin_walk( grab::CommandKind::Drag,
                                state,
                                *context.seat,
                                command.options,
                                command.from,
                                grab::kernel::input::waypoints( command.from,
                                                                command.to,
                                                                command.options ) );
                if( begun != grab::sequence::Status::Running )
                {
                    return begun;
                }

                // Held BEFORE the press for the reason x11_drag_recipe returns
                // PossiblyCommitted: past this point the button may be down
                // whatever the call reports, and exit() has to be able to put
                // it back up.
                state.held   = true;
                auto pressed = drive_button( *context.seat, command.button, true );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::Drag, "press", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Running;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::CaptureCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !CapturingSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Capture, "capture" );
            }
            else
            {
                if( command.output.empty() == command.locator.empty() )
                {
                    return note_invalid(
                        grab::CommandKind::Capture,
                        "enter",
                        "capture takes exactly one of an output path and a locator"
                    );
                }

                auto begun =
                    context.seat->begin_capture( command.output, command.locator );
                if( !begun )
                {
                    note_failure( grab::CommandKind::Capture, "enter", begun.error() );
                    return grab::sequence::Status::Failure;
                }
                // Opaque: Running until it finishes, however long that is.
                // Nothing here declares a duration, and nothing assumes zero.
                return grab::sequence::Status::Running;
            }
        }

        // ---------------------------------------------------------------
        // tick(): only the Timed and Opaque classes have one. The generic
        // overload covers the Instant class, which finished at enter().
        // ---------------------------------------------------------------

        template<typename PayloadT,
                 typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        tick_command( const PayloadT&,
                      CommandState&,
                      ExecContext<SeatT>&,
                      std::chrono::steady_clock::time_point )
        {
            return grab::sequence::Status::Success;
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        tick_command( const grab::sequence::WaitCommand&,
                      CommandState& state,
                      ExecContext<SeatT>&,
                      std::chrono::steady_clock::time_point now )
        {
            return now >= state.deadline ? grab::sequence::Status::Success
                                         : grab::sequence::Status::Running;
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        tick_command( const grab::sequence::MoveCommand&    command,
                      CommandState&                         state,
                      ExecContext<SeatT>&                   context,
                      std::chrono::steady_clock::time_point now )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Move, "pointer" );
            }
            else
            {
                return advance_walk( grab::CommandKind::Move,
                                     state,
                                     *context.seat,
                                     command.options.step_dwell,
                                     now );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        tick_command( const grab::sequence::FollowCommand&  command,
                      CommandState&                         state,
                      ExecContext<SeatT>&                   context,
                      std::chrono::steady_clock::time_point now )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Follow, "pointer" );
            }
            else
            {
                return advance_walk( grab::CommandKind::Follow,
                                     state,
                                     *context.seat,
                                     command.options.step_dwell,
                                     now );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        tick_command( const grab::sequence::DragCommand&    command,
                      CommandState&                         state,
                      ExecContext<SeatT>&                   context,
                      std::chrono::steady_clock::time_point now )
        {
            if constexpr( !PointerSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Drag, "pointer" );
            }
            else
            {
                const auto walked = advance_walk( grab::CommandKind::Drag,
                                                  state,
                                                  *context.seat,
                                                  command.options.step_dwell,
                                                  now );
                if( walked != grab::sequence::Status::Success )
                {
                    return walked;
                }

                auto released = drive_button( *context.seat, command.button, false );
                if( !released )
                {
                    // held stays true, so exit() tries the release again on
                    // the unwind. A button left down outlives the process.
                    note_failure( grab::CommandKind::Drag, "release", released.error() );
                    return grab::sequence::Status::Failure;
                }
                state.held = false;
                return grab::sequence::Status::Success;
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        tick_command( const grab::sequence::CaptureCommand&,
                      CommandState&,
                      ExecContext<SeatT>& context,
                      std::chrono::steady_clock::time_point )
        {
            if constexpr( !CapturingSeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::Capture, "capture" );
            }
            else
            {
                auto polled = context.seat->poll_capture();
                if( !polled.has_value() )
                {
                    return grab::sequence::Status::Running;
                }
                if( !*polled )
                {
                    note_failure( grab::CommandKind::Capture, "tick", polled->error() );
                    return grab::sequence::Status::Failure;
                }
                return grab::sequence::Status::Success;
            }
        }

        // ---------------------------------------------------------------
        // exit(): the neutralization path. Only three commands can leave
        // anything down, and the generic overload says so for the rest.
        // ---------------------------------------------------------------

        template<typename PayloadT,
                 typename SeatT>
        void
        exit_command( const PayloadT&,
                      CommandState&,
                      ExecContext<SeatT>& )
        {
        }

        template<typename SeatT>
        void
        exit_command( const grab::sequence::DragCommand& command,
                      CommandState&                      state,
                      ExecContext<SeatT>&                context )
        {
            if constexpr( PointerSeat<SeatT> )
            {
                if( !state.held )
                {
                    return;
                }
                // Cleared before the attempt so a failing seat cannot turn
                // exit() into an unbounded retry; the failure is logged, which
                // is what NeutralizationOutcome reports upwards.
                state.held    = false;
                auto released = drive_button( *context.seat, command.button, false );
                note_release( grab::CommandKind::Drag, "button", released.has_value() );
                if( !released )
                {
                    note_failure( grab::CommandKind::Drag,
                                  "neutralize",
                                  released.error() );
                }
            }
        }

        template<typename SeatT>
        void
        exit_command( const grab::sequence::PressCommand& command,
                      CommandState&                       state,
                      ExecContext<SeatT>&                 context )
        {
            if constexpr( PointerSeat<SeatT> )
            {
                // The document owns this button: a later input.release is
                // supposed to lift it. Only an unwind proves that step will
                // never run.
                if( !state.document_hold || !state.interrupted )
                {
                    return;
                }
                state.document_hold = false;
                auto released = drive_button( *context.seat, command.button, false );
                note_release( grab::CommandKind::Press, "button", released.has_value() );
                if( !released )
                {
                    note_failure( grab::CommandKind::Press,
                                  "neutralize",
                                  released.error() );
                }
            }
        }

        template<typename SeatT>
        void
        exit_command( const grab::sequence::KeyDownCommand& command,
                      CommandState&                         state,
                      ExecContext<SeatT>&                   context )
        {
            if constexpr( KeyboardSeat<SeatT> )
            {
                if( !state.document_hold || !state.interrupted )
                {
                    return;
                }
                state.document_hold = false;
                auto released       = drive_key( *context.seat, command.key, false );
                note_release( grab::CommandKind::KeyDown, "key", released.has_value() );
                if( !released )
                {
                    note_failure( grab::CommandKind::KeyDown,
                                  "neutralize",
                                  released.error() );
                }
            }
        }

    }    // namespace detail

    template<typename SeatT>
    [[nodiscard]]
    grab::sequence::Status
    enter( const grab::sequence::Command& command,
           CommandState&                  state,
           ExecContext<SeatT>&            context )
    {
        state.entered  = true;
        state.started  = context.now;
        state.deadline = context.now;
        state.emitted  = 0U;
        state.waypoints.clear();

        const auto status = std::visit(
            [&state, &context]( const auto& payload )
            {
                return detail::enter_command( payload, state, context );
            },
            command
        );

        log::verbose(
            [&command, status]( auto& event )
            {
                const auto kind = grab::sequence::kind_of( command );
                event.tag( log::tags::sequence )
                    .value( "enter", grab::command_name( kind ) )
                    .value( "timing",
                            grab::sequence::timing_class_name(
                                grab::timing_class_of( kind )
                            ) )
                    .value( "status", grab::sequence::status_name( status ) );
            }
        );
        return status;
    }

    template<typename SeatT>
    [[nodiscard]]
    grab::sequence::Status
    tick( const grab::sequence::Command&        command,
          CommandState&                         state,
          ExecContext<SeatT>&                   context,
          std::chrono::steady_clock::time_point now )
    {
        // A step that was never entered has nothing in flight to advance;
        // reporting Success would retire work that never happened.
        if( !state.entered )
        {
            return grab::sequence::Status::Failure;
        }

        return std::visit(
            [&state, &context, now]( const auto& payload )
            {
                return detail::tick_command( payload, state, context, now );
            },
            command
        );
    }

    // ALWAYS RUNS, including on interrupt, and it is what releases anything
    // held. Set state.interrupted first — or call interrupt() below, which
    // does it for you — when the run is being unwound rather than completing,
    // because that is the only signal that no later step will release what
    // input.press and input.key_down deliberately left down.
    template<typename SeatT>
    void
    exit( const grab::sequence::Command& command,
          CommandState&                  state,
          ExecContext<SeatT>&            context )
    {
        if( !state.entered )
        {
            return;
        }

        std::visit(
            [&state, &context]( const auto& payload )
            {
                detail::exit_command( payload, state, context );
            },
            command
        );
    }

    // The unwind path, spelled once so no caller can forget the flag. A
    // held button or key that survives this call survives the process and
    // reaches the next application.
    template<typename SeatT>
    void
    interrupt( const grab::sequence::Command& command,
               CommandState&                  state,
               ExecContext<SeatT>&            context )
    {
        state.interrupted = true;
        exit( command, state, context );
    }

}    // namespace grab::kernel::sequence
