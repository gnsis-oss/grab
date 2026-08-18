#include "grab/provisioning.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "session/display_probe.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    // The whole point of the API is that it needs no ambient display, so these
    // tests deliberately do not require one — they build the display they use.
    constexpr std::uint16_t testWidth  = 640U;
    constexpr std::uint16_t testHeight = 480U;

    [[nodiscard]]
    grab::DisplayRequest
    bare_headless()
    {
        return grab::DisplayRequest{
            .backend           = grab::DisplayBackend::Headless,
            .display           = std::nullopt,
            .width             = testWidth,
            .height            = testHeight,
            .host_display      = std::nullopt,
            .window_manager    = false,
            .compositor        = false,
            .session_bus       = false,
            .accessibility_bus = false,
        };
    }

    [[nodiscard]]
    grab::DisplayRequest
    full_headless()
    {
        auto request              = bare_headless();
        request.window_manager    = true;
        request.compositor        = true;
        request.session_bus       = true;
        request.accessibility_bus = true;
        return request;
    }

    [[nodiscard]]
    std::optional<std::string>
    value_of( const std::vector<std::pair<std::string,
                                          std::string>>& environment,
              std::string_view                           key )
    {
        const auto found = std::ranges::find_if( environment,
                                                 [key]( const auto& entry )
                                                 {
                                                     return entry.first == key;
                                                 } );
        if( found == environment.end() )
        {
            return std::nullopt;
        }
        return found->second;
    }

    // adopt_environment() is a change to the whole process, so a test that
    // makes one puts the variables back for whatever runs next.
    class EnvironmentGuard
    {
        public:

            explicit EnvironmentGuard( std::vector<std::string> names )
            {
                for( auto& name : names )
                {
                    const char* const value = std::getenv( name.c_str() );
                    saved_.emplace_back( std::move( name ),
                                         value == nullptr
                                             ? std::optional<std::string>{}
                                             : std::optional{ std::string{ value } } );
                }
            }

            EnvironmentGuard( const EnvironmentGuard& ) = delete;
            EnvironmentGuard&
            operator=( const EnvironmentGuard& )   = delete;
            EnvironmentGuard( EnvironmentGuard&& ) = delete;
            EnvironmentGuard&
            operator=( EnvironmentGuard&& ) = delete;

            ~EnvironmentGuard()
            {
                for( const auto& [name, value] : saved_ )
                {
                    if( value.has_value() )
                    {
                        static_cast<void>( ::setenv( name.c_str(), value->c_str(), 1 ) );
                    }
                    else
                    {
                        static_cast<void>( ::unsetenv( name.c_str() ) );
                    }
                }
            }

        private:

            std::vector<std::pair<std::string, std::optional<std::string>>> saved_;
    };

    [[nodiscard]]
    bool
    tool_installed( std::string_view name )
    {
        const std::string path = "/usr/bin/" + std::string{ name };
        return access( path.c_str(), X_OK ) == 0;
    }

}    // namespace

TEST( Provisioning,
      RejectsAccessibilityBusWithoutSessionBus )
{
    auto request              = bare_headless();
    request.accessibility_bus = true;

    const auto provisioned    = grab::provision_display( request );
    ASSERT_FALSE( provisioned.has_value() );
    EXPECT_EQ( provisioned.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( provisioned.error().message.find( "session_bus" ), std::string::npos );
}

TEST( Provisioning,
      RejectsZeroGeometry )
{
    auto request           = bare_headless();
    request.height         = 0U;

    const auto provisioned = grab::provision_display( request );
    ASSERT_FALSE( provisioned.has_value() );
    EXPECT_EQ( provisioned.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( Provisioning,
      RejectsEmptyDisplayName )
{
    auto request           = bare_headless();
    request.display        = std::string{};

    const auto provisioned = grab::provision_display( request );
    ASSERT_FALSE( provisioned.has_value() );
    EXPECT_EQ( provisioned.error().code, grab::ErrorCode::InvalidArgument );
}

TEST( Provisioning,
      HeadlessDisplayComesUpAndIsTornDown )
{
    std::string name;
    {
        auto provisioned = grab::provision_display( bare_headless() );
        ASSERT_TRUE( provisioned.has_value() ) << provisioned.error().message;

        name = std::string{ provisioned->name() };
        EXPECT_FALSE( name.empty() );
        EXPECT_EQ( name.front(), ':' );
        EXPECT_TRUE( grab::session::display_connectable( name ) );

        const auto environment = provisioned->child_environment();
        EXPECT_EQ( value_of( environment, "DISPLAY" ), name );
    }

    // Teardown kills what it started, by recorded pid: the display it created
    // is gone with the object.
    EXPECT_FALSE( grab::session::display_connectable( name ) );
}

// A bare display is one grab cannot honestly drive, and each query says so in
// its own words rather than returning a bare false.
TEST( Provisioning,
      BareDisplayReportsEveryMissingPrecondition )
{
    auto provisioned = grab::provision_display( bare_headless() );
    ASSERT_TRUE( provisioned.has_value() ) << provisioned.error().message;

    const auto manager = provisioned->window_manager();
    ASSERT_FALSE( manager.has_value() );
    EXPECT_EQ( manager.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_EQ( manager.error().capability, "mouse.click" );
    EXPECT_NE( manager.error().message.find( "activates nothing" ), std::string::npos );

    const auto compositing = provisioned->compositor();
    ASSERT_FALSE( compositing.has_value() );
    EXPECT_EQ( compositing.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_EQ( compositing.error().capability, "overlay" );
    // The same sentence the X11 overlay delegate reports when it refuses to
    // draw, so one condition reads as one condition.
    EXPECT_NE( compositing.error().message.find(
                   "X11 overlay requires an owned compositing manager selection"
               ),
               std::string::npos );
    EXPECT_NE( compositing.error().message.find( "_NET_WM_CM_S" ), std::string::npos );

    const auto accessibility = provisioned->accessibility();
    ASSERT_FALSE( accessibility.has_value() );
    EXPECT_EQ( accessibility.error().code, grab::ErrorCode::CapabilityUnavailable );
    EXPECT_NE( accessibility.error().message.find( "AT_SPI_BUS" ), std::string::npos );

    // A display without an accessibility bus does not claim to give a child
    // one either.
    EXPECT_FALSE(
        value_of( provisioned->child_environment(), "GTK_MODULES" ).has_value()
    );
}

TEST( Provisioning,
      FullyProvisionedDisplaySatisfiesEveryPrecondition )
{
    if( !tool_installed( "openbox" ) || !tool_installed( "xcompmgr" ) )
    {
        GTEST_SKIP() << "no window manager or compositing manager installed";
    }

    auto provisioned = grab::provision_display( full_headless() );
    ASSERT_TRUE( provisioned.has_value() ) << provisioned.error().message;

    EXPECT_TRUE( provisioned->window_manager().has_value() )
        << provisioned->window_manager().error().message;
    EXPECT_TRUE( provisioned->compositor().has_value() )
        << provisioned->compositor().error().message;
    EXPECT_TRUE( provisioned->accessibility().has_value() )
        << provisioned->accessibility().error().message;

    const auto environment = provisioned->child_environment();
    EXPECT_EQ( value_of( environment, "DISPLAY" ), std::string{ provisioned->name() } );

    // A child launched into this lands on the provisioned display AND on the
    // private session bus the accessibility bus sits on — not on whatever the
    // operator's session happens to be using.
    const auto bus = value_of( environment, "DBUS_SESSION_BUS_ADDRESS" );
    ASSERT_TRUE( bus.has_value() );
    EXPECT_NE( bus->find( "unix:path=" ), std::string::npos );
    EXPECT_EQ( value_of( environment, "GTK_MODULES" ), "gail:atk-bridge" );
    EXPECT_EQ( value_of( environment, "GNOME_ACCESSIBILITY" ), "1" );
}

TEST( Provisioning,
      RefusesADisplayThatIsAlreadyInUse )
{
    auto running = grab::provision_display( bare_headless() );
    ASSERT_TRUE( running.has_value() ) << running.error().message;

    auto request      = bare_headless();
    request.display   = std::string{ running->name() };

    const auto second = grab::provision_display( request );
    ASSERT_FALSE( second.has_value() );
    EXPECT_EQ( second.error().code, grab::ErrorCode::DeviceInaccessible );
    EXPECT_NE( second.error().message.find( "Existing" ), std::string::npos );
}

// Existing is the "drive the display the operator is already using" path:
// starting a second window manager or compositing manager there would be
// destructive, and tearing one down on the way out doubly so.
TEST( Provisioning,
      ExistingSpawnsNothingAndTearsNothingDown )
{
    if( !tool_installed( "openbox" ) || !tool_installed( "xcompmgr" ) )
    {
        GTEST_SKIP() << "no window manager or compositing manager installed";
    }

    auto host = grab::provision_display( full_headless() );
    ASSERT_TRUE( host.has_value() ) << host.error().message;
    const std::string name{ host->name() };

    {
        auto attached = grab::provision_display( grab::DisplayRequest{
            .backend      = grab::DisplayBackend::Existing,
            .display      = name,
            .host_display = std::nullopt,
        } );
        ASSERT_TRUE( attached.has_value() ) << attached.error().message;

        // Same query either way: what it reports here was started by somebody
        // else entirely.
        EXPECT_EQ( attached->name(), name );
        EXPECT_TRUE( attached->window_manager().has_value() );
        EXPECT_TRUE( attached->compositor().has_value() );
        EXPECT_TRUE( attached->accessibility().has_value() );
    }

    // The attached handle is gone; everything it reported on is still running.
    EXPECT_TRUE( grab::session::display_connectable( name ) );
    EXPECT_TRUE( host->window_manager().has_value() );
    EXPECT_TRUE( host->compositor().has_value() );
    EXPECT_TRUE( host->accessibility().has_value() );
}

TEST( Provisioning,
      ExistingReportsRatherThanFailsOnAMissingPrecondition )
{
    auto host = grab::provision_display( bare_headless() );
    ASSERT_TRUE( host.has_value() ) << host.error().message;

    auto attached = grab::provision_display( grab::DisplayRequest{
        .backend      = grab::DisplayBackend::Existing,
        .display      = std::string{ host->name() },
        .host_display = std::nullopt,
    } );
    // Attaching succeeds even though the desktop has none of the
    // preconditions; the caller learns that from the queries, not from a
    // refusal it can do nothing about.
    ASSERT_TRUE( attached.has_value() ) << attached.error().message;
    EXPECT_FALSE( attached->window_manager().has_value() );
    EXPECT_FALSE( attached->compositor().has_value() );
    EXPECT_FALSE( attached->accessibility().has_value() );
}

TEST( Provisioning,
      ExistingRefusesADisplayThatIsNotThere )
{
    const auto attached = grab::provision_display( grab::DisplayRequest{
        .backend      = grab::DisplayBackend::Existing,
        .display      = std::string{ ":991" },
        .host_display = std::nullopt,
    } );
    ASSERT_FALSE( attached.has_value() );
    EXPECT_EQ( attached.error().code, grab::ErrorCode::DisplayUnavailable );
}

// The nested backend opens its window on a display grab provisioned for the
// purpose — never on the operator's own, which no test may touch.
TEST( Provisioning,
      NestedDisplayOpensOnItsHost )
{
    if( !tool_installed( "Xephyr" ) )
    {
        GTEST_SKIP() << "Xephyr is not installed";
    }

    auto host = grab::provision_display( bare_headless() );
    ASSERT_TRUE( host.has_value() ) << host.error().message;

    std::string nested_name;
    {
        auto request         = bare_headless();
        request.backend      = grab::DisplayBackend::Nested;
        request.host_display = std::string{ host->name() };

        auto nested          = grab::provision_display( request );
        ASSERT_TRUE( nested.has_value() ) << nested.error().message;

        nested_name = std::string{ nested->name() };
        EXPECT_NE( nested_name, std::string{ host->name() } );
        EXPECT_TRUE( grab::session::display_connectable( nested_name ) );
    }
    EXPECT_FALSE( grab::session::display_connectable( nested_name ) );
    EXPECT_TRUE( grab::session::display_connectable( std::string{ host->name() } ) );
}

TEST( Provisioning,
      NestedWithoutAHostDisplayIsRejected )
{
    auto request         = bare_headless();
    request.backend      = grab::DisplayBackend::Nested;
    request.host_display = std::string{};

    // An empty host display is no host display, and the ambient DISPLAY is
    // whatever the test runner has — so the reason must name both ways out.
    const auto provisioned = grab::provision_display( request );
    if( provisioned.has_value() )
    {
        GTEST_SKIP() << "DISPLAY is set, so the ambient host display applied";
    }
    EXPECT_EQ( provisioned.error().code, grab::ErrorCode::InvalidArgument );
    EXPECT_NE( provisioned.error().message.find( "host_display" ), std::string::npos );
}

TEST( Provisioning,
      MovingTransfersOwnershipOfEverythingStarted )
{
    auto provisioned = grab::provision_display( bare_headless() );
    ASSERT_TRUE( provisioned.has_value() ) << provisioned.error().message;
    const std::string name{ provisioned->name() };

    {
        grab::ProvisionedDisplay moved{ std::move( *provisioned ) };
        EXPECT_EQ( moved.name(), name );
        EXPECT_TRUE( grab::session::display_connectable( name ) );

        // The moved-from handle owns nothing and says so rather than
        // pretending the display is still its own.
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
        EXPECT_TRUE( provisioned->name().empty() );
        EXPECT_FALSE( provisioned->window_manager().has_value() );
    }
    // The display went with the object that took ownership of it.
    EXPECT_FALSE( grab::session::display_connectable( name ) );
}

// Two displays at once is the ordinary case for a test suite, and the free
// number scan must not hand the same one to both.
TEST( Provisioning,
      TwoDisplaysDoNotCollide )
{
    auto first = grab::provision_display( bare_headless() );
    ASSERT_TRUE( first.has_value() ) << first.error().message;
    auto second = grab::provision_display( bare_headless() );
    ASSERT_TRUE( second.has_value() ) << second.error().message;

    EXPECT_NE( first->name(), second->name() );
    EXPECT_TRUE( grab::session::display_connectable( std::string{ first->name() } ) );
    EXPECT_TRUE( grab::session::display_connectable( std::string{ second->name() } ) );
}

// The composability claim: provision, then open a session on it, with the
// display name taken from the display rather than copied by the caller.
//
// Kept last in the file, and its environment restored, because adopting the
// display's environment is a change to this whole process.
TEST( Provisioning,
      SessionOpensOnAProvisionedDisplay )
{
    if( !tool_installed( "openbox" ) || !tool_installed( "xcompmgr" ) )
    {
        GTEST_SKIP() << "no window manager or compositing manager installed";
    }

    const EnvironmentGuard guard{
        { "DISPLAY", "DBUS_SESSION_BUS_ADDRESS" }
    };

    auto provisioned = grab::provision_display( full_headless() );
    ASSERT_TRUE( provisioned.has_value() ) << provisioned.error().message;

    auto session = grab::open_session( *provisioned );
    ASSERT_TRUE( session.has_value() ) << session.error().message;
    ASSERT_NE( *session, nullptr );
    EXPECT_TRUE( ( *session )->is_open() );

    // grab's own AT-SPI driver reads the accessibility bus named by this
    // process's environment, which is what open_session adopted.
    EXPECT_EQ( std::string{ std::getenv( "DISPLAY" ) },
               std::string{ provisioned->name() } );

    // The overlay is the precondition-sensitive facade: on a display with a
    // compositing manager it binds instead of degrading.
    const auto overlay = ( *session )->overlay();
    EXPECT_TRUE( overlay.has_value() )
        << ( overlay.has_value() ? std::string{} : overlay.error().message );

    ( *session )->close();
}
