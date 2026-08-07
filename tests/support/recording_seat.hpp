#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Seat double for tests. execute_drag and the command executor are templated on
// SeatT, so this satisfies them without an X connection.

#include "grab/geometry/point.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace grab::testing
{

    struct SeatEvent
    {
            enum class Kind : std::uint8_t
            {
                Move,
                Button,
                Flush,
                Count,
            };

            Kind                                  kind{ Kind::Flush };
            std::int16_t                          x{};
            std::int16_t                          y{};
            std::uint8_t                          button{};
            bool                                  pressed{};
            std::chrono::steady_clock::time_point at{};
    };

    // One overlay call, flattened to what a display-free test can assert on.
    //
    // The whole Shape is not kept: a test that wants to know a circle was drawn
    // where it asked needs the geometry ALTERNATIVE and its principal point,
    // and keeping the variant would make every assertion a std::get_if. The
    // alternative index is recorded rather than an enum so a new Geometry
    // alternative cannot silently map onto an existing name.
    struct OverlayEvent
    {
            enum class Op : std::uint8_t
            {
                Add,
                Update,
                Remove,
                Clear,
                Grab,
                Release,
                Attach,
                Detach,
                Count,
            };

            static constexpr std::size_t noGeometry = static_cast<std::size_t>( -1 );

            Op                           op{ Op::Clear };
            std::string                  handle{};
            // std::variant_npos-like sentinel for the ops that carry no shape.
            std::size_t                  geometry{ noGeometry };
            // The principal point of that geometry: a rect's origin, an
            // ellipse's centre, a polygon's or path's first point. Zero when
            // there is no shape at all.
            double                       x{ 0.0 };
            double                       y{ 0.0 };
            std::optional<grab::geometry::Point>  offset{};
            std::chrono::steady_clock::time_point at{};
    };

    class RecordingSeat final
    {
        public:

            [[nodiscard]]
            grab::Result<void>
            move_pointer_absolute( std::int16_t x,
                                   std::int16_t y )
            {
                events_.push_back( SeatEvent{
                    .kind = SeatEvent::Kind::Move,
                    .x    = x,
                    .y    = y,
                    .at   = now_
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            button( std::uint8_t code,
                    bool         pressed )
            {
                events_.push_back( SeatEvent{
                    .kind    = SeatEvent::Kind::Button,
                    .button  = code,
                    .pressed = pressed,
                    .at      = now_
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            flush()
            {
                events_.push_back(
                    SeatEvent{ .kind = SeatEvent::Kind::Flush, .at = now_ }
                );
                return {};
            }

            // ── OverlaySeat ──────────────────────────────────
            //
            // Recorded, never drawn: three later units assert against these and
            // none of them may need a display. The seam is by handle because
            // the document names a shape before any scene exists.

            [[nodiscard]]
            grab::Result<void>
            overlay_add( std::string_view            handle,
                         const grab::overlay::Shape& shape )
            {
                record_shape( OverlayEvent::Op::Add, handle, shape );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_update( std::string_view            handle,
                            const grab::overlay::Shape& shape )
            {
                record_shape( OverlayEvent::Op::Update, handle, shape );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_remove( std::string_view handle )
            {
                record_handle( OverlayEvent::Op::Remove, handle );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_clear()
            {
                record_handle( OverlayEvent::Op::Clear, {} );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_grab()
            {
                record_handle( OverlayEvent::Op::Grab, {} );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_release()
            {
                record_handle( OverlayEvent::Op::Release, {} );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_attach( std::string_view                     handle,
                            std::optional<grab::geometry::Point> offset )
            {
                overlay_events_.push_back( OverlayEvent{
                    .op       = OverlayEvent::Op::Attach,
                    .handle   = std::string{ handle },
                    .geometry = OverlayEvent::noGeometry,
                    .x        = 0.0,
                    .y        = 0.0,
                    .offset   = offset,
                    .at       = now_,
                } );
                return {};
            }

            [[nodiscard]]
            grab::Result<void>
            overlay_detach( std::string_view handle )
            {
                record_handle( OverlayEvent::Op::Detach, handle );
                return {};
            }

            void
            set_now( std::chrono::steady_clock::time_point now ) noexcept
            {
                now_ = now;
            }

            [[nodiscard]]
            const std::vector<SeatEvent>&
            events() const noexcept
            {
                return events_;
            }

            [[nodiscard]]
            const std::vector<OverlayEvent>&
            overlay_events() const noexcept
            {
                return overlay_events_;
            }

        private:

            // A rect's origin, an ellipse's centre, a polygon's or a path's
            // first named point. Enough to tell "it drew where I asked" from
            // "it drew somewhere", which is what the assertions need.
            [[nodiscard]]
            static grab::SpacePoint
            principal_point( const grab::overlay::Geometry& geometry )
            {
                grab::SpacePoint point{};
                std::visit(
                    [&point]( const auto& figure )
                    {
                        using Figure = std::remove_cvref_t<decltype( figure )>;
                        if constexpr( std::is_same_v<Figure, grab::overlay::Rect> )
                        {
                            point.x     = figure.bounds.x;
                            point.y     = figure.bounds.y;
                            point.space = figure.bounds.space;
                        }
                        else if constexpr( std::is_same_v<Figure,
                                                          grab::overlay::Ellipse> )
                        {
                            point = figure.center;
                        }
                        else if constexpr( std::is_same_v<Figure,
                                                          grab::overlay::Polygon> )
                        {
                            if( !figure.points.empty() )
                            {
                                point = figure.points.front();
                            }
                        }
                        else
                        {
                            for( const auto& command : figure.commands )
                            {
                                if( const auto* const moved =
                                        std::get_if<grab::overlay::MoveTo>( &command ) )
                                {
                                    point = moved->point;
                                    return;
                                }
                                if( const auto* const lined =
                                        std::get_if<grab::overlay::LineTo>( &command ) )
                                {
                                    point = lined->point;
                                    return;
                                }
                                if( const auto* const curved =
                                        std::get_if<grab::overlay::BezierTo>( &command );
                                    curved != nullptr && !curved->control.empty() )
                                {
                                    point = curved->control.front();
                                    return;
                                }
                            }
                        }
                    },
                    geometry
                );
                return point;
            }

            void
            record_shape( OverlayEvent::Op            op,
                          std::string_view            handle,
                          const grab::overlay::Shape& shape )
            {
                const auto point = principal_point( shape.geometry );
                overlay_events_.push_back( OverlayEvent{
                    .op       = op,
                    .handle   = std::string{ handle },
                    .geometry = shape.geometry.index(),
                    .x        = point.x,
                    .y        = point.y,
                    .offset   = std::nullopt,
                    .at       = now_,
                } );
            }

            void
            record_handle( OverlayEvent::Op op,
                           std::string_view handle )
            {
                overlay_events_.push_back( OverlayEvent{
                    .op       = op,
                    .handle   = std::string{ handle },
                    .geometry = OverlayEvent::noGeometry,
                    .x        = 0.0,
                    .y        = 0.0,
                    .offset   = std::nullopt,
                    .at       = now_,
                } );
            }

            std::vector<SeatEvent>                events_;
            std::vector<OverlayEvent>             overlay_events_;
            std::chrono::steady_clock::time_point now_{};
    };

}    // namespace grab::testing
