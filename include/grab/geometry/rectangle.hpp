#pragma once

#include "grab/geometry/point.hpp"
#include "grab/geometry/size.hpp"

#include <array>
#include <cstdint>

namespace grab::geometry
{

    struct Rectangle
    {
            std::int32_t            x               = 0;
            std::int32_t            y               = 0;
            std::uint32_t           width           = 0U;
            std::uint32_t           height          = 0U;

            static constexpr double center_fraction = 0.5;

            [[nodiscard]]
            constexpr Point
            origin() const noexcept
            {
                return Point{
                    .x = x,
                    .y = y,
                };
            }

            [[nodiscard]]
            constexpr Size
            size() const noexcept
            {
                return Size{
                    .width  = width,
                    .height = height,
                };
            }

            [[nodiscard]]
            Point
            center() const noexcept
            {
                return point_at_fraction( center_fraction, center_fraction );
            }

            [[nodiscard]]
            constexpr std::int32_t
            right() const noexcept
            {
                return detail::to_coord( static_cast<std::int64_t>( x ) +
                                         static_cast<std::int64_t>( width ) );
            }

            [[nodiscard]]
            constexpr std::int32_t
            bottom() const noexcept
            {
                return detail::to_coord( static_cast<std::int64_t>( y ) +
                                         static_cast<std::int64_t>( height ) );
            }

            [[nodiscard]]
            constexpr bool
            contains( Point point ) const noexcept
            {
                const auto point_x     = static_cast<std::int64_t>( point.x );
                const auto point_y     = static_cast<std::int64_t>( point.y );
                const auto left        = static_cast<std::int64_t>( x );
                const auto top         = static_cast<std::int64_t>( y );
                const auto rect_right  = left + static_cast<std::int64_t>( width );
                const auto rect_bottom = top + static_cast<std::int64_t>( height );
                return point_x >=
                       left &&
                       point_x <
                       rect_right &&
                       point_y >=
                       top &&
                       point_y < rect_bottom;
            }

            [[nodiscard]]
            Point
            point_at_fraction( double fx,
                               double fy ) const noexcept
            {
                return Point{
                    .x = detail::to_coord( static_cast<std::int64_t>( x ) +
                                           static_cast<std::int64_t>( round_half_even(
                                               static_cast<double>( width ) * fx
                                           ) ) ),
                    .y = detail::to_coord( static_cast<std::int64_t>( y ) +
                                           static_cast<std::int64_t>( round_half_even(
                                               static_cast<double>( height ) * fy
                                           ) ) ),
                };
            }

            [[nodiscard]]
            constexpr Point
            point_at_offset( std::int32_t ox,
                             std::int32_t oy ) const noexcept
            {
                return Point{
                    .x = detail::to_coord( static_cast<std::int64_t>( x ) +
                                           static_cast<std::int64_t>( ox ) ),
                    .y = detail::to_coord( static_cast<std::int64_t>( y ) +
                                           static_cast<std::int64_t>( oy ) ),
                };
            }

            [[nodiscard]]
            constexpr std::array<Point,
                                 4U>
            corners() const noexcept
            {
                return std::array<Point, 4U>{
                    origin(),
                    Point{.x = right(),        .y = y},
                    Point{.x = right(), .y = bottom()},
                    Point{      .x = x, .y = bottom()},
                };
            }

            [[nodiscard]]
            friend constexpr bool
            operator==( Rectangle lhs,
                        Rectangle rhs ) noexcept = default;
    };

}    // namespace grab::geometry
