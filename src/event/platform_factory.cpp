#include "event/atspi.hpp"
#include "event/browser_bridge.hpp"
#include "event/evdev.hpp"
#include "event/monitor_source.hpp"
#include "event/platform_factory.hpp"
#include "event/source.hpp"
#include "event/state_source.hpp"
#include "event/window_x11.hpp"
#include "event/xinput2.hpp"
#include "grab/event.hpp"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    constexpr auto             evdevKinds   = std::to_array<grab::EventKind>( {
        grab::EventKind::KeyDown,
        grab::EventKind::KeyUp,
        grab::EventKind::MouseMove,
    } );

    constexpr auto             xinputKinds  = std::to_array<grab::EventKind>( {
        grab::EventKind::KeyDown,
        grab::EventKind::KeyUp,
        grab::EventKind::MouseClick,
        grab::EventKind::MouseMove,
    } );

    constexpr auto             windowKinds  = std::to_array<grab::EventKind>( {
        grab::EventKind::WindowFocusChanged,
        grab::EventKind::WindowTitleChanged,
        grab::EventKind::WindowCreated,
        grab::EventKind::WindowClosed,
    } );

    constexpr auto             a11yKinds    = std::to_array<grab::EventKind>( {
        grab::EventKind::A11yButtonClicked,
        grab::EventKind::A11yMenuOpened,
        grab::EventKind::A11yMenuClosed,
        grab::EventKind::A11yFocusChanged,
        grab::EventKind::A11yTextChanged,
        grab::EventKind::A11yStateChanged,
    } );

    constexpr auto             browserKinds = std::to_array<grab::EventKind>( {
        grab::EventKind::AppTabChanged,
        grab::EventKind::AppContextUpdate,
        grab::EventKind::BrowserTabSwitched,
    } );

    constexpr std::string_view evdevName    = "evdev";
    constexpr std::string_view xinputName   = "xinput2";
    constexpr std::string_view windowName   = "window";
    constexpr std::string_view atspiName    = "atspi";
    constexpr std::string_view browserName  = "browser";

    [[nodiscard]]
    std::optional<std::string>
    make_display( const char* display )
    {
        if( display == nullptr )
        {
            return std::nullopt;
        }
        return std::string{ display };
    }

}    // namespace

namespace grab::event
{

    std::vector<std::unique_ptr<EventSource>>
    PlatformFactory::build( const SourceConfig& config )
    {
        std::vector<std::unique_ptr<EventSource>> sources;

        if( config.enable_input )
        {
            if( config.evdev_device.has_value() )
            {
                sources.push_back( std::make_unique<MonitorSource<EvdevMonitor>>(
                    evdevName,
                    std::span<const grab::EventKind>{ evdevKinds },
                    [path = config.evdev_device->string()]( grab::core::Reactor& reactor,
                                                            grab::EventBus&      bus )
                    {
                        return EvdevMonitor::open_device( path, reactor, bus );
                    }
                ) );
            }
            else
            {
                sources.push_back( std::make_unique<MonitorSource<XInput2Monitor>>(
                    xinputName,
                    std::span<const grab::EventKind>{ xinputKinds },
                    [disp =
                         make_display( config.display )]( grab::core::Reactor& reactor,
                                                          grab::EventBus&      bus )
                    {
                        return XInput2Monitor::start( disp.has_value() ? disp->c_str()
                                                                       : nullptr,
                                                      reactor,
                                                      bus );
                    }
                ) );
            }
        }

        if( config.enable_window )
        {
            sources.push_back( std::make_unique<MonitorSource<WindowTracker>>(
                windowName,
                std::span<const grab::EventKind>{ windowKinds },
                [disp = make_display( config.display ),
                 poll = config.window_poll]( grab::core::Reactor& reactor,
                                             grab::EventBus&      bus )
                {
                    return WindowTracker::start( disp.has_value() ? disp->c_str()
                                                                  : nullptr,
                                                 reactor,
                                                 bus,
                                                 poll );
                }
            ) );
        }

        if( config.enable_a11y )
        {
            sources.push_back( std::make_unique<MonitorSource<AtspiMonitor>>(
                atspiName,
                std::span<const grab::EventKind>{ a11yKinds },
                []( grab::core::Reactor& reactor, grab::EventBus& bus )
                {
                    return AtspiMonitor::start( reactor, bus );
                }
            ) );
        }

        if( config.enable_browser && config.browser_input_fd.has_value() )
        {
            sources.push_back( std::make_unique<MonitorSource<BrowserBridge>>(
                browserName,
                std::span<const grab::EventKind>{ browserKinds },
                [fd = *config.browser_input_fd]( grab::core::Reactor& reactor,
                                                 grab::EventBus&      bus )
                {
                    return BrowserBridge::start( fd, reactor, bus );
                }
            ) );
        }

        if( config.enable_state )
        {
            sources.push_back( std::make_unique<StateSource>( config.state_interval ) );
        }

        return sources;
    }

}    // namespace grab::event
