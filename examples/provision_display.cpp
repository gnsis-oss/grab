// provision_display — ask grab for a display it can honestly drive, print the
// report, and hold it until you press Enter.
//
// This is the whole consumer side of display provisioning. Everything a
// harness used to hand-roll — Xvfb, a window manager so clicks activate, a
// compositing manager so overlays draw, a session bus, an accessibility bus,
// and a teardown that kills exactly what it started — is one call:
//
//     provision_display({ .backend = DisplayBackend::Headless })
//
// Launch your own application into child_environment() and it lands on that
// display with the accessibility bridge switched on.
//
//     provision_display                 # headless, all preconditions
//     provision_display --nested        # a window on $DISPLAY you can watch
//     provision_display --attach :0     # report on a display already running
//
// --attach starts nothing: a second window manager on a desktop somebody is
// using would be destructive, so that mode reports rather than imposes.

#include "grab/provisioning.hpp"
#include "grab/result.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    void
    report( std::string_view          precondition,
            const grab::Result<void>& state )
    {
        std::cout << "  " << precondition << ( state.has_value() ? "  ok" : "  --" )
                  << '\n';
        if( !state.has_value() )
        {
            std::cout << "      " << state.error().message << '\n';
        }
    }

}    // namespace

int
main( int    argc,
      char** argv )
{
    const std::vector<std::string> arguments{ argv + 1, argv + argc };

    grab::DisplayRequest           request;
    for( std::size_t index = 0U; index < arguments.size(); ++index )
    {
        if( arguments[index] == "--nested" )
        {
            request.backend = grab::DisplayBackend::Nested;
        }
        else if( arguments[index] == "--attach" && index + 1U < arguments.size() )
        {
            request.backend = grab::DisplayBackend::Existing;
            request.display = arguments[++index];
        }
        else
        {
            std::cerr << "usage: provision_display [--nested] [--attach :N]\n";
            return 2;
        }
    }

    auto provisioned = grab::provision_display( request );
    if( !provisioned.has_value() )
    {
        // The failure names what to install rather than leaving the caller to
        // discover it as "the click did nothing" hours later.
        std::cerr << "cannot provision a display: " << provisioned.error().message
                  << '\n';
        for( const auto& attempt : provisioned.error().attempts )
        {
            std::cerr << "  tried " << attempt.provider << ": " << attempt.reason
                      << '\n';
        }
        return 1;
    }

    std::cout << "display " << provisioned->name() << '\n';
    report( "window manager   ", provisioned->window_manager() );
    report( "compositor       ", provisioned->compositor() );
    report( "accessibility bus", provisioned->accessibility() );

    std::cout << "\nchild environment:\n";
    for( const auto& [key, value] : provisioned->child_environment() )
    {
        std::cout << "  " << key << "=" << value << '\n';
    }

    std::cout << "\nPress Enter to tear it down." << std::endl;
    std::string line;
    static_cast<void>( std::getline( std::cin, line ) );
    return 0;
}
