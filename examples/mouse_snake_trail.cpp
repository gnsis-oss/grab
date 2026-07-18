#include "grab/image.hpp"
#include "grab/input.hpp"
#include "grab/result.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <thread>
#include <utility>

namespace
{

    // Sweep geometry. The pass count is odd so the final pass travels
    // left->right and the sweep ends at the inset bottom-right corner.
    constexpr std::int32_t              screenMarginPx = 40;
    constexpr std::size_t               snakePassCount = 9U;
    constexpr std::int32_t              sweepStepPx    = 12;
    constexpr std::chrono::milliseconds sweepStepInterval{ 6 };

    struct PixelPoint
    {
            std::int32_t x = 0;
            std::int32_t y = 0;

            friend constexpr bool
            operator==( const PixelPoint&,
                        const PixelPoint& ) = default;
    };

    [[nodiscard]]
    constexpr std::array<PixelPoint,
                         snakePassCount * 2U>
    snake_waypoints( std::int32_t width,
                     std::int32_t height )
    {
        std::array<PixelPoint, snakePassCount * 2U> points{};
        const std::int32_t                          left   = screenMarginPx;
        const std::int32_t                          right  = width - screenMarginPx;
        const std::int32_t                          top    = screenMarginPx;
        const std::int32_t                          bottom = height - screenMarginPx;
        const std::int32_t                          spanY  = bottom - top;
        const auto lastPass = static_cast<std::int32_t>( snakePassCount ) - 1;
        for( std::size_t pass = 0U; pass < snakePassCount; ++pass )
        {
            const std::int32_t row =
                top + ( ( static_cast<std::int32_t>( pass ) * spanY ) / lastPass );
            const bool rightward = pass % 2U == 0U;
            points.at( pass * 2U ) =
                PixelPoint{ .x = rightward ? left : right, .y = row };
            points.at( ( pass * 2U ) + 1U ) =
                PixelPoint{ .x = rightward ? right : left, .y = row };
        }
        return points;
    }

    // Compile-time contract on a reference display: the sweep starts at the
    // inset top-left corner, ends at the inset bottom-right corner, and
    // alternates direction (pass 0 rightward, pass 1 leftward).
    constexpr std::int32_t referenceWidth  = 1'920;
    constexpr std::int32_t referenceHeight = 1'080;
    constexpr auto         referenceWaypoints =
        snake_waypoints( referenceWidth, referenceHeight );
    static_assert( snakePassCount % 2U == 1U,
                   "odd pass count so the final pass ends bottom-right" );
    static_assert( referenceWaypoints.front() == PixelPoint{
                                                     .x = screenMarginPx,
                                                     .y = screenMarginPx,
                                                 } );
    static_assert( referenceWaypoints.back() ==
                   PixelPoint{
                       .x = referenceWidth - screenMarginPx,
                       .y = referenceHeight - screenMarginPx,
                   } );
    static_assert( referenceWaypoints.at( 1U ).x == referenceWidth - screenMarginPx );
    static_assert( referenceWaypoints.at( 2U ).x == referenceWidth - screenMarginPx );
    static_assert( referenceWaypoints.at( 3U ).x == screenMarginPx );

    [[nodiscard]]
    grab::Result<void>
    move_line( grab::Input&      input,
               const PixelPoint& from,
               const PixelPoint& to )
    {
        const std::int32_t deltaX   = to.x - from.x;
        const std::int32_t deltaY   = to.y - from.y;
        const std::int32_t distance = std::max( std::abs( deltaX ), std::abs( deltaY ) );
        const std::int32_t steps    = std::max( distance / sweepStepPx, 1 );
        for( std::int32_t step = 1; step <= steps; ++step )
        {
            const std::int32_t x     = from.x + ( ( deltaX * step ) / steps );
            const std::int32_t y     = from.y + ( ( deltaY * step ) / steps );
            auto               moved = input.move( static_cast<std::int16_t>( x ),
                                                   static_cast<std::int16_t>( y ) );
            if( !moved.has_value() )
            {
                return moved;
            }
            std::this_thread::sleep_for( sweepStepInterval );
        }
        return {};
    }

    [[nodiscard]]
    grab::Result<void>
    run()
    {
        auto session = grab::Session::open();
        if( !session.has_value() )
        {
            return std::unexpected( std::move( session.error() ) );
        }

        auto screen = grab::Screen::open();
        if( !screen.has_value() )
        {
            return std::unexpected( std::move( screen.error() ) );
        }
        auto frame = screen->display();
        if( !frame.has_value() )
        {
            return std::unexpected( std::move( frame.error() ) );
        }
        const auto width  = static_cast<std::int32_t>( frame->width );
        const auto height = static_cast<std::int32_t>( frame->height );

        auto       input  = grab::Input::open();
        if( !input.has_value() )
        {
            return std::unexpected( std::move( input.error() ) );
        }

        const auto waypoints = snake_waypoints( width, height );
        auto placed = input->move( static_cast<std::int16_t>( waypoints.front().x ),
                                   static_cast<std::int16_t>( waypoints.front().y ) );
        if( !placed.has_value() )
        {
            return placed;
        }
        for( std::size_t index = 1U; index < waypoints.size(); ++index )
        {
            auto swept =
                move_line( *input, waypoints.at( index - 1U ), waypoints.at( index ) );
            if( !swept.has_value() )
            {
                return swept;
            }
        }

        ( *session )->close();
        return {};
    }

}    // namespace

int
main()
{
    auto result = run();
    if( !result.has_value() )
    {
        std::cerr << "mouse_snake_trail: " << result.error().message << '\n';
        return 1;
    }
    return 0;
}
