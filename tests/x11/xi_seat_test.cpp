#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xi_seat.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string_view>
// clang-format on

namespace
{

    using grab::platform::x11::XcbConnection;
    using grab::platform::x11::XiSeat;

    constexpr std::string_view seat_name = "grab-session-test";
    constexpr std::int16_t     warp_x    = 137;
    constexpr std::int16_t     warp_y    = 251;
    constexpr double           tolerance = 2.0;

}    // namespace

TEST( XiSeat,
      CreateExposesDistinctMasterAndCleansUp )
{
    auto conn = XcbConnection::open( "" );
    if( !conn.has_value() )
    {
        GTEST_SKIP() << "no X display";
    }

    auto seat = XiSeat::create( *conn, seat_name );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;
    EXPECT_NE( seat->pointer_id(), 0U );
    EXPECT_NE( seat->keyboard_id(), 0U );
    EXPECT_NE( seat->pointer_id(), seat->primary_pointer_id() );
    // Destructor removes the master when `seat` leaves scope.
}

TEST( XiSeat,
      WarpMovesNewSeatNotPrimary )
{
    auto conn = XcbConnection::open( "" );
    if( !conn.has_value() )
    {
        GTEST_SKIP() << "no X display";
    }

    auto seat = XiSeat::create( *conn, seat_name );
    ASSERT_TRUE( seat.has_value() ) << seat.error().message;

    const std::uint16_t primary = seat->primary_pointer_id();
    const auto          before  = seat->query( primary );
    ASSERT_TRUE( before.has_value() ) << before.error().message;

    ASSERT_TRUE( seat->warp_to( warp_x, warp_y ).has_value() );

    const auto after    = seat->query( primary );
    const auto new_seat = seat->query( seat->pointer_id() );
    ASSERT_TRUE( after.has_value() ) << after.error().message;
    ASSERT_TRUE( new_seat.has_value() ) << new_seat.error().message;

    // Isolation: the human's pointer is untouched; our seat moved to target.
    EXPECT_NEAR( after->x, before->x, tolerance );
    EXPECT_NEAR( after->y, before->y, tolerance );
    EXPECT_NEAR( new_seat->x, static_cast<double>( warp_x ), tolerance );
    EXPECT_NEAR( new_seat->y, static_cast<double>( warp_y ), tolerance );
}
