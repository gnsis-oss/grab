#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace grab::compat::eventgrab_v1
{

    // The eventgrab.v1 browser.tab_switched projection: a compat-local value that
    // replaces the removed core grab::BrowserTab. Derived (low-confidence) from an
    // active_child.changed graph delta plus the newly-active window's node properties;
    // encoded to / decoded from the frozen eventgrab.v1 wire.
    struct BrowserTabProjection
    {
            std::string app;
            grab::Pid   pid;
            std::string tab_title;
            std::string prev_tab_title;

            friend bool
            operator==( const BrowserTabProjection&,
                        const BrowserTabProjection& ) = default;
    };

    // Derive a browser tab-switch projection from an active_child.changed transition.
    // `active_child` is the ActiveChildChanged GraphChange; `current_*` are the
    // now-active window's node properties; `previous_title` is the prior active window's
    // title. Returns nullopt unless the current app looks like a browser (is_browser_app
    // evidence) and the title actually changed — this is candidate evidence, never
    // identity.
    [[nodiscard]]
    std::optional<BrowserTabProjection>
    project_active_child_change( const grab::GraphChange& active_child,
                                 std::string_view         current_app,
                                 grab::Pid                current_pid,
                                 std::string_view         current_title,
                                 std::string_view         previous_title );

    // Encode a projection to an eventgrab.v1 wire Event (kind BROWSER_TAB_SWITCHED,
    // category EVENT_CATEGORY_BROWSER, data fields app/pid/tab_title/prev_tab_title).
    [[nodiscard]]
    eventgrab::v1::Event
    to_wire( const BrowserTabProjection& projection );

    // Decode a projection back from an eventgrab.v1 wire Event. Errors if required
    // fields (app, pid, tab_title) are missing.
    [[nodiscard]]
    grab::Result<BrowserTabProjection>
    from_wire( const eventgrab::v1::Event& wire );

}    // namespace grab::compat::eventgrab_v1
