#include "grab/result.hpp"
#include "notify/notifier.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <string>
// clang-format on

namespace
{

    constexpr const char*  notifyMember           = "Notify";
    constexpr const char*  notificationsPath      = "/org/freedesktop/Notifications";
    constexpr const char*  notifySignature        = "susssasa{sv}i";
    constexpr const char*  testAppName            = "grab-test";
    constexpr const char*  testSummary            = "hi";
    constexpr const char*  testBody               = "b";
    constexpr std::int32_t shortTimeoutMs         = 1;
    constexpr auto         deviceInaccessibleCode = grab::ErrorCode::DeviceInaccessible;

    [[nodiscard]]
    grab::notify::Notification
    test_notification()
    {
        return grab::notify::Notification{
            .app_name   = std::string{ testAppName },
            .summary    = std::string{ testSummary },
            .body       = std::string{ testBody },
            .icon       = {},
            .timeout_ms = shortTimeoutMs,
        };
    }

    [[nodiscard]]
    bool
    bus_is_unavailable( const grab::Result<grab::notify::Notifier>& notifier )
    {
        return !notifier.has_value() && notifier.error().code == deviceInaccessibleCode;
    }

}    // namespace

TEST( NotifyOptions,
      DefaultsAreVisibleAndOverridable )
{
    constexpr auto customTimeout = std::chrono::milliseconds{ 250 };

    constexpr grab::notify::NotifyOptions defaults;
    EXPECT_EQ( defaults.timeout, grab::notify::NotifyOptions::defaultTimeout );
    EXPECT_EQ( defaults.timeout, std::chrono::milliseconds{ 2'000 } );

    constexpr grab::notify::NotifyOptions customized{
        .timeout = customTimeout,
    };
    EXPECT_EQ( customized.timeout, customTimeout );
}

TEST( NotifyOptions,
      RejectsTimeoutOutsideDbusRange )
{
    const auto notifier = grab::notify::Notifier::open( grab::notify::NotifyOptions{
        .timeout = std::chrono::milliseconds::max(),
    } );

    ASSERT_FALSE( notifier.has_value() );
    EXPECT_EQ( notifier.error().code, grab::ErrorCode::InvalidArgument );
}

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
    EXPECT_TRUE( dbus_message_has_member( message->get(), notifyMember ) );
    EXPECT_TRUE( dbus_message_has_path( message->get(), notificationsPath ) );
    EXPECT_TRUE( dbus_message_has_signature( message->get(), notifySignature ) );
}
