#include "drivers/desktop/x11/xcb_reply.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdlib>
// clang-format on

namespace
{

    struct ReplyProbe
    {
            int value = 0;
    };

    constexpr int probe_value = 37;

}    // namespace

TEST( XcbReply,
      WrapsPointerWithFreeDeleter )
{
    ReplyProbe probe{ .value = probe_value };
    auto       reply = grab::platform::x11::make_xcb_reply( &probe );

    EXPECT_EQ( reply.get(), &probe );
    EXPECT_EQ( reply.get_deleter(), &std::free );

    ReplyProbe* const released = reply.release();
    EXPECT_EQ( released, &probe );
}

TEST( XcbReply,
      WrapsNullPointer )
{
    auto reply = grab::platform::x11::make_xcb_reply<ReplyProbe>( nullptr );

    EXPECT_EQ( reply, nullptr );
    EXPECT_EQ( reply.get_deleter(), &std::free );
}
