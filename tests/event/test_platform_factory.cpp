#include "core/reactor.hpp"
#include "event/platform_factory.hpp"
#include "event/source.hpp"
#include "grab/event.hpp"
#include "grab/event_bus.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
// clang-format on

namespace
{

    constexpr std::string_view evdevName       = "evdev";
    constexpr std::string_view xinputName      = "xinput2";
    constexpr std::string_view windowName      = "window";
    constexpr std::string_view atspiName       = "atspi";
    constexpr std::string_view browserName     = "browser";
    constexpr std::string_view stateName       = "state";

    constexpr std::string_view evdevDevicePath = "/dev/input/event0";

    // A well-formed display with no server behind it: XInput2Monitor::start
    // reads the string (via xcb_connect) then fails cleanly. Used to exercise
    // the closure's display copy under ASan.
    constexpr std::string_view unavailableDisplay = ":99";
    constexpr int              browserInputFd     = 12'345;

    constexpr auto             evdevKinds         = std::to_array<grab::EventKind>( {
        grab::EventKind::KeyDown,
        grab::EventKind::KeyUp,
        grab::EventKind::MouseMove,
    } );

    constexpr auto             xinputKinds        = std::to_array<grab::EventKind>( {
        grab::EventKind::KeyDown,
        grab::EventKind::KeyUp,
        grab::EventKind::MouseClick,
        grab::EventKind::MouseMove,
    } );

    constexpr auto             windowKinds        = std::to_array<grab::EventKind>( {
        grab::EventKind::WindowFocusChanged,
        grab::EventKind::WindowTitleChanged,
        grab::EventKind::WindowCreated,
        grab::EventKind::WindowClosed,
    } );

    constexpr auto             a11yKinds          = std::to_array<grab::EventKind>( {
        grab::EventKind::A11yButtonClicked,
        grab::EventKind::A11yMenuOpened,
        grab::EventKind::A11yMenuClosed,
        grab::EventKind::A11yFocusChanged,
        grab::EventKind::A11yTextChanged,
        grab::EventKind::A11yStateChanged,
    } );

    constexpr auto             browserKinds       = std::to_array<grab::EventKind>( {
        grab::EventKind::AppTabChanged,
        grab::EventKind::AppContextUpdate,
        grab::EventKind::BrowserTabSwitched,
    } );

    constexpr auto             stateKinds         = std::to_array<grab::EventKind>( {
        grab::EventKind::StateSnapshot,
    } );

    constexpr auto             defaultSourceNames = std::to_array<std::string_view>(
        { xinputName, windowName, atspiName, stateName }
    );

    using SourceList = std::vector<std::unique_ptr<grab::event::EventSource>>;

    [[nodiscard]]
    std::vector<std::string_view>
    source_names( const SourceList& sources )
    {
        std::vector<std::string_view> names;
        names.reserve( sources.size() );
        for( const auto& source : sources )
        {
            names.emplace_back( source->name() );
        }
        return names;
    }

    [[nodiscard]]
    const grab::event::EventSource*
    find_source( const SourceList& sources,
                 std::string_view  name )
    {
        const auto iter = std::ranges::find_if( sources,
                                                [name]( const auto& source )
                                                {
                                                    return source->name() == name;
                                                } );
        if( iter == sources.end() )
        {
            return nullptr;
        }
        return iter->get();
    }

    [[nodiscard]]
    bool
    has_source( const SourceList& sources,
                std::string_view  name )
    {
        return find_source( sources, name ) != nullptr;
    }

    void
    expect_kinds( const grab::event::EventSource&  source,
                  std::span<const grab::EventKind> expected )
    {
        const auto actual = source.kinds();
        ASSERT_EQ( actual.size(), expected.size() );
        EXPECT_TRUE( std::ranges::equal( actual, expected ) );
    }

}    // namespace

TEST( PlatformFactory,
      DefaultConfigProducesXinputWindowAtspi )
{
    const auto sources =
        grab::event::PlatformFactory::build( grab::event::SourceConfig{} );

    EXPECT_EQ( source_names( sources ),
               std::vector<std::string_view>( defaultSourceNames.begin(),
                                              defaultSourceNames.end() ) );

    const auto* xinput = find_source( sources, xinputName );
    ASSERT_NE( xinput, nullptr );
    expect_kinds( *xinput, xinputKinds );

    const auto* window = find_source( sources, windowName );
    ASSERT_NE( window, nullptr );
    expect_kinds( *window, windowKinds );

    const auto* atspi = find_source( sources, atspiName );
    ASSERT_NE( atspi, nullptr );
    expect_kinds( *atspi, a11yKinds );

    EXPECT_FALSE( has_source( sources, evdevName ) );
    EXPECT_FALSE( has_source( sources, browserName ) );

    const auto* state = find_source( sources, stateName );
    ASSERT_NE( state, nullptr );
    expect_kinds( *state, stateKinds );
}

TEST( PlatformFactory,
      EvdevDeviceSelectsEvdevNotXinput )
{
    grab::event::SourceConfig config;
    config.evdev_device = std::filesystem::path{ evdevDevicePath };

    const auto  sources = grab::event::PlatformFactory::build( config );

    const auto* evdev   = find_source( sources, evdevName );
    ASSERT_NE( evdev, nullptr );
    EXPECT_EQ( evdev->name(), evdevName );
    expect_kinds( *evdev, evdevKinds );
    EXPECT_FALSE( has_source( sources, xinputName ) );
}

TEST( PlatformFactory,
      BrowserAbsentWithoutFd )
{
    grab::event::SourceConfig config;
    config.enable_browser = true;

    const auto sources    = grab::event::PlatformFactory::build( config );

    EXPECT_FALSE( has_source( sources, browserName ) );
}

TEST( PlatformFactory,
      BrowserPresentWithFd )
{
    grab::event::SourceConfig config;
    config.enable_browser   = true;
    config.browser_input_fd = browserInputFd;

    const auto  sources     = grab::event::PlatformFactory::build( config );

    const auto* browser     = find_source( sources, browserName );
    ASSERT_NE( browser, nullptr );
    EXPECT_EQ( browser->name(), browserName );
    expect_kinds( *browser, browserKinds );
}

TEST( PlatformFactory,
      DisablingCategoriesDropsThem )
{
    grab::event::SourceConfig config;
    config.enable_input  = false;
    config.enable_window = false;
    config.enable_a11y   = false;
    config.enable_state  = false;

    const auto sources   = grab::event::PlatformFactory::build( config );

    EXPECT_FALSE( has_source( sources, evdevName ) );
    EXPECT_FALSE( has_source( sources, xinputName ) );
    EXPECT_FALSE( has_source( sources, windowName ) );
    EXPECT_FALSE( has_source( sources, atspiName ) );
    EXPECT_FALSE( has_source( sources, stateName ) );
}

TEST( PlatformFactory,
      StateSourceFollowsEnableState )
{
    grab::event::SourceConfig enabled;
    enabled.enable_input        = false;
    enabled.enable_window       = false;
    enabled.enable_a11y         = false;
    enabled.enable_browser      = false;
    enabled.enable_state        = true;

    const auto  enabled_sources = grab::event::PlatformFactory::build( enabled );
    const auto* state           = find_source( enabled_sources, stateName );
    ASSERT_NE( state, nullptr );
    expect_kinds( *state, stateKinds );

    grab::event::SourceConfig disabled;
    disabled.enable_input       = false;
    disabled.enable_window      = false;
    disabled.enable_a11y        = false;
    disabled.enable_browser     = false;
    disabled.enable_state       = false;

    const auto disabled_sources = grab::event::PlatformFactory::build( disabled );
    EXPECT_FALSE( has_source( disabled_sources, stateName ) );
}

TEST( PlatformFactory,
      KindsSpanOutlivesConfigTemporary )
{
    const auto sources =
        grab::event::PlatformFactory::build( grab::event::SourceConfig{} );

    EXPECT_EQ( source_names( sources ),
               std::vector<std::string_view>( defaultSourceNames.begin(),
                                              defaultSourceNames.end() ) );

    const auto* xinput = find_source( sources, xinputName );
    ASSERT_NE( xinput, nullptr );
    expect_kinds( *xinput, xinputKinds );

    const auto* window = find_source( sources, windowName );
    ASSERT_NE( window, nullptr );
    expect_kinds( *window, windowKinds );

    const auto* atspi = find_source( sources, atspiName );
    ASSERT_NE( atspi, nullptr );
    expect_kinds( *atspi, a11yKinds );

    const auto* state = find_source( sources, stateName );
    ASSERT_NE( state, nullptr );
    expect_kinds( *state, stateKinds );
}

TEST( PlatformFactory,
      BuildDoesNotThrowUnderEmptyConfig )
{
    grab::event::SourceConfig config;
    config.enable_input   = false;
    config.enable_window  = false;
    config.enable_a11y    = false;
    config.enable_browser = false;
    config.enable_state   = false;

    SourceList sources;
    EXPECT_NO_THROW( sources = grab::event::PlatformFactory::build( config ) );
    EXPECT_TRUE( sources.empty() );
}

// Regression guard for the display-lifetime crux: the xinput2 closure must
// capture a COPY of SourceConfig.display, not the raw const char*. Here the
// display string is destroyed before start() runs; a raw-pointer capture would
// read freed memory (caught by ASan). start() itself fails cleanly because no
// X server is reachable at the display.
TEST( PlatformFactory,
      DisplayCopySurvivesConfigTemporaryOnStart )
{
    grab::core::Reactor reactor;
    grab::EventBus      bus;

    SourceList          sources;
    {
        const std::string         display{ unavailableDisplay };
        grab::event::SourceConfig config;
        config.display = display.c_str();
        sources        = grab::event::PlatformFactory::build( config );
    }

    grab::event::EventSource* xinput = nullptr;
    for( auto& source : sources )
    {
        if( source->name() == xinputName )
        {
            xinput = source.get();
            break;
        }
    }
    ASSERT_NE( xinput, nullptr );

    // Reads the captured display; must not touch the freed temporary above.
    const auto result = xinput->start( reactor, bus );
    static_cast<void>( result );
}
