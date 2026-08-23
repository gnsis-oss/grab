#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The seat `grab play` and the public sequence API (grab/sequence.hpp) drive:
// grab::Input, widened to the concepts in kernel/sequence/execute.hpp. Moved
// out of the CLI so both surfaces run commands through exactly one seat.
//
// grab::input::Seat spells keys by KEYCODE, and the name-to-keycode step
// needs a Keymap that lives above the seat -- so the adapter sits on
// grab::Input, which already exposes key_down/key_up by name, one rung higher
// than the raw seat.
//
// THE OVERLAY SURFACE HAS TWO SOURCES. Left to itself the seat opens a
// Session LAZILY, on the first overlay step -- a document that draws nothing
// must not pay for a session, and `grab click` (which routes through this
// same seat as a one-step document) must not start failing on a display with
// no compositing manager. An embedder that already holds a live Session
// binds it with bind_session() instead, and every overlay step then lands on
// THAT session's surface -- which is the whole point of playing a sequence
// against a session rather than beside it.
//
// This header is internal to the library. The public surface is
// include/grab/sequence.hpp.

#include "codec/png.hpp"
#include "grab/geometry/point.hpp"
#include "grab/input.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "kernel/sequence/execute.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace grab::sequence
{

    [[nodiscard]]
    inline grab::Result<void>
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
            stream.put( static_cast<char>( std::to_integer<unsigned char>( value ) ) );
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
                                using Element = std::remove_cvref_t<decltype( element )>;
                                if constexpr( std::is_same_v<Element,
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

    inline void
    stamp_space( grab::overlay::Geometry& geometry,
                 grab::CoordinateSpaceId  space )
    {
        for_each_point( geometry,
                        [space]( double&, double&, grab::CoordinateSpaceId& stamp )
                        {
                            stamp = space;
                        } );
    }

    inline void
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
    inline Anchor
    anchor_of( const grab::overlay::Geometry& geometry )
    {
        auto   copy = geometry;
        Anchor found{};
        bool   seen = false;
        for_each_point( copy,
                        [&found, &seen]( double& x, double& y, grab::CoordinateSpaceId& )
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
    class SessionSeat final
    {
        public:

            [[nodiscard]]
            static grab::Result<SessionSeat>
            open( const char*      display,
                  std::string_view layout )
            {
                auto input = grab::Input::open( display, layout );
                if( !input.has_value() )
                {
                    return std::unexpected( std::move( input.error() ) );
                }
                return SessionSeat{
                    std::move( *input ),
                    display == nullptr ? std::string{} : std::string{ display },
                };
            }

            // Route every overlay step onto a session the CALLER owns instead
            // of lazily opening one here. Non-owning: the caller's session
            // must outlive the seat. This is what play( Session&, ... ) is
            // for -- shapes land on the surface the embedder is already
            // looking at, not on a second session beside it.
            void
            bind_session( grab::Session& session ) noexcept
            {
                borrowed_ = &session;
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

            // ── Eager open, for --trail / --feedback ─────────
            //
            // Lazy is right for a document: one that draws nothing must not
            // pay for a session, and `grab click` must keep working on a
            // display with no compositing manager. It is WRONG for the
            // visual flags. A trail that starts when the document first
            // draws something has already missed every move before that, so
            // the first waypoints of a run would silently produce no
            // segments -- indistinguishable from `--trail` not working.
            //
            // The same session and the same overlay the overlay.* steps use:
            // opening a second pair would put the trail on one surface and
            // the document's shapes on another.
            [[nodiscard]]
            grab::Result<void>
            open_session()
            {
                auto surface = ensure_overlay();
                if( !surface.has_value() )
                {
                    return std::unexpected( std::move( surface.error() ) );
                }
                return {};
            }

            [[nodiscard]]
            grab::Session*
            session() noexcept
            {
                return borrowed_ != nullptr ? borrowed_ : session_.get();
            }

            [[nodiscard]]
            grab::Overlay*
            surface() noexcept
            {
                return overlay_;
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
                const auto found = std::ranges::find( shapes_, handle, &Placed::handle );
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
                if( borrowed_ != nullptr )
                {
                    // The embedder's surface, not a second one: opening a
                    // session here would draw the document's shapes beside
                    // the session the caller is playing against.
                    auto facade = borrowed_->overlay();
                    if( !facade.has_value() )
                    {
                        return std::unexpected( std::move( facade.error() ) );
                    }
                    auto bound_space = ( *facade )->space();
                    if( !bound_space.has_value() )
                    {
                        return std::unexpected( std::move( bound_space.error() ) );
                    }
                    overlay_ = *facade;
                    space_   = *bound_space;
                    log::nominal(
                        []( auto& event )
                        {
                            event.tag( log::tags::player ).value( "overlay", "bound" );
                        }
                    );
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
                                    display_.empty() ? "default" : display_.c_str() );
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
                    const auto   current  = anchor_of( entry.shape.geometry );
                    const double target_x = static_cast<double>( x ) + entry.offset.x;
                    const double target_y = static_cast<double>( y ) + entry.offset.y;
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

            SessionSeat( grab::Input input,
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
                    grab::Screen::open( display_.empty() ? nullptr : display_.c_str() );
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
            // The two overlay sources: a caller-owned session bound with
            // bind_session(), or the one this seat opened lazily. At most one
            // is ever set.
            grab::Session*                    borrowed_{ nullptr };
            std::unique_ptr<grab::Session>    session_{};
            grab::Overlay*                    overlay_{ nullptr };
            grab::CoordinateSpaceId           space_{};
            std::vector<Placed>               shapes_{};
    };

    static_assert( grab::kernel::sequence::PointerSeat<SessionSeat> );
    static_assert( grab::kernel::sequence::LocatingSeat<SessionSeat> );
    static_assert( grab::kernel::sequence::KeyboardSeat<SessionSeat> );
    static_assert( grab::kernel::sequence::TextSeat<SessionSeat> );
    static_assert( grab::kernel::sequence::CapturingSeat<SessionSeat> );
    static_assert( grab::kernel::sequence::OverlaySeat<SessionSeat> );

}    // namespace grab::sequence
