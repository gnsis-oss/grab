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
//             key_down, key_up, and all eight overlay ops — enter() does the
//             work and returns Success. A reactor-thread overlay mutation
//             averages 0.02 ms and add_many of 56 shapes costs 1.1 ms, so an
//             overlay step never owns a frame's latency: the frame is paid by
//             the flush() the player issues per tick.
//   Timed     wait, move, follow, drag  — enter() returns Running; tick()
//             returns Running until the deadline, emitting paced waypoints.
//   Opaque    capture — enter() returns Running and tick() polls; it finishes
//             when it finishes, however long that is.
//
// NOTHING HERE SLEEPS. Pacing is expressed as deadlines compared against the
// `now` the caller passes in, which is what lets a test walk a 128 ms drag in
// microseconds of real time and what keeps src/ free of raw sleeps.
//
// WHERE A RUN SPENDS ITS WALL CLOCK IS MEASURED HERE, because this is the only
// layer that touches the seat. Two independent facilities, and they answer
// different questions:
//
//   ExecContext::instrument   optional and null by default. When a caller
//                             supplies one, every enter/tick/exit is tallied by
//                             `<phase>:<command>` and every seat round trip by
//                             `seat.<verb>` — so "the drag was slow" resolves
//                             into "63 flushes at 0.4 ms" or "one 47 ms flush".
//                             A null instrument constructs no timer and reads
//                             no clock; see detail::Sample.
//
//   the log                   the pacing STORY, which a tally cannot carry: per
//                             waypoint, at debug under `input`, the deadline it
//                             was due at, the instant it fired, and the delta
//                             between them. CommandState::worst_overshoot keeps
//                             the worst of those deltas whether or not anything
//                             is logging, because a drag that should take 128 ms
//                             and takes 190 ms is a fact about the run and not a
//                             fact about the log level.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/overlay.hpp"
#include "grab/pointer_button.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/input/waypoints.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"
#include "kernel/support/step_diag.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

    // Drawing, and the pointer capture a modal drawing tool needs. A seat that
    // does not satisfy this fails every overlay step with a logged missing
    // capability, exactly as the pointer, keyboard, text and capture halves do
    // — a step that silently does nothing is the failure mode this avoids.
    //
    // THE SEAM IS BY HANDLE, NOT BY overlay::ShapeId. A document names a shape
    // before any scene exists, so the handle-to-ShapeId map is run state and
    // belongs to the seat; putting a ShapeId in the document would put run
    // state on an immutable value.
    //
    // overlay_grab() inherits everything Overlay::capture_pointer carries: arm
    // it when the tool becomes armed rather than at button-press, and THE
    // CALLER OWNS THE CAPTURE. A pointer grab that outlives its owner freezes
    // the whole desktop, which is a worse failure than a held button, so the
    // unwind path must reach overlay_release() however the run ends — see
    // CommandState::overlay_grab_held, which is what makes that happen.
    template<typename SeatT>
    concept OverlaySeat = requires( SeatT&                               seat,
                                    std::string_view                     handle,
                                    const grab::overlay::Shape&          shape,
                                    std::optional<grab::geometry::Point> offset ) {
        {
            seat.overlay_add( handle, shape )
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_update( handle, shape )
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_remove( handle )
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_clear()
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_grab()
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_release()
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_attach( handle, offset )
        } -> std::same_as<grab::Result<void>>;
        {
            seat.overlay_detach( handle )
        } -> std::same_as<grab::Result<void>>;
    };

    template<typename SeatT>
    struct ExecContext
    {
            SeatT*                                 seat{ nullptr };
            grab::kernel::scheduling::TimerThread* timers{ nullptr };
            std::chrono::steady_clock::time_point  now{};
            // Where per-command and per-seat-call timings are tallied. NULL BY
            // DEFAULT and null-safe everywhere: a caller that wants the numbers
            // owns an Instrument and points this at it, and one that does not
            // pays nothing — not even a clock read — because no timer is
            // constructed at all. Defaulted rather than required so every
            // existing caller and every existing test still compiles unchanged.
            //
            // The instrument is single-threaded by construction, so it belongs
            // to whoever drives the pump. A Blocking step's body runs on a
            // worker; do not hand that worker this pointer.
            grab::diag::Instrument*                instrument{ nullptr };
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
            // An explicit, document-owned hold of the POINTER CAPTURE, set by
            // overlay.grab and lifted by overlay.release. It behaves exactly
            // like document_hold — released by exit() only when `interrupted`
            // says no later step will do it — and it is a SEPARATE FLAG on
            // purpose, for two reasons:
            //
            //   1. play_command.hpp classifies any step with `held ||
            //      document_hold` as ErrorCode::PossiblyCommitted. That is the
            //      right verdict for a half-committed button press and the
            //      wrong one for a grab, which commits no input at all.
            //   2. What is at stake differs by an order of magnitude. A
            //      stranded button reaches the next application; a stranded
            //      pointer grab freezes the whole desktop, and recovery needs
            //      another X client or a VT switch.
            //
            // Set BEFORE the seat call, like the two above: past that point the
            // server may have handed this process the pointer whatever the
            // round trip reports, and a capture that MIGHT be held has to be
            // released like one that is. overlay.release is Idempotent, so a
            // redundant release costs nothing and a missed one costs the
            // desktop; the asymmetry decides.
            bool                                  overlay_grab_held{ false };
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
            // THE PACING ERROR: the worst single overshoot the walk has seen,
            // meaning how far past its own deadline the latest waypoint
            // actually fired. Never negative — a waypoint is never emitted
            // early, because the deadline is absolute.
            //
            // Computed UNCONDITIONALLY, and deliberately not gated on a log
            // level. It costs one subtraction and one comparison per waypoint
            // and reads no clock, since both `now` and the deadline are already
            // in hand; and it is a fact about the run rather than about the log
            // — a caller that wants to know whether step_dwell was honoured can
            // read it without turning logging on and re-running.
            std::chrono::nanoseconds              worst_overshoot{};
            // One-shot latch for the records that describe a COMPLETION: the
            // tick that satisfies a deadline, and the walk summary. A pump may
            // legally tick a step it has not retired yet, and a Timed command
            // answers Success on every tick past its deadline, so without this
            // the same completion is reported once per surplus tick.
            bool                                  reported{ false };
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
        inline constexpr std::size_t      minimumCurveControlPoints = 2U;

        // ── Timing ───────────────────────────────────────────────
        //
        // The compile gate for step timing. NOMINAL, not debug: an instrument
        // is opt-in already — a caller that passes one is asking for the
        // numbers — so gating the code out at the level people actually run at
        // would mean the facility is missing exactly when it is wanted, which
        // is the mistake `compileLevel` defaulting to nominal used to make.
        // GRAB_LOG_LEVEL=off still removes every clock read, which is what
        // makes "it compiles out" checkable rather than asserted.
        inline constexpr grab::log::Level measureLevel = grab::log::Level::Nominal;

        // diag::Measure writes into an Instrument REFERENCE, and ExecContext's
        // is a pointer that is usually null. Holding the Measure in an optional
        // is what keeps a null instrument genuinely free: the timer is never
        // constructed, so neither clock read happens. A hand-rolled
        // `if( into ) { start = now(); } … if( into ) { record( now() - start ); }`
        // would be two branches instead of one and could not be compiled out by
        // level, which is the whole reason Measure exists.
        template<grab::log::Level Level>
        class Sample final
        {
            public:

                static constexpr bool enabled = grab::diag::Measure<Level>::enabled;

                Sample( grab::diag::Instrument* into,
                        std::string_view        name ) noexcept
                {
                    if constexpr( enabled )
                    {
                        if( into != nullptr )
                        {
                            timer_.emplace( *into, name );
                        }
                    }
                    else
                    {
                        ( void )into;
                        ( void )name;
                    }
                }

                ~Sample()               = default;

                Sample( const Sample& ) = delete;
                Sample&
                operator=( const Sample& ) = delete;
                Sample( Sample&& )         = delete;
                Sample&
                operator=( Sample&& ) = delete;

            private:

                struct Idle
                {
                };

                using Store =
                    std::conditional_t<enabled,
                                       std::optional<grab::diag::Measure<Level>>,
                                       Idle>;

                [[no_unique_address]]
                Store timer_{};
        };

        // Time one seat round trip. Written as a wrapper around an invocable
        // rather than one function per verb because the seat's verbs differ in
        // arity and return type, and because the measured region has to be
        // exactly the call: a `return call();` destroys the Sample after the
        // result is computed and before the caller sees it.
        template<typename SeatT,
                 typename Call>
        [[nodiscard]]
        auto
        measured( ExecContext<SeatT>& context,
                  std::string_view    name,
                  Call&&              call ) -> decltype( call() )
        {
            const Sample<measureLevel> sample{ context.instrument, name };
            return call();
        }

        // ── Tally names ──────────────────────────────────────────
        //
        // Instrument keys on a string_view and compares the POINTER first, so a
        // name must be one constexpr object per (phase, command) — building
        // "enter:" + command_name() at the call site would allocate on the hot
        // path and defeat that compare. This table pays for the concatenation
        // once, at compile time, in rodata.
        //
        // A run therefore claims one slot per phase per command kind it
        // actually uses, plus one per seat verb. maxInstrumentSlots is 64,
        // which a realistic document is nowhere near; a document exercising
        // every kind would overflow, and Instrument::overflowed() says so.

        enum class Phase : std::uint8_t
        {
            Enter,
            Tick,
            Exit,
            Count,
        };

        inline constexpr std::size_t phaseCount =
            static_cast<std::size_t>( Phase::Count );
        inline constexpr std::size_t commandKindCount =
            static_cast<std::size_t>( grab::CommandKind::Count );

        // The longest command name is 16 characters and the longest prefix six.
        // build_tally_names() is consteval and subscripts with at(), so an
        // overrun is a compile error rather than a truncated name.
        inline constexpr std::size_t tallyNameCapacity = 32U;

        struct TallyName
        {
                std::array<char, tallyNameCapacity> text{};
                std::size_t                         size{};
        };

        using TallyNameTable =
            std::array<std::array<TallyName, commandKindCount>, phaseCount>;

        [[nodiscard]]
        consteval TallyNameTable
        build_tally_names()
        {
            constexpr std::array<std::string_view, phaseCount> prefixes{
                "enter:",
                "tick:",
                "exit:",
            };

            TallyNameTable names{};
            for( std::size_t phase = 0U; phase < phaseCount; ++phase )
            {
                for( std::size_t kind = 0U; kind < commandKindCount; ++kind )
                {
                    auto&       entry   = names.at( phase ).at( kind );
                    std::size_t written = 0U;
                    for( const char letter : prefixes.at( phase ) )
                    {
                        entry.text.at( written ) = letter;
                        ++written;
                    }
                    for( const char letter :
                         grab::command_name( static_cast<grab::CommandKind>( kind ) ) )
                    {
                        entry.text.at( written ) = letter;
                        ++written;
                    }
                    entry.size = written;
                }
            }
            return names;
        }

        inline constexpr TallyNameTable tallyNames = build_tally_names();

        // Stable for the life of the program, which is what the pointer-first
        // compare in Instrument::find_or_add relies on.
        [[nodiscard]]
        constexpr std::string_view
        tally_name( Phase             phase,
                    grab::CommandKind kind ) noexcept
        {
            const auto& entry = tallyNames[static_cast<std::size_t>( phase )]
                                          [static_cast<std::size_t>( kind )];
            return std::string_view{ entry.text.data(), entry.size };
        }

        // The seat half. Named after the seat verb rather than after the
        // command, because the point of measuring them separately is to tell a
        // command that is slow from a command that is waiting on the server.
        namespace tallies
        {

            inline constexpr std::string_view seatMove   = "seat.move_pointer_absolute";
            inline constexpr std::string_view seatButton = "seat.button";
            inline constexpr std::string_view seatFlush  = "seat.flush";
            inline constexpr std::string_view seatLocate = "seat.pointer_position";
            inline constexpr std::string_view seatKey    = "seat.key_by_name";
            inline constexpr std::string_view seatText   = "seat.type_text";
            inline constexpr std::string_view seatCaptureBegin  = "seat.begin_capture";
            inline constexpr std::string_view seatCapturePoll   = "seat.poll_capture";

            inline constexpr std::string_view seatOverlayAdd    = "seat.overlay_add";
            inline constexpr std::string_view seatOverlayUpdate = "seat.overlay_update";
            inline constexpr std::string_view seatOverlayRemove = "seat.overlay_remove";
            inline constexpr std::string_view seatOverlayClear  = "seat.overlay_clear";
            inline constexpr std::string_view seatOverlayGrab   = "seat.overlay_grab";
            inline constexpr std::string_view seatOverlayRelease =
                "seat.overlay_release";
            inline constexpr std::string_view seatOverlayAttach = "seat.overlay_attach";
            inline constexpr std::string_view seatOverlayDetach = "seat.overlay_detach";

        }    // namespace tallies

        // ── Capabilities ─────────────────────────────────────────
        //
        // The seat concept each command needs, spelled once so a rejection can
        // NAME the concept that was missing. "seat cannot run this command" is
        // useless to whoever has to fix the seat; "OverlaySeat" is a grep.
        //
        // This is also the structural half of that record: a test can assert
        // the mapping without reading log prose, which is what §5 asks for.

        inline constexpr std::string_view pointerSeatConcept   = "PointerSeat";
        inline constexpr std::string_view locatingSeatConcept  = "LocatingSeat";
        inline constexpr std::string_view keyboardSeatConcept  = "KeyboardSeat";
        inline constexpr std::string_view textSeatConcept      = "TextSeat";
        inline constexpr std::string_view capturingSeatConcept = "CapturingSeat";
        inline constexpr std::string_view overlaySeatConcept   = "OverlaySeat";
        inline constexpr std::string_view noSeatConcept        = "";

        // LocatingSeat is deliberately absent: input.move needs it only when it
        // declares no origin, and that rejection comes out of resolve_origin as
        // a CapabilityUnavailable error rather than through this table, because
        // it depends on the command's payload and not on its kind.
        [[nodiscard]]
        constexpr std::string_view
        required_capability( grab::CommandKind kind ) noexcept
        {
            switch( kind )
            {
                case grab::CommandKind::Warp :
                case grab::CommandKind::Click :
                case grab::CommandKind::ClickAt :
                case grab::CommandKind::Press :
                case grab::CommandKind::Release :
                case grab::CommandKind::Scroll :
                case grab::CommandKind::Move :
                case grab::CommandKind::Follow :
                case grab::CommandKind::Drag :
                    return pointerSeatConcept;
                case grab::CommandKind::Key :
                case grab::CommandKind::KeyDown :
                case grab::CommandKind::KeyUp :
                    return keyboardSeatConcept;
                case grab::CommandKind::Type :
                    return textSeatConcept;
                case grab::CommandKind::Capture :
                    return capturingSeatConcept;
                case grab::CommandKind::OverlayAdd :
                case grab::CommandKind::OverlayUpdate :
                case grab::CommandKind::OverlayRemove :
                case grab::CommandKind::OverlayClear :
                case grab::CommandKind::OverlayGrab :
                case grab::CommandKind::OverlayRelease :
                case grab::CommandKind::OverlayAttach :
                case grab::CommandKind::OverlayDetach :
                    return overlaySeatConcept;
                default :
                    // time.wait needs no seat at all, and the CLI verbs in this
                    // enum are not sequence steps.
                    return noSeatConcept;
            }
        }

        // How a hold came to exist, which decides when it may be lifted. The
        // two kinds are the design's, not an implementation detail: an implicit
        // hold belongs to the command that took it, a document hold belongs to
        // a later step that may never run.
        inline constexpr std::string_view implicitHold       = "implicit";
        inline constexpr std::string_view documentHold       = "document";
        inline constexpr std::string_view pointerCaptureHold = "pointer_capture";

        // Why exit() let a hold go.
        inline constexpr std::string_view releasedByCommand = "command_owned";
        inline constexpr std::string_view releasedByUnwind  = "interrupted";

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
        // not a transient one. The capability comes from required_capability()
        // rather than from the call site, so the record names the CONCEPT the
        // seat failed to satisfy — "OverlaySeat", not "overlay" — and there is
        // one place to change when a command's requirement changes.
        [[nodiscard]]
        grab::sequence::Status
        note_unavailable( grab::CommandKind kind );

        [[nodiscard]]
        grab::sequence::Status
        note_invalid( grab::CommandKind kind,
                      std::string_view  stage,
                      std::string       message );

        // A hold coming into existence, at verbose: which command took it and
        // which of the two kinds it is, because that is what decides whether
        // exit() may lift it.
        void
        note_hold_taken( grab::CommandKind kind,
                         std::string_view  hold );

        // ...and going away. `reason` separates the two paths §6.1 draws apart:
        // an implicit hold is released because the command that took it is done
        // with it, a document hold only because the unwind proved no later step
        // will ever do it. A record that says only "released" cannot tell a
        // chord that worked from a chord that was cut short.
        void
        note_release( grab::CommandKind kind,
                      std::string_view  what,
                      std::string_view  hold,
                      std::string_view  reason,
                      bool              succeeded );

        // A Timed command's PLAN, at verbose, recorded where the deadline is
        // set. `waypoints` is zero for a wait, which has a duration and no
        // steps. Without this a pacing error read later has no denominator.
        void
        note_deadline( grab::CommandKind         kind,
                       std::size_t               waypoints,
                       std::chrono::milliseconds dwell,
                       std::chrono::nanoseconds  span );

        // ...and the tick that satisfied it, with how far past it that tick
        // landed. A deadline nobody reports meeting is indistinguishable from
        // one nobody reached.
        void
        note_deadline_met( grab::CommandKind        kind,
                           std::chrono::nanoseconds overshoot );

        // One overlay mutation, reported. Every overlay op is Instant, so the
        // seat call IS the step: there is no deadline to set, no waypoint to
        // resume from, and nothing for a tick() to advance.
        [[nodiscard]]
        grab::sequence::Status
        settle_overlay( grab::CommandKind         kind,
                        std::string_view          handle,
                        const grab::Result<void>& outcome );

        // Every overlay op but add REQUIRES a handle: an overlay.add with an
        // empty one is fire-and-forget by design — drawable, never referenced
        // again — while an empty handle anywhere else names no shape at all.
        // The loader rejects that case too, but this layer is also reached by
        // splice() and by the CLI adapter, neither of which passes through it.
        [[nodiscard]]
        grab::sequence::Status
        note_missing_handle( grab::CommandKind kind );

        [[nodiscard]]
        grab::Result<void>
        validate_options( const grab::input::DragOptions& options );

        [[nodiscard]]
        grab::Result<void>
        validate_reachable( const std::vector<grab::geometry::Point>& points );

        // ── Seat round trips ─────────────────────────────────────
        //
        // THESE TAKE THE CONTEXT, NOT THE SEAT, and that is the only reason
        // their signatures changed: the instrument lives on the context, and a
        // round trip that cannot reach it cannot be timed. The seat is where a
        // sequence's wall clock actually goes — an XTest request plus a flush
        // is a server round trip — so measuring the command without measuring
        // the calls it makes answers "slow" with "yes".

        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        seat_move( ExecContext<SeatT>& context,
                   std::int16_t        x,
                   std::int16_t        y )
        {
            return measured( context,
                             tallies::seatMove,
                             [&context, x, y]
                             {
                                 return context.seat->move_pointer_absolute( x, y );
                             } );
        }

        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        seat_flush( ExecContext<SeatT>& context )
        {
            return measured( context,
                             tallies::seatFlush,
                             [&context]
                             {
                                 return context.seat->flush();
                             } );
        }

        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        seat_button( ExecContext<SeatT>& context,
                     std::uint8_t        code,
                     bool                pressed )
        {
            return measured( context,
                             tallies::seatButton,
                             [&context, code, pressed]
                             {
                                 return context.seat->button( code, pressed );
                             } );
        }

        // One pointer position, plus the flush that makes it leave the
        // process. The flush is not optional: without it the waypoint sits in
        // the connection's output buffer and lands whenever the buffer next
        // drains, which is exactly the pacing this layer exists to provide.
        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        warp_to( ExecContext<SeatT>&   context,
                 grab::geometry::Point point )
        {
            if( !in_seat_range( point ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "pointer coordinate is outside int16 range" );
            }
            auto moved = seat_move( context,
                                    static_cast<std::int16_t>( point.x ),
                                    static_cast<std::int16_t>( point.y ) );
            if( !moved )
            {
                return moved;
            }
            return seat_flush( context );
        }

        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::Result<void>
        drive_button( ExecContext<SeatT>& context,
                      std::uint8_t        code,
                      bool                pressed )
        {
            auto driven = seat_button( context, code, pressed );
            if( !driven )
            {
                return driven;
            }
            return seat_flush( context );
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::Result<void>
        drive_key( ExecContext<SeatT>& context,
                   std::string_view    name,
                   bool                pressed )
        requires KeyboardSeat<SeatT>
        {
            auto driven =
                measured( context,
                          tallies::seatKey,
                          [&context, name, pressed]
                          {
                              return context.seat->key_by_name( name, pressed );
                          } );
            if( !driven )
            {
                return driven;
            }
            if constexpr( PointerSeat<SeatT> )
            {
                return seat_flush( context );
            }
            else
            {
                return context.seat->flush();
            }
        }

        // Where a move starts. An absent origin is not zero: it means
        // "wherever the pointer already is", which only the seat can answer —
        // and it is the one BLOCKING READ in this file, so it is tallied
        // separately rather than folded into the enter() that asked for it.
        template<typename SeatT>
        [[nodiscard]]
        grab::Result<grab::geometry::Point>
        resolve_origin( const std::optional<grab::geometry::Point>& declared,
                        ExecContext<SeatT>&                         context )
        {
            if( declared.has_value() )
            {
                return *declared;
            }
            if constexpr( LocatingSeat<SeatT> )
            {
                return measured( context,
                                 tallies::seatLocate,
                                 [&context]
                                 {
                                     return context.seat->pointer_position();
                                 } );
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
        // Microseconds are the unit every timing value in this file is logged
        // in: nanoseconds overflow a reader's patience and milliseconds round a
        // pacing error of interest down to zero.
        [[nodiscard]]
        constexpr std::chrono::microseconds::rep
        as_micros( std::chrono::nanoseconds span ) noexcept
        {
            return std::chrono::duration_cast<std::chrono::microseconds>( span ).count();
        }

        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::sequence::Status
        advance_walk( grab::CommandKind                     kind,
                      CommandState&                         state,
                      ExecContext<SeatT>&                   context,
                      std::chrono::milliseconds             dwell,
                      std::chrono::steady_clock::time_point now )
        {
            while( state.emitted < state.waypoints.size() )
            {
                const auto ordinal =
                    static_cast<std::chrono::milliseconds::rep>( state.emitted + 1U );
                const auto due = state.started + ( dwell * ordinal );
                if( now < due )
                {
                    break;
                }

                const auto point = state.waypoints.at( state.emitted );
                auto       moved = warp_to( context, point );
                if( !moved )
                {
                    note_failure( kind, "waypoint", moved.error() );
                    return grab::sequence::Status::Failure;
                }
                ++state.emitted;

                // THE PACING ERROR, and the reason this loop reports at all.
                // `now - due` is how late this waypoint was; it is never
                // negative, because the loop broke above if the deadline had
                // not arrived. Two arithmetic operations and no clock read —
                // `now` was handed in and `due` was needed for the break.
                const auto overshoot =
                    std::chrono::duration_cast<std::chrono::nanoseconds>( now - due );
                state.worst_overshoot = std::max( state.worst_overshoot, overshoot );

                // Everything below is built INSIDE the sink, so an off level
                // computes none of it — this runs once per waypoint and a
                // 512-step drag runs it 512 times.
                log::debug(
                    [kind, &state, point, dwell, ordinal, overshoot]( auto& event )
                    {
                        event.tag( log::tags::input )
                            .value( "command", grab::command_name( kind ) )
                            .value( "waypoint", state.emitted )
                            .value( "of", state.waypoints.size() )
                            .value( "x", point.x )
                            .value( "y", point.y )
                            .value( "due_us", as_micros( dwell * ordinal ) )
                            .value( "fired_us",
                                    as_micros( ( dwell * ordinal ) + overshoot ) )
                            .value( "late_us", as_micros( overshoot ) );
                    }
                );
            }

            if( state.emitted != state.waypoints.size() )
            {
                return grab::sequence::Status::Running;
            }

            // The per-walk summary: what the document asked for, what the run
            // delivered, and the worst single miss inside it. Latched, because
            // a Timed command answers Success on every tick past its deadline
            // and the summary describes the completion, not the tick.
            if( !state.reported )
            {
                state.reported = true;
                log::verbose(
                    [kind, &state, dwell, now]( auto& event )
                    {
                        const auto intended =
                            dwell * static_cast<std::chrono::milliseconds::rep>(
                                        state.waypoints.size()
                                    );
                        event.tag( log::tags::input )
                            .value( "command", grab::command_name( kind ) )
                            .value( "walk", "complete" )
                            .value( "waypoints", state.emitted )
                            .value( "intended_us", as_micros( intended ) )
                            .value( "actual_us", as_micros( now - state.started ) )
                            .value( "worst_overshoot_us",
                                    as_micros( state.worst_overshoot ) );
                    }
                );
            }
            return grab::sequence::Status::Success;
        }

        // Shared preamble for the three motion commands: validate the
        // options, resolve and range-check the walk, put the pointer on the
        // path's first point, and set the deadline the walk will finish by.
        template<PointerSeat SeatT>
        [[nodiscard]]
        grab::sequence::Status
        begin_walk( grab::CommandKind                    kind,
                    CommandState&                        state,
                    ExecContext<SeatT>&                  context,
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
                auto placed = warp_to( context, *origin );
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

            // The plan the walk is about to be judged against: how many
            // waypoints, how far apart, and the instant the last one is owed
            // by. Without it a pacing error read later has no denominator.
            note_deadline( kind,
                           state.waypoints.size(),
                           options.step_dwell,
                           state.deadline - state.started );
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
            note_deadline( grab::CommandKind::Wait,
                           0U,
                           std::chrono::milliseconds::zero(),
                           command.duration );
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
                return note_unavailable( grab::CommandKind::Warp );
            }
            else
            {
                auto placed = warp_to( context, command.to );
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
                return note_unavailable( grab::CommandKind::Click );
            }
            else
            {
                auto pressed = seat_button( context, command.button, true );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::Click, "press", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                auto released = seat_button( context, command.button, false );
                if( !released )
                {
                    note_failure( grab::CommandKind::Click,
                                  "release",
                                  released.error() );
                    return grab::sequence::Status::Failure;
                }
                auto flushed = seat_flush( context );
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
                return note_unavailable( grab::CommandKind::ClickAt );
            }
            else
            {
                auto placed = warp_to( context, command.at );
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
                return note_unavailable( grab::CommandKind::Press );
            }
            else
            {
                // Marked held BEFORE the call: XTest may have committed the
                // press even when the round trip reports failure, and a button
                // that might be down has to be released like one that is.
                state.document_hold = true;
                note_hold_taken( grab::CommandKind::Press, documentHold );
                auto pressed = drive_button( context, command.button, true );
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
                return note_unavailable( grab::CommandKind::Release );
            }
            else
            {
                auto released = drive_button( context, command.button, false );
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
                return note_unavailable( grab::CommandKind::Scroll );
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
                        auto pressed = seat_button( context, code, true );
                        if( !pressed )
                        {
                            return pressed;
                        }
                        auto released = seat_button( context, code, false );
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

                auto flushed = seat_flush( context );
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
                return note_unavailable( grab::CommandKind::Type );
            }
            else
            {
                auto typed =
                    measured( context,
                              tallies::seatText,
                              [&context, &command]
                              {
                                  return context.seat->type_text( command.text );
                              } );
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
                return note_unavailable( grab::CommandKind::Key );
            }
            else
            {
                auto pressed =
                    measured( context,
                              tallies::seatKey,
                              [&context, &command]
                              {
                                  return context.seat->key_by_name( command.key, true );
                              } );
                if( !pressed )
                {
                    note_failure( grab::CommandKind::Key, "press", pressed.error() );
                    return grab::sequence::Status::Failure;
                }
                auto released = drive_key( context, command.key, false );
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
                return note_unavailable( grab::CommandKind::KeyDown );
            }
            else
            {
                // The document owns this hold — a matching input.key_up
                // releases it, which is the only way to spell Ctrl+C. Marked
                // before the call for the same reason as Press.
                state.document_hold = true;
                note_hold_taken( grab::CommandKind::KeyDown, documentHold );
                auto pressed = drive_key( context, command.key, true );
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
                return note_unavailable( grab::CommandKind::KeyUp );
            }
            else
            {
                auto released = drive_key( context, command.key, false );
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
                return note_unavailable( grab::CommandKind::Move );
            }
            else
            {
                auto options = validate_options( command.options );
                if( !options )
                {
                    note_failure( grab::CommandKind::Move, "options", options.error() );
                    return grab::sequence::Status::Failure;
                }

                auto origin = resolve_origin( command.from, context );
                if( !origin )
                {
                    note_failure( grab::CommandKind::Move, "origin", origin.error() );
                    return grab::sequence::Status::Failure;
                }

                return begin_walk( grab::CommandKind::Move,
                                   state,
                                   context,
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
                return note_unavailable( grab::CommandKind::Follow );
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
                                   context,
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
                return note_unavailable( grab::CommandKind::Drag );
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
                                context,
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
                state.held = true;
                note_hold_taken( grab::CommandKind::Drag, implicitHold );
                auto pressed = drive_button( context, command.button, true );
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
                return note_unavailable( grab::CommandKind::Capture );
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
                    measured( context,
                              tallies::seatCaptureBegin,
                              [&context, &command]
                              {
                                  return context.seat->begin_capture( command.output,
                                                                      command.locator );
                              } );
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
        // The overlay arms. All eight are Instant and none is Blocking: the
        // mutation itself averages 0.02 ms from the reactor thread, and the
        // frame it becomes visible in is paid by the player's per-tick flush
        // rather than by the step. So enter() returns Success or Failure,
        // there is no Running state to invent, and only overlay.grab has an
        // exit() of its own.
        //
        // What is NOT here, deliberately: the handle-to-ShapeId map, the
        // per-tick repositioning that makes overlay.attach look like a carry,
        // and the real Overlay adapter. All three are run state, and the seat
        // already sees every waypoint — move_pointer_absolute is called once
        // per waypoint — so attachment tracking belongs there. These arms
        // forward the handle and the offset through and report what the seat
        // answers; that is the whole of their job.
        // ---------------------------------------------------------------

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayAddCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayAdd );
            }
            else
            {
                // The one op that may omit its handle: an unhandled add is
                // fire-and-forget, which is a useful thing to be able to say
                // and not an error to be corrected.
                return settle_overlay(
                    grab::CommandKind::OverlayAdd,
                    command.handle,
                    measured( context,
                              tallies::seatOverlayAdd,
                              [&context, &command]
                              {
                                  return context.seat->overlay_add( command.handle,
                                                                    command.shape );
                              } )
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayUpdateCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayUpdate );
            }
            else
            {
                if( command.handle.empty() )
                {
                    return note_missing_handle( grab::CommandKind::OverlayUpdate );
                }
                return settle_overlay(
                    grab::CommandKind::OverlayUpdate,
                    command.handle,
                    measured( context,
                              tallies::seatOverlayUpdate,
                              [&context, &command]
                              {
                                  return context.seat->overlay_update( command.handle,
                                                                       command.shape );
                              } )
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayRemoveCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayRemove );
            }
            else
            {
                if( command.handle.empty() )
                {
                    return note_missing_handle( grab::CommandKind::OverlayRemove );
                }
                // Idempotent: a ttl or fade lifetime expires a shape from the
                // scene itself, so a remove may find nothing. The seat reports
                // that as success, and this arm does not second-guess it.
                return settle_overlay(
                    grab::CommandKind::OverlayRemove,
                    command.handle,
                    measured( context,
                              tallies::seatOverlayRemove,
                              [&context, &command]
                              {
                                  return context.seat->overlay_remove( command.handle );
                              } )
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayClearCommand&,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayClear );
            }
            else
            {
                return settle_overlay(
                    grab::CommandKind::OverlayClear,
                    {},
                    measured( context,
                              tallies::seatOverlayClear,
                              [&context]
                              {
                                  return context.seat->overlay_clear();
                              } )
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayGrabCommand&,
                       CommandState&       state,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayGrab );
            }
            else
            {
                // THE CALLER OWNS THE CAPTURE. Marked before the call, and
                // deliberately not through document_hold — see
                // CommandState::overlay_grab_held for both reasons. Without
                // this line a run interrupted between overlay.grab and
                // overlay.release leaves the pointer grabbed, which freezes
                // the desktop for everyone, not just for grab.
                state.overlay_grab_held = true;
                note_hold_taken( grab::CommandKind::OverlayGrab, pointerCaptureHold );
                return settle_overlay( grab::CommandKind::OverlayGrab,
                                       {},
                                       measured( context,
                                                 tallies::seatOverlayGrab,
                                                 [&context]
                                                 {
                                                     return context.seat->overlay_grab();
                                                 } ) );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayReleaseCommand&,
                       CommandState&       state,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayRelease );
            }
            else
            {
                // A release step ends with the capture down whatever it found,
                // so its own state carries no grab into the unwind. This
                // clears its OWN flag and cannot clear the grab step's, which
                // lives in a different CommandState: a run unwound after a
                // completed grab/release pair therefore issues one further,
                // harmless release. overlay.release is Idempotent precisely so
                // that trade is available.
                state.overlay_grab_held = false;
                note_release( grab::CommandKind::OverlayRelease,
                              "pointer_capture",
                              pointerCaptureHold,
                              releasedByCommand,
                              true );
                return settle_overlay(
                    grab::CommandKind::OverlayRelease,
                    {},
                    measured( context,
                              tallies::seatOverlayRelease,
                              [&context]
                              {
                                  return context.seat->overlay_release();
                              } )
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayAttachCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayAttach );
            }
            else
            {
                if( command.handle.empty() )
                {
                    return note_missing_handle( grab::CommandKind::OverlayAttach );
                }
                // The offset is forwarded exactly as the document spelled it,
                // absence included: nullopt means "keep the gap the shape
                // already has", which is the shape's position minus the
                // pointer's at attach time and is knowable only where the
                // scene is — in the seat.
                return settle_overlay(
                    grab::CommandKind::OverlayAttach,
                    command.handle,
                    measured( context,
                              tallies::seatOverlayAttach,
                              [&context, &command]
                              {
                                  return context.seat->overlay_attach( command.handle,
                                                                       command.offset );
                              } )
                );
            }
        }

        template<typename SeatT>
        [[nodiscard]]
        grab::sequence::Status
        enter_command( const grab::sequence::OverlayDetachCommand& command,
                       CommandState&,
                       ExecContext<SeatT>& context )
        {
            if constexpr( !OverlaySeat<SeatT> )
            {
                return note_unavailable( grab::CommandKind::OverlayDetach );
            }
            else
            {
                if( command.handle.empty() )
                {
                    return note_missing_handle( grab::CommandKind::OverlayDetach );
                }
                return settle_overlay(
                    grab::CommandKind::OverlayDetach,
                    command.handle,
                    measured( context,
                              tallies::seatOverlayDetach,
                              [&context, &command]
                              {
                                  return context.seat->overlay_detach( command.handle );
                              } )
                );
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
            if( now < state.deadline )
            {
                return grab::sequence::Status::Running;
            }
            // How late the pump was in noticing, which is the wait's own
            // pacing error: a wait cannot finish early, so everything past
            // the deadline is scheduling latency. Latched, because a wait
            // answers Success on every later tick too.
            state.worst_overshoot =
                std::chrono::duration_cast<std::chrono::nanoseconds>( now -
                                                                      state.deadline );
            if( !state.reported )
            {
                state.reported = true;
                note_deadline_met( grab::CommandKind::Wait, state.worst_overshoot );
            }
            return grab::sequence::Status::Success;
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
                return note_unavailable( grab::CommandKind::Move );
            }
            else
            {
                return advance_walk( grab::CommandKind::Move,
                                     state,
                                     context,
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
                return note_unavailable( grab::CommandKind::Follow );
            }
            else
            {
                return advance_walk( grab::CommandKind::Follow,
                                     state,
                                     context,
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
                return note_unavailable( grab::CommandKind::Drag );
            }
            else
            {
                const auto walked = advance_walk( grab::CommandKind::Drag,
                                                  state,
                                                  context,
                                                  command.options.step_dwell,
                                                  now );
                if( walked != grab::sequence::Status::Success )
                {
                    return walked;
                }

                auto released = drive_button( context, command.button, false );
                if( !released )
                {
                    // held stays true, so exit() tries the release again on
                    // the unwind. A button left down outlives the process.
                    note_failure( grab::CommandKind::Drag, "release", released.error() );
                    return grab::sequence::Status::Failure;
                }
                state.held = false;
                note_release( grab::CommandKind::Drag,
                              "button",
                              implicitHold,
                              releasedByCommand,
                              true );
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
                return note_unavailable( grab::CommandKind::Capture );
            }
            else
            {
                auto polled = measured( context,
                                        tallies::seatCapturePoll,
                                        [&context]
                                        {
                                            return context.seat->poll_capture();
                                        } );
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
        // exit(): the neutralization path. Only four commands can leave
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
                auto released = drive_button( context, command.button, false );
                // An IMPLICIT hold: the drag took this button for its own
                // walk, so the reason is always "the command owned it" and
                // never the unwind, whether or not the run was interrupted.
                note_release( grab::CommandKind::Drag,
                              "button",
                              implicitHold,
                              releasedByCommand,
                              released.has_value() );
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
                auto released       = drive_button( context, command.button, false );
                // A DOCUMENT hold, and the guard above means this line is
                // reached only on the unwind — which is exactly what the
                // reason records.
                note_release( grab::CommandKind::Press,
                              "button",
                              documentHold,
                              releasedByUnwind,
                              released.has_value() );
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
                auto released       = drive_key( context, command.key, false );
                note_release( grab::CommandKind::KeyDown,
                              "key",
                              documentHold,
                              releasedByUnwind,
                              released.has_value() );
                if( !released )
                {
                    note_failure( grab::CommandKind::KeyDown,
                                  "neutralize",
                                  released.error() );
                }
            }
        }

        // The one overlay op with an exit(), and the one hold in this file
        // whose escape is worse than a stranded button: a pointer grab that
        // outlives its owner freezes the WHOLE DESKTOP, and recovery needs
        // another X client or a VT switch.
        //
        // The rule is §6.1's, unchanged — an explicit hold the document owns
        // is released only on unwind, because overlay.grab and overlay.release
        // are a pair and the first step's own exit() must not break it, any
        // more than a key_down's exit() may break a chord. `interrupted` is
        // the only thing that proves the release step will never run.
        template<typename SeatT>
        void
        exit_command( const grab::sequence::OverlayGrabCommand&,
                      CommandState&       state,
                      ExecContext<SeatT>& context )
        {
            if constexpr( OverlaySeat<SeatT> )
            {
                if( !state.overlay_grab_held || !state.interrupted )
                {
                    return;
                }
                // Cleared before the attempt, like the drag's button: a
                // failing seat must not turn exit() into an unbounded retry.
                // The failure is logged, which is what a NeutralizationOutcome
                // reports upwards — and a failed release here is the single
                // worst thing this layer can report.
                state.overlay_grab_held = false;
                auto released           = measured( context,
                                                    tallies::seatOverlayRelease,
                                                    [&context]
                                                    {
                                              return context.seat->overlay_release();
                                                    } );
                note_release( grab::CommandKind::OverlayGrab,
                              "pointer_capture",
                              pointerCaptureHold,
                              releasedByUnwind,
                              released.has_value() );
                if( !released )
                {
                    note_failure( grab::CommandKind::OverlayGrab,
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
        state.entered         = true;
        state.started         = context.now;
        state.deadline        = context.now;
        state.emitted         = 0U;
        state.worst_overshoot = std::chrono::nanoseconds::zero();
        state.reported        = false;
        state.waypoints.clear();

        const auto             kind = grab::sequence::kind_of( command );

        // The measured region is the visit and nothing else: the record below
        // is built after it, so composing a log line is never charged to the
        // command that happened to be running.
        grab::sequence::Status status{};
        {
            const detail::Sample<detail::measureLevel> sample{
                context.instrument,
                detail::tally_name( detail::Phase::Enter, kind )
            };
            status = std::visit(
                [&state, &context]( const auto& payload )
                {
                    return detail::enter_command( payload, state, context );
                },
                command
            );
        }

        log::verbose(
            [kind, status]( auto& event )
            {
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
        // reporting Success would retire work that never happened. Not
        // measured: there is no work here to attribute.
        if( !state.entered )
        {
            return grab::sequence::Status::Failure;
        }

        const auto             kind = grab::sequence::kind_of( command );
        grab::sequence::Status status{};
        {
            const detail::Sample<detail::measureLevel> sample{
                context.instrument,
                detail::tally_name( detail::Phase::Tick, kind )
            };
            status = std::visit(
                [&state, &context, now]( const auto& payload )
                {
                    return detail::tick_command( payload, state, context, now );
                },
                command
            );
        }

        // A tick that only checked a clock is the common case and says nothing
        // worth a line; a tick that CHANGED the answer is the state transition.
        // The waypoint records under `input` carry the rest.
        if( status != grab::sequence::Status::Running )
        {
            log::verbose(
                [kind, status, &state]( auto& event )
                {
                    event.tag( log::tags::sequence )
                        .value( "tick", grab::command_name( kind ) )
                        .value( "status", grab::sequence::status_name( status ) )
                        .value( "emitted", state.emitted )
                        .value( "worst_overshoot_us",
                                detail::as_micros( state.worst_overshoot ) );
                }
            );
        }
        return status;
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

        const auto kind = grab::sequence::kind_of( command );

        // WHAT THIS STEP STILL HOLDS, read before the visit clears it. The
        // three flags are the only reason an exit() is ever more than a
        // formality, and reading them afterwards would report every exit as
        // holding nothing.
        const bool held_anything =
            state.held || state.document_hold || state.overlay_grab_held;

        {
            const detail::Sample<detail::measureLevel> sample{
                context.instrument,
                detail::tally_name( detail::Phase::Exit, kind )
            };
            std::visit(
                [&state, &context]( const auto& payload )
                {
                    detail::exit_command( payload, state, context );
                },
                command
            );
        }

        // Whether the step was UNWOUND or simply finished is the distinction
        // §6.1 draws, and it is what decides whether a document hold was
        // lifted or deliberately left for a later step. A record that omits it
        // cannot tell a working chord from a cut-short one.
        log::verbose(
            [kind, &state, held_anything]( auto& event )
            {
                event.tag( log::tags::sequence )
                    .value( "exit", grab::command_name( kind ) )
                    .value( "reason",
                            state.interrupted ? detail::releasedByUnwind
                                              : detail::releasedByCommand )
                    .value( "held_on_entry", held_anything )
                    .value( "holds_after",
                            state.held ||
                                state.document_hold ||
                                state.overlay_grab_held );
            }
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
