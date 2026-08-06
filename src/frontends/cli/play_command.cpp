#include "codec/png.hpp"
#include "frontends/cli/common.hpp"
#include "frontends/cli/play_command.hpp"
#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/geometry/point.hpp"
#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/sequence_types.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/trace.hpp"
#include "kernel/scheduling/timer_thread.hpp"
#include "kernel/sequence/interpreter.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace grab::cli
{

    namespace
    {

        using Clock       = std::chrono::steady_clock;
        using OrderedJson = nlohmann::ordered_json;
        using grab::kernel::sequence::Sequence;

        constexpr std::string_view pacingFlag     = "--pacing";
        constexpr std::string_view graceFlag      = "--grace-ms";
        constexpr std::string_view dryRunFlag     = "--dry-run";
        constexpr std::string_view reportFlag     = "--report";
        constexpr std::string_view flagPrefix     = "-";
        constexpr std::string_view singleStepName = "cli";

        // A day. The bound exists so that grace * depth cannot overflow the
        // nanosecond duration the plan is accumulated in.
        constexpr std::uint64_t    maximumGraceMs = 86'400'000U;

        // The loop wakes on the timer's eventfd; this bounds how long it can
        // block if that wake is ever lost, so a stuck run is a slow run rather
        // than a hung process.
        constexpr int              maximumWaitMs = 250;
        constexpr int              minimumWaitMs = 1;

        // What to arm when the player wants a tick but states no deadline --
        // an Opaque body, which finishes when it finishes. Polling it at this
        // cadence is what keeps the loop off a spin.
        constexpr auto             opaquePollPeriod = std::chrono::milliseconds{ 1 };

        void
        print_play_usage()
        {
            ( void )std::fputs( "usage: grab play SEQUENCE.json "
                                "[--pacing strict|grace|precise] [--grace-ms N] "
                                "[--dry-run] [--report PATH.jsonl]\n",
                                stderr );
        }

        [[nodiscard]]
        std::string_view
        neutralization_name( grab::NeutralizationOutcome outcome ) noexcept
        {
            switch( outcome )
            {
                case grab::NeutralizationOutcome::NotAttempted :
                    return "not_attempted";
                case grab::NeutralizationOutcome::NothingHeld :
                    return "nothing_held";
                case grab::NeutralizationOutcome::Released :
                    return "released";
                case grab::NeutralizationOutcome::Failed :
                    return "failed";
            }
            return "not_attempted";
        }

        // The commit column of the per-step Receipt. The Player reports a
        // status rather than a commit, so this is a projection: a step that
        // succeeded committed, and one that never entered did not.
        [[nodiscard]]
        grab::CommitStatus
        commit_of( grab::sequence::StepStatus status ) noexcept
        {
            switch( status )
            {
                case grab::sequence::StepStatus::Succeeded :
                    return grab::CommitStatus::Committed;
                case grab::sequence::StepStatus::Failed :
                case grab::sequence::StepStatus::Pending :
                case grab::sequence::StepStatus::Ready :
                case grab::sequence::StepStatus::Running :
                case grab::sequence::StepStatus::Skipped :
                case grab::sequence::StepStatus::Count :
                    break;
            }
            return grab::CommitStatus::FailedBeforeCommit;
        }

        [[nodiscard]]
        grab::Result<void>
        write_bytes( const std::filesystem::path& path,
                     std::span<const std::byte>   bytes )
        {
            if( bytes.size() >
                static_cast<std::size_t>( std::numeric_limits<std::streamsize>::max() ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "capture output is too large to write" );
            }

            std::ofstream stream{ path, std::ios::binary };
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "failed to open capture output: " + path.string() );
            }
            for( const std::byte value : bytes )
            {
                stream.put(
                    static_cast<char>( std::to_integer<unsigned char>( value ) )
                );
            }
            if( !stream )
            {
                return grab::fail( grab::ErrorCode::DeviceInaccessible,
                                   "failed to write capture output: " + path.string() );
            }
            return {};
        }

        // ── Overlay geometry, walked generically ──────────────
        //
        // Two jobs need every point of a Shape and nothing else: STAMPING the
        // delegate's coordinate space onto a document that could not name one,
        // and TRANSLATING a shape that rides the pointer. Both are spelled
        // once, over a visitor, so a new Geometry alternative fails to compile
        // here rather than silently going unstamped or refusing to move.
        //
        // A document is written before any session exists, so every SpacePoint
        // it carries has the default space id — and the default is not a
        // registered space. The transform lookup would refuse it, so the stamp
        // is not cosmetic: without it every overlay.add fails.
        template<typename Visit>
        void
        for_each_point( grab::overlay::Geometry& geometry,
                        Visit&&                  visit )
        {
            std::visit(
                [&visit]( auto& figure )
                {
                    using Figure = std::remove_cvref_t<decltype( figure )>;
                    if constexpr( std::is_same_v<Figure, grab::overlay::Rect> )
                    {
                        visit( figure.bounds.x, figure.bounds.y, figure.bounds.space );
                    }
                    else if constexpr( std::is_same_v<Figure, grab::overlay::Ellipse> )
                    {
                        visit( figure.center.x, figure.center.y, figure.center.space );
                    }
                    else if constexpr( std::is_same_v<Figure, grab::overlay::Polygon> )
                    {
                        for( auto& point : figure.points )
                        {
                            visit( point.x, point.y, point.space );
                        }
                    }
                    else
                    {
                        for( auto& command : figure.commands )
                        {
                            std::visit(
                                [&visit]( auto& element )
                                {
                                    using Element =
                                        std::remove_cvref_t<decltype( element )>;
                                    if constexpr( std::is_same_v<
                                                      Element,
                                                      grab::overlay::MoveTo> ||
                                                  std::is_same_v<Element,
                                                                 grab::overlay::LineTo> )
                                    {
                                        visit( element.point.x,
                                               element.point.y,
                                               element.point.space );
                                    }
                                    else if constexpr( std::is_same_v<
                                                           Element,
                                                           grab::overlay::BezierTo> )
                                    {
                                        for( auto& control : element.control )
                                        {
                                            visit( control.x, control.y, control.space );
                                        }
                                    }
                                },
                                command
                            );
                        }
                    }
                },
                geometry
            );
        }

        void
        stamp_space( grab::overlay::Geometry& geometry,
                     grab::CoordinateSpaceId  space )
        {
            for_each_point( geometry,
                            [space]( double&, double&, grab::CoordinateSpaceId& stamp )
                            {
                                stamp = space;
                            } );
        }

        void
        translate_geometry( grab::overlay::Geometry& geometry,
                            double                   dx,
                            double                   dy )
        {
            for_each_point( geometry,
                            [dx, dy]( double& x, double& y, grab::CoordinateSpaceId& )
                            {
                                x += dx;
                                y += dy;
                            } );
        }

        // Where a shape "is": a rect's origin, an ellipse's centre, the first
        // named point of a polygon or path. It is the reference the carry
        // offset is measured from, so it only has to be STABLE under
        // translation, which every alternative's first point is.
        struct Anchor
        {
                double x{ 0.0 };
                double y{ 0.0 };
        };

        [[nodiscard]]
        Anchor
        anchor_of( const grab::overlay::Geometry& geometry )
        {
            auto   copy = geometry;
            Anchor found{};
            bool   seen = false;
            for_each_point( copy,
                            [&found,
                             &seen]( double& x, double& y, grab::CoordinateSpaceId& )
                            {
                                if( seen )
                                {
                                    return;
                                }
                                seen    = true;
                                found.x = x;
                                found.y = y;
                            } );
            return found;
        }

        // The seat `grab play` actually drives: grab::Input, widened to the
        // concepts in execute.hpp.
        //
        // grab::input::Seat spells keys by KEYCODE, and the name-to-keycode
        // step needs a Keymap that lives above the seat -- so the adapter sits
        // on grab::Input, which already exposes key_down/key_up by name, one
        // rung higher than the raw seat.
        //
        // flush() is a no-op ON PURPOSE: every grab::Input operation flushes
        // its own connection before returning (input_facade.cpp:306 and
        // friends), so there is never a waypoint left sitting in an output
        // buffer for this to push out.
        class InputSeat final
        {
            public:

                [[nodiscard]]
                static grab::Result<InputSeat>
                open( const char*      display,
                      std::string_view layout )
                {
                    auto input = grab::Input::open( display, layout );
                    if( !input.has_value() )
                    {
                        return std::unexpected( std::move( input.error() ) );
                    }
                    return InputSeat{
                        std::move( *input ),
                        display == nullptr ? std::string{} : std::string{ display },
                    };
                }

                // Every waypoint of every motion command arrives here, once
                // per waypoint -- which is exactly why ATTACHMENT LIVES IN THE
                // SEAT. A shape that rides the pointer has to be repositioned
                // on each of those ticks; hang it off the command layer and it
                // teleports at the end of the move instead of being carried.
                [[nodiscard]]
                grab::Result<void>
                move_pointer_absolute( std::int16_t x,
                                       std::int16_t y )
                {
                    auto moved = input_.move( x, y );
                    if( !moved.has_value() )
                    {
                        return moved;
                    }
                    return carry_attached( x, y );
                }

                [[nodiscard]]
                grab::Result<void>
                button( std::uint8_t code,
                        bool         pressed )
                {
                    return pressed ? input_.press( code ) : input_.release( code );
                }

                [[nodiscard]]
                grab::Result<void>
                flush()
                {
                    return {};
                }

                [[nodiscard]]
                grab::Result<grab::geometry::Point>
                pointer_position()
                {
                    return input_.position();
                }

                [[nodiscard]]
                grab::Result<void>
                key_by_name( std::string_view name,
                             bool             pressed )
                {
                    return pressed ? input_.key_down( name ) : input_.key_up( name );
                }

                [[nodiscard]]
                grab::Result<void>
                type_text( std::string_view utf8 )
                {
                    return input_.type_text( utf8 );
                }

                // Synchronous, and deliberately so: there is no worker here to
                // join, which is why the runner's join() is a no-op. The split
                // into begin/poll is kept because the Opaque contract owns it
                // -- a capture that reported from one call would force the
                // layer above to pretend the work took no time.
                [[nodiscard]]
                grab::Result<void>
                begin_capture( std::string_view output,
                               std::string_view locator )
                {
                    if( !locator.empty() )
                    {
                        // The grammar accepts a locator target but pins no
                        // destination for it, so there is nowhere to put the
                        // image. Saying which capability is missing beats a
                        // step that silently does nothing.
                        return grab::fail(
                            grab::ErrorCode::CapabilityUnavailable,
                            "screen.capture by locator has no destination in the "
                            "sequence grammar; use \"out\""
                        );
                    }
                    capture_ = capture_display( output );
                    return {};
                }

                [[nodiscard]]
                std::optional<grab::Result<void>>
                poll_capture()
                {
                    if( !capture_.has_value() )
                    {
                        return std::nullopt;
                    }
                    auto done = std::move( *capture_ );
                    capture_.reset();
                    return done;
                }

                // ── OverlaySeat ──────────────────────────────────
                //
                // THE HANDLE-TO-ShapeId MAP IS RUN STATE AND LIVES HERE. A
                // document names a shape before any scene exists, so nothing
                // in it can carry a ShapeId; the seat is the first place that
                // knows both.
                //
                // The session is opened LAZILY, on the first overlay step. A
                // document that draws nothing must not pay for a session, and
                // `grab click` -- which routes through this same seat as a
                // one-step document -- must not start failing on a display
                // with no compositing manager.

                [[nodiscard]]
                grab::Result<void>
                overlay_add( std::string_view            handle,
                             const grab::overlay::Shape& shape )
                {
                    auto surface = ensure_overlay();
                    if( !surface.has_value() )
                    {
                        return std::unexpected( std::move( surface.error() ) );
                    }

                    grab::overlay::Shape placed = shape;
                    stamp_space( placed.geometry, space_ );
                    auto id = ( *surface )->add( placed );
                    if( !id.has_value() )
                    {
                        return std::unexpected( std::move( id.error() ) );
                    }
                    if( handle.empty() )
                    {
                        // Fire-and-forget: drawable, never referenced again.
                        // Storing it under a name nothing can spell would only
                        // grow the map for the life of the run.
                        return {};
                    }
                    std::erase_if( shapes_,
                                   [handle]( const Placed& entry )
                                   {
                                       return entry.handle == handle;
                                   } );
                    shapes_.push_back( Placed{
                        .handle   = std::string{ handle },
                        .id       = *id,
                        .shape    = std::move( placed ),
                        .offset   = Anchor{},
                        .attached = false,
                    } );
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_update( std::string_view            handle,
                                const grab::overlay::Shape& shape )
                {
                    auto* const entry = find_shape( handle );
                    if( entry == nullptr )
                    {
                        return unknown_handle( handle );
                    }
                    grab::overlay::Shape placed = shape;
                    stamp_space( placed.geometry, space_ );
                    auto updated = overlay_->update( entry->id, placed );
                    if( !updated.has_value() )
                    {
                        return updated;
                    }
                    // The document's geometry becomes the new truth, carry or
                    // no carry: the next waypoint re-derives the shape's
                    // position from it, so a recolour mid-carry keeps riding
                    // rather than snapping back to where it was added.
                    entry->shape = std::move( placed );
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_remove( std::string_view handle )
                {
                    auto* const entry = find_shape( handle );
                    if( entry == nullptr )
                    {
                        return unknown_handle( handle );
                    }
                    const auto id = entry->id;
                    std::erase_if( shapes_,
                                   [handle]( const Placed& candidate )
                                   {
                                       return candidate.handle == handle;
                                   } );
                    auto removed = overlay_->remove( id );
                    if( !removed.has_value() &&
                        removed.error().code == grab::ErrorCode::StaleShape )
                    {
                        // A ttl or fade lifetime expires a shape from the
                        // scene ITSELF, so a remove may legitimately find
                        // nothing. Design §3.2 makes that a no-op rather than
                        // an error, because the alternative makes a fading
                        // flash plus explicit cleanup unwritable.
                        return {};
                    }
                    return removed;
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_clear()
                {
                    if( overlay_ == nullptr )
                    {
                        // Nothing was ever drawn, so there is nothing to clear
                        // and no reason to open a session in order to say so.
                        shapes_.clear();
                        return {};
                    }
                    overlay_->clear();
                    shapes_.clear();
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_grab()
                {
                    auto surface = ensure_overlay();
                    if( !surface.has_value() )
                    {
                        return std::unexpected( std::move( surface.error() ) );
                    }
                    return ( *surface )->capture_pointer();
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_release()
                {
                    if( overlay_ == nullptr )
                    {
                        // No session was ever opened, so this process cannot
                        // be holding the pointer. Opening one HERE would be
                        // the wrong answer twice over: it is the unwind path,
                        // and it would fail on a display with no compositor.
                        return {};
                    }
                    return overlay_->release_pointer();
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_attach( std::string_view                     handle,
                                std::optional<grab::geometry::Point> offset )
                {
                    auto* const entry = find_shape( handle );
                    if( entry == nullptr )
                    {
                        return unknown_handle( handle );
                    }
                    if( offset.has_value() )
                    {
                        entry->offset = Anchor{
                            .x = static_cast<double>( offset->x ),
                            .y = static_cast<double>( offset->y ),
                        };
                    }
                    else
                    {
                        // The default is the gap the shape ALREADY HAS: its
                        // position minus the pointer's, right now. That is what
                        // makes a square picked up by its corner stay held by
                        // that corner instead of snapping its origin onto the
                        // cursor.
                        auto pointer = input_.position();
                        if( !pointer.has_value() )
                        {
                            return std::unexpected( std::move( pointer.error() ) );
                        }
                        const auto anchor = anchor_of( entry->shape.geometry );
                        entry->offset     = Anchor{
                            .x = anchor.x - static_cast<double>( pointer->x ),
                            .y = anchor.y - static_cast<double>( pointer->y ),
                        };
                    }
                    entry->attached = true;
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                overlay_detach( std::string_view handle )
                {
                    auto* const entry = find_shape( handle );
                    if( entry == nullptr )
                    {
                        return unknown_handle( handle );
                    }
                    entry->attached = false;
                    return {};
                }

            private:

                // One placed shape: the name the document spells, the id the
                // scene answered with, and the geometry as it currently
                // stands. The shape is kept because a carry translates it, and
                // translating it needs the absolute coordinates it last had.
                struct Placed
                {
                        std::string            handle{};
                        grab::overlay::ShapeId id{};
                        grab::overlay::Shape   shape{};
                        Anchor                 offset{};
                        bool                   attached{ false };
                };

                [[nodiscard]]
                Placed*
                find_shape( std::string_view handle )
                {
                    const auto found =
                        std::ranges::find( shapes_, handle, &Placed::handle );
                    return found == shapes_.end() ? nullptr : &*found;
                }

                [[nodiscard]]
                static grab::Result<void>
                unknown_handle( std::string_view handle )
                {
                    std::string message{ "overlay handle '" };
                    message.append( handle );
                    message.append( "' names no shape this run has added" );
                    return grab::fail( grab::ErrorCode::NoMatch, std::move( message ) );
                }

                // Opened on demand, and kept for the life of the seat. The
                // overlay facade is non-owning and stays valid for the
                // session's lifetime, so the raw pointer is the session's to
                // invalidate, not ours.
                [[nodiscard]]
                grab::Result<grab::Overlay*>
                ensure_overlay()
                {
                    if( overlay_ != nullptr )
                    {
                        return overlay_;
                    }
                    grab::SessionOptions options;
                    if( !display_.empty() )
                    {
                        // Honouring the display is not cosmetic: a session that
                        // silently connects elsewhere draws its overlay on a
                        // display the caller never named.
                        options.display = display_;
                    }
                    auto session = grab::Session::open( options );
                    if( !session.has_value() )
                    {
                        return std::unexpected( std::move( session.error() ) );
                    }
                    auto facade = ( *session )->overlay();
                    if( !facade.has_value() )
                    {
                        return std::unexpected( std::move( facade.error() ) );
                    }
                    auto space = ( *facade )->space();
                    if( !space.has_value() )
                    {
                        return std::unexpected( std::move( space.error() ) );
                    }
                    session_ = std::move( *session );
                    overlay_ = *facade;
                    space_   = *space;

                    log::nominal(
                        [this]( auto& event )
                        {
                            event.tag( log::tags::player )
                                .value( "overlay", "opened" )
                                .value( "display",
                                        display_.empty() ? "default"
                                                         : display_.c_str() );
                        }
                    );
                    return overlay_;
                }

                // Move every attached shape so it keeps the gap it was picked
                // up with. Called once per waypoint, which is what makes a
                // carry look like a carry.
                [[nodiscard]]
                grab::Result<void>
                carry_attached( std::int16_t x,
                                std::int16_t y )
                {
                    if( overlay_ == nullptr )
                    {
                        return {};
                    }
                    for( auto& entry : shapes_ )
                    {
                        if( !entry.attached )
                        {
                            continue;
                        }
                        const auto   current = anchor_of( entry.shape.geometry );
                        const double target_x =
                            static_cast<double>( x ) + entry.offset.x;
                        const double target_y =
                            static_cast<double>( y ) + entry.offset.y;
                        translate_geometry( entry.shape.geometry,
                                            target_x - current.x,
                                            target_y - current.y );
                        auto moved = overlay_->update( entry.id, entry.shape );
                        if( !moved.has_value() )
                        {
                            return moved;
                        }
                    }
                    return {};
                }

                InputSeat( grab::Input input,
                           std::string display ) noexcept :
                    input_( std::move( input ) ),
                    display_( std::move( display ) )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                capture_display( std::string_view output )
                {
                    auto screen =
                        grab::Screen::open( display_.empty() ? nullptr
                                                             : display_.c_str() );
                    if( !screen.has_value() )
                    {
                        return std::unexpected( std::move( screen.error() ) );
                    }
                    auto image = screen->display();
                    if( !image.has_value() )
                    {
                        return std::unexpected( std::move( image.error() ) );
                    }
                    auto encoded = grab::codec::encode_png( *image );
                    if( !encoded.has_value() )
                    {
                        return std::unexpected( std::move( encoded.error() ) );
                    }
                    return write_bytes( std::filesystem::path{ output }, *encoded );
                }

                grab::Input                       input_;
                std::string                       display_;
                std::optional<grab::Result<void>> capture_{};
                std::unique_ptr<grab::Session>    session_{};
                grab::Overlay*                    overlay_{ nullptr };
                grab::CoordinateSpaceId           space_{};
                std::vector<Placed>               shapes_{};
        };

        static_assert( grab::kernel::sequence::PointerSeat<InputSeat> );
        static_assert( grab::kernel::sequence::LocatingSeat<InputSeat> );
        static_assert( grab::kernel::sequence::KeyboardSeat<InputSeat> );
        static_assert( grab::kernel::sequence::TextSeat<InputSeat> );
        static_assert( grab::kernel::sequence::CapturingSeat<InputSeat> );
        static_assert( grab::kernel::sequence::OverlaySeat<InputSeat> );

        [[nodiscard]]
        grab::Result<std::uint64_t>
        parse_milliseconds( std::string_view text )
        {
            std::uint64_t     value = 0U;
            const auto* const begin = text.data();
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            const auto* const end    = begin + text.size();
            const auto        parsed = std::from_chars( begin, end, value );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                value > maximumGraceMs )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "--grace-ms must be a whole number of "
                                   "milliseconds in range 0..86400000" );
            }
            return value;
        }

        // ── The interrupt path ────────────────────────────────
        //
        // SIGINT and SIGTERM during a run, turned into an ordinary unwind
        // rather than a dead process. This matters most for overlay.grab. The
        // X server does drop a pointer grab when the grabbing client's
        // connection closes, so a KILLED process is not left holding it -- but
        // a process that merely stops pumping and goes on to write its report
        // would be, and so would any embedder that outlives the run. Turning
        // the signal into player.interrupt() runs the same unwind an abort
        // does, so the capture and every held button come up first.
        volatile std::sig_atomic_t interruptRequested = 0;    // NOLINT(cert-err58-cpp)

        extern "C" void
        note_interrupt( int )
        {
            interruptRequested = 1;
        }

        using SignalHandler = void ( * )( int );

        class InterruptTrap
        {
            public:

                InterruptTrap() noexcept
                {
                    interruptRequested = 0;
                    previous_int_      = std::signal( SIGINT, note_interrupt );
                    previous_term_     = std::signal( SIGTERM, note_interrupt );
                }

                ~InterruptTrap()
                {
                    // Restored, not left installed: this is a library entry
                    // point as well as a CLI verb, and a process-wide
                    // disposition it never gives back is a side effect nobody
                    // asked for.
                    if( previous_int_ != SIG_ERR )
                    {
                        ( void )std::signal( SIGINT, previous_int_ );
                    }
                    if( previous_term_ != SIG_ERR )
                    {
                        ( void )std::signal( SIGTERM, previous_term_ );
                    }
                    interruptRequested = 0;
                }

                InterruptTrap( const InterruptTrap& ) = delete;
                InterruptTrap&
                operator=( const InterruptTrap& ) = delete;
                InterruptTrap( InterruptTrap&& )  = delete;
                InterruptTrap&
                operator=( InterruptTrap&& ) = delete;

            private:

                SignalHandler previous_int_{ SIG_ERR };
                SignalHandler previous_term_{ SIG_ERR };
        };

        // Neutralization that survives a return statement, an error return AND
        // an exception. A destructor is the only thing a throw between here and
        // the end of the function will still run, and what it lifts may be the
        // pointer capture -- which, left down, freezes the whole desktop rather
        // than merely reaching the next application.
        template<typename SeatT>
        class OutstandingHolds
        {
            public:

                explicit OutstandingHolds(
                    grab::cli::SeatRunner<SeatT>& runner
                ) noexcept :
                    runner_( &runner )
                {
                }

                ~OutstandingHolds()
                {
                    release();
                }

                OutstandingHolds( const OutstandingHolds& ) = delete;
                OutstandingHolds&
                operator=( const OutstandingHolds& )   = delete;
                OutstandingHolds( OutstandingHolds&& ) = delete;
                OutstandingHolds&
                operator=( OutstandingHolds&& ) = delete;

                grab::NeutralizationOutcome
                release() noexcept
                {
                    if( released_ )
                    {
                        return outcome_;
                    }
                    released_ = true;
                    try
                    {
                        outcome_ = runner_->release_outstanding();
                    }
                    catch( ... )    // NOLINT(bugprone-empty-catch)
                    {
                        outcome_ = grab::NeutralizationOutcome::Failed;
                    }
                    return outcome_;
                }

            private:

                grab::cli::SeatRunner<SeatT>* runner_;
                grab::NeutralizationOutcome   outcome_{
                    grab::NeutralizationOutcome::NothingHeld
                };
                bool released_{ false };
        };

        // poll() rather than a sleep: the wait is owned by the timer thread's
        // eventfd, and the timeout is only a bound on how long a lost wake can
        // cost.
        void
        wait_readable( int                       descriptor,
                       std::chrono::milliseconds remaining )
        {
            auto budget = static_cast<std::int64_t>( remaining.count() );
            budget      = std::max<std::int64_t>( budget, minimumWaitMs );
            budget      = std::min<std::int64_t>( budget, maximumWaitMs );

            pollfd watched{
                .fd      = descriptor,
                .events  = POLLIN,
                .revents = 0,
            };
            while( ::poll( &watched, 1U, static_cast<int>( budget ) ) < 0 )
            {
                if( errno != EINTR )
                {
                    return;
                }
            }
        }

    }    // namespace

    grab::Result<PlayOptions>
    parse_play_options( std::span<const std::string_view> args )
    {
        PlayOptions options;
        bool        has_document = false;
        auto        current      = args.begin();
        while( current != args.end() )
        {
            const std::string_view argument = *current;
            ++current;

            if( argument == dryRunFlag )
            {
                options.dry_run = true;
                continue;
            }
            if( argument == pacingFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--pacing requires a value" );
                }
                const auto mode = grab::sequence::pacing_mode_from_name( *current );
                if( !mode.has_value() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--pacing must be strict, grace or precise" );
                }
                options.pacing = *mode;
                ++current;
                continue;
            }
            if( argument == graceFlag )
            {
                if( current == args.end() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--grace-ms requires a value" );
                }
                auto milliseconds = parse_milliseconds( *current );
                if( !milliseconds.has_value() )
                {
                    return std::unexpected( std::move( milliseconds.error() ) );
                }
                options.grace = std::chrono::milliseconds{
                    static_cast<std::chrono::milliseconds::rep>( *milliseconds )
                };
                ++current;
                continue;
            }
            if( argument == reportFlag )
            {
                if( current == args.end() || current->empty() )
                {
                    return grab::fail( grab::ErrorCode::InvalidArgument,
                                       "--report requires a path" );
                }
                options.report = *current;
                ++current;
                continue;
            }
            if( argument.starts_with( flagPrefix ) )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "unknown option for play: " +
                                       std::string{ argument } );
            }
            if( has_document )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "play accepts exactly one sequence document" );
            }
            options.document = argument;
            has_document     = true;
        }

        if( !has_document || options.document.empty() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               "play requires a sequence document" );
        }
        return options;
    }

    grab::sequence::PacingOptions
    effective_pacing( grab::sequence::PacingOptions document,
                      const PlayOptions&            options ) noexcept
    {
        return grab::sequence::PacingOptions{
            .mode  = options.pacing.value_or( document.mode ),
            .grace = options.grace.value_or( document.grace ),
        };
    }

    grab::Result<Sequence>
    with_pacing( const Sequence&               program,
                 grab::sequence::PacingOptions pacing )
    {
        if( program.pacing().mode ==
            pacing.mode &&
            program.pacing().grace == pacing.grace )
        {
            return program;
        }
        std::vector<grab::sequence::Step> steps( program.steps().begin(),
                                                 program.steps().end() );
        return Sequence::build( std::move( steps ),
                                pacing,
                                std::string{ program.name() } );
    }

    grab::Result<Sequence>
    single_step_sequence( grab::sequence::Command command )
    {
        std::vector<grab::sequence::Step> steps;
        steps.push_back( grab::sequence::Step{
            .command = std::move( command ),
        } );
        return Sequence::build( std::move( steps ),
                                grab::sequence::PacingOptions{},
                                std::string{ singleStepName } );
    }

    std::string
    dry_run_report( const Sequence&               program,
                    grab::sequence::PacingOptions pacing )
    {
        std::string text;

        text.append( "sequence: " );
        text.append( program.name() );
        text.push_back( '\n' );

        text.append( "pacing: " );
        text.append( grab::sequence::pacing_mode_name( pacing.mode ) );
        text.append( " grace_ms=" );
        text.append( std::to_string( pacing.grace.count() ) );
        text.push_back( '\n' );

        text.append( "steps: " );
        text.append( std::to_string( program.steps().size() ) );
        text.push_back( '\n' );

        text.append( "order:" );
        for( const auto id : program.order() )
        {
            text.push_back( ' ' );
            text.append( std::to_string( id.index() ) );
        }
        text.push_back( '\n' );

        for( const auto id : program.order() )
        {
            const auto* const step = program.find( id );
            if( step == nullptr )
            {
                continue;
            }
            text.append( "step " );
            text.append( std::to_string( id.index() ) );
            text.append( " '" );
            text.append( step->label );
            text.append( "' " );
            text.append(
                grab::command_name( grab::sequence::kind_of( step->command ) )
            );
            text.append( " after=[" );
            bool first = true;
            for( const auto predecessor : step->after )
            {
                if( !first )
                {
                    text.push_back( ',' );
                }
                text.append( std::to_string( predecessor.index() ) );
                first = false;
            }
            text.append( "]\n" );
        }

        // Recomputed under the EFFECTIVE pacing, never read from the document:
        // printing the document's figure while running under --pacing grace
        // would report a plan the run will not follow.
        const auto plan = std::chrono::duration_cast<std::chrono::milliseconds>(
            grab::kernel::sequence::planned( program, pacing )
        );
        text.append( "plan: >= " );
        text.append( std::to_string( plan.count() ) );
        text.append( " ms, " );
        text.append(
            std::to_string( grab::kernel::sequence::unestimated_steps( program ) )
        );
        text.append( " steps unestimated\n" );
        return text;
    }

    std::vector<std::string>
    report_records( const Sequence&                       program,
                    const grab::kernel::sequence::Player& player )
    {
        std::vector<std::string> records;
        records.reserve( program.steps().size() );

        const std::string run = player.run_id().to_string();
        for( const auto id : program.order() )
        {
            const auto* const step = program.find( id );
            if( step == nullptr )
            {
                continue;
            }
            const auto  status = player.status_of( id );
            const auto  timing = player.timing_of( id );

            OrderedJson receipt{
                {        "commit",
                 std::string{
                 grab::detail::commit_status_name.text_of( commit_of( status ),
                 "failed_before_commit" )
                 }                                                            },
                {   "retry_class",
                 std::string{ grab::detail::retry_class_name.text_of(
                 grab::kernel::sequence::retry_class_of_step( *step ),
                 "never"
                 ) }                                                          },
                // Run-level: the Player reports one neutralization outcome for
                // the whole unwind rather than one per step.
                {"neutralization",
                 std::string{ neutralization_name( player.neutralization() ) }},
            };

            OrderedJson record{
                {       "run",                         run                              },
                {  "sequence",                             std::string{ program.name() }},
                {      "step",                                                id.index()},
                {     "label",                                               step->label},
                {        "op",
                 std::string{
                 grab::command_name( grab::sequence::kind_of( step->command ) )
                 }                                                                      },
                {    "status", std::string{ grab::sequence::step_status_name( status ) }},
                {   "call_ns",                              timing.call_duration.count()},
                {"overrun_ns",                           player.overrun_of( id ).count()},
                {   "receipt",                                      std::move( receipt )},
            };
            if( timing.declared.has_value() )
            {
                record["declared_ns"] = timing.declared->count();
            }
            else
            {
                record["declared_ns"] = nullptr;
            }
            records.push_back( record.dump() );
        }
        return records;
    }

    grab::Result<void>
    write_report( const std::string&              path,
                  const std::vector<std::string>& records )
    {
        std::ofstream stream{ std::filesystem::path{ path }, std::ios::trunc };
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "failed to open report: " + path );
        }
        for( const auto& record : records )
        {
            stream << record << '\n';
        }
        if( !stream )
        {
            return grab::fail( grab::ErrorCode::DeviceInaccessible,
                               "failed to write report: " + path );
        }
        return {};
    }

    grab::Result<void>
    drive( grab::kernel::sequence::Player& player )
    {
        std::optional<grab::kernel::scheduling::TimerThread> timers;

        auto                                                 started = player.play();
        if( !started.has_value() )
        {
            return started;
        }

        grab::Result<void> outcome{};
        while( true )
        {
            auto pumped = player.pump( Clock::now() );
            if( !pumped.has_value() )
            {
                outcome = pumped;
                break;
            }
            const auto state = player.state();
            if( state !=
                grab::sequence::PlayState::Playing &&
                state != grab::sequence::PlayState::Paused )
            {
                break;
            }
            // A signal is an unwind, not a stop: interrupt() exits every
            // entered step in reverse and reaps the holds the completed ones
            // left down, which is the only thing that lifts a pointer capture
            // taken between overlay.grab and an overlay.release that is now
            // never going to run.
            if( interruptRequested != 0 )
            {
                ( void )player.interrupt();
                outcome = grab::fail( grab::ErrorCode::Cancelled,
                                      "the run was interrupted by a signal" );
                break;
            }

            const auto now      = Clock::now();
            const auto deadline = player.next_deadline();
            // The frontier is non-empty while Playing, and every member is
            // either Ready or Running, so both of those carry a candidate.
            // Reaching here without one means the run cannot advance on its
            // own, which is a stall rather than a wait.
            if( !deadline.has_value() )
            {
                outcome = grab::fail( grab::ErrorCode::InternalFault,
                                      "the run stalled: no step can advance" );
                break;
            }

            const auto wake = *deadline > now ? *deadline : now + opaquePollPeriod;
            if( !timers.has_value() )
            {
                timers.emplace();
            }
            const int descriptor = timers->wake_fd();
            if( descriptor < 0 )
            {
                outcome = grab::fail( grab::ErrorCode::ProviderFailed,
                                      "the timer thread has no wake descriptor, so "
                                      "the run cannot be paced" );
                break;
            }
            const auto token = timers->arm( wake );
            wait_readable( descriptor,
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               wake - Clock::now()
                           ) );
            timers->cancel( token );
            ( void )timers->drain();
        }

        // Whatever ended the loop, nothing may stay entered: an entered step
        // may hold a button, and interrupt() is the path that exits it.
        if( player.state() ==
            grab::sequence::PlayState::Playing ||
            player.state() == grab::sequence::PlayState::Paused )
        {
            ( void )player.interrupt();
        }
        return outcome;
    }

    int
    play_program( const Sequence&                        program,
                  grab::kernel::sequence::CommandRunner& runner,
                  const PlayOptions&                     options )
    {
        grab::kernel::sequence::Player player{ program, runner };
        auto                           outcome  = drive( player );

        bool                           reported = true;
        if( !options.report.empty() )
        {
            auto written =
                write_report( options.report, report_records( program, player ) );
            if( !written.has_value() )
            {
                print_error( written.error().message );
                reported = false;
            }
        }

        if( !outcome.has_value() )
        {
            print_error( outcome.error().message );
            return runtimeExitCode;
        }
        if( !reported )
        {
            return runtimeExitCode;
        }
        if( player.state() != grab::sequence::PlayState::Done )
        {
            print_error( "the run did not reach the end of the document" );
            return runtimeExitCode;
        }
        return successExitCode;
    }

    grab::Result<void>
    play_single_command( grab::sequence::Command command,
                         const char*             display,
                         std::string_view        layout )
    {
        auto program = single_step_sequence( std::move( command ) );
        if( !program.has_value() )
        {
            return std::unexpected( std::move( program.error() ) );
        }
        auto seat = InputSeat::open( display, layout );
        if( !seat.has_value() )
        {
            return std::unexpected( std::move( seat.error() ) );
        }

        const InterruptTrap            trap;
        SeatRunner<InputSeat>          runner{ *seat };
        OutstandingHolds<InputSeat>    holds{ runner };
        grab::kernel::sequence::Player player{ *program, runner };
        auto                           outcome  = drive( player );
        const auto                     released = holds.release();
        if( outcome.has_value() && released == grab::NeutralizationOutcome::Failed )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "the command left a button, key or pointer "
                               "capture down and it could not be released" );
        }
        return outcome;
    }

    int
    run_play_command( std::span<const std::string_view> args )
    {
        auto options = parse_play_options( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            print_play_usage();
            return usageExitCode;
        }

        auto program =
            grab::kernel::sequence::load( std::filesystem::path{ options->document } );
        if( !program.has_value() )
        {
            print_error( program.error().message );
            return runtimeExitCode;
        }
        auto valid = grab::kernel::sequence::validate( *program );
        if( !valid.has_value() )
        {
            print_error( valid.error().message );
            return runtimeExitCode;
        }

        const auto pacing = effective_pacing( program->pacing(), *options );
        auto       paced  = with_pacing( *program, pacing );
        if( !paced.has_value() )
        {
            print_error( paced.error().message );
            return runtimeExitCode;
        }

        if( options->dry_run )
        {
            const auto text = dry_run_report( *paced, pacing );
            ( void )std::fwrite( text.data(), sizeof( char ), text.size(), stdout );
            return successExitCode;
        }

        auto seat = InputSeat::open( nullptr, std::string_view{} );
        if( !seat.has_value() )
        {
            print_error( seat.error().message );
            return runtimeExitCode;
        }

        // The trap is armed before the runner and disarmed after the holds are
        // lifted, so there is no window in which a signal can reach a run that
        // has already stopped tracking what it is holding.
        const InterruptTrap         trap;
        SeatRunner<InputSeat>       runner{ *seat };
        OutstandingHolds<InputSeat> holds{ runner };
        const int                   code     = play_program( *paced, runner, *options );
        const auto                  released = holds.release();

        log::nominal(
            [&paced, released]( auto& event )
            {
                event.tag( log::tags::player )
                    .value( "played", paced->name() )
                    .value( "outstanding", neutralization_name( released ) );
            }
        );

        if( released == grab::NeutralizationOutcome::Failed )
        {
            print_error( "the document left a button, key or pointer capture down "
                         "and it could not be released" );
            return runtimeExitCode;
        }
        return code;
    }

}    // namespace grab::cli
