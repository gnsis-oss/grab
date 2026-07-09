#include "grab/result.hpp"
#include "notify/notifier.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string>
// clang-format on

namespace
{

    constexpr const char*  kNotifyMember      = "Notify";
    constexpr const char*  kNotificationsPath = "/org/freedesktop/Notifications";
    constexpr const char*  kNotifySignature   = "susssasa{sv}i";
    constexpr const char*  kTestAppName       = "grab-test";
    constexpr const char*  kTestSummary       = "hi";
    constexpr const char*  kTestBody          = "b";
    constexpr std::int32_t kShortTimeoutMs    = 1;
    constexpr auto kDeviceInaccessibleCode    = grab::ErrorCode::device_inaccessible;

    [[nodiscard]]
    grab::notify::Notification
    test_notification()
    {
        return grab::notify::Notification{
            .app_name   = std::string{ kTestAppName },
            .summary    = std::string{ kTestSummary },
            .body       = std::string{ kTestBody },
            .icon       = {},
            .timeout_ms = kShortTimeoutMs,
        };
    }

    [[nodiscard]]
    bool
    bus_is_unavailable( const grab::Result<grab::notify::Notifier>& notifier )
    {
        return !notifier.has_value() && notifier.error().code == kDeviceInaccessibleCode;
    }

}    // namespace

TEST( Notifier,
      OpensSessionBusOrGracefullyFails )
{
    auto notifier = grab::notify::Notifier::open();

    if( bus_is_unavailable( notifier ) )
    {
        GTEST_SKIP() << notifier.error().message;
    }

    ASSERT_TRUE( notifier.has_value() ) << notifier.error().message;
}

TEST( Notifier,
      NotifyReturnsId )
{
    auto notifier = grab::notify::Notifier::open();

    if( bus_is_unavailable( notifier ) )
    {
        GTEST_SKIP() << notifier.error().message;
    }

    ASSERT_TRUE( notifier.has_value() ) << notifier.error().message;

    auto id = notifier->notify( test_notification() );
    ASSERT_TRUE( id.has_value() ) << id.error().message;

    auto closed = notifier->close( *id );
    EXPECT_TRUE( closed.has_value() ) << closed.error().message;
}

TEST( Notifier,
      BuildsValidNotifyMessage )
{
    auto message = grab::notify::build_notify_message( test_notification() );

    ASSERT_TRUE( message.has_value() ) << message.error().message;
    EXPECT_TRUE( dbus_message_has_member( message->get(), kNotifyMember ) );
    EXPECT_TRUE( dbus_message_has_path( message->get(), kNotificationsPath ) );
    EXPECT_TRUE( dbus_message_has_signature( message->get(), kNotifySignature ) );
}
