#include "compat/eventgrab_v1/browser_evidence.hpp"
#include "compat/eventgrab_v1/browser_projection.hpp"
#include "eventgrab/v1/events.pb.h"
#include "grab/pid.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grab::compat::eventgrab_v1
{

    std::optional<BrowserTabProjection>
    project_active_child_change( const grab::GraphChange& active_child,
                                 std::string_view         current_app,
                                 grab::Pid                current_pid,
                                 std::string_view         current_title,
                                 std::string_view         previous_title )
    {
        static_cast<void>( active_child );

        if( !is_browser_app( current_app ) ||
            current_title.empty() ||
            current_title == previous_title )
        {
            return std::nullopt;
        }

        return BrowserTabProjection{
            .app            = std::string{ current_app },
            .pid            = current_pid,
            .tab_title      = std::string{ current_title },
            .prev_tab_title = std::string{ previous_title },
        };
    }

    eventgrab::v1::Event
    to_wire( const BrowserTabProjection& projection )
    {
        eventgrab::v1::Event wire;
        wire.set_kind( eventgrab::v1::BROWSER_TAB_SWITCHED );
        wire.set_category( eventgrab::v1::EVENT_CATEGORY_BROWSER );

        ( *wire.mutable_data() )["app"]            = projection.app;
        ( *wire.mutable_data() )["pid"]            = projection.pid.to_string();
        ( *wire.mutable_data() )["tab_title"]      = projection.tab_title;
        ( *wire.mutable_data() )["prev_tab_title"] = projection.prev_tab_title;
        return wire;
    }

    grab::Result<BrowserTabProjection>
    from_wire( const eventgrab::v1::Event& wire )
    {
        const auto& data = wire.data();
        if( !data.contains( "app" ) )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               "eventgrab.v1 browser.tab_switched missing app" );
        }
        if( !data.contains( "pid" ) )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               "eventgrab.v1 browser.tab_switched missing pid" );
        }
        if( !data.contains( "tab_title" ) )
        {
            return grab::fail( grab::ErrorCode::ProtocolError,
                               "eventgrab.v1 browser.tab_switched missing tab_title" );
        }

        std::string previous_title;
        if( data.contains( "prev_tab_title" ) )
        {
            previous_title = data.at( "prev_tab_title" );
        }

        return BrowserTabProjection{
            .app            = data.at( "app" ),
            .pid            = grab::Pid::from_string( data.at( "pid" ) ),
            .tab_title      = data.at( "tab_title" ),
            .prev_tab_title = std::move( previous_title ),
        };
    }

}    // namespace grab::compat::eventgrab_v1
