#include "compat/eventgrab_v1/browser_projection.hpp"
#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/pid.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <optional>
#include <string>
// clang-format on

TEST( BrowserProjection,
      DerivesTabSwitchFromActiveChildChangeIntoBrowser )
{
    const grab::GraphChange delta{
        .node            = 2U,
        .related         = 0U,
        .relation        = 0U,
        .previous_active = 1U,
    };

    const auto projection =
        grab::compat::eventgrab_v1::project_active_child_change( delta,
                                                                 "firefox",
                                                                 grab::Pid{ 4'242 },
                                                                 "Docs",
                                                                 "Search" );

    ASSERT_TRUE( projection.has_value() );
    EXPECT_EQ( projection->app, "firefox" );
    EXPECT_EQ( projection->pid, grab::Pid{ 4'242 } );
    EXPECT_EQ( projection->tab_title, "Docs" );
    EXPECT_EQ( projection->prev_tab_title, "Search" );
}

TEST( BrowserProjection,
      RejectsNonBrowserAndUnchangedTitle )
{
    const grab::GraphChange delta{
        .node            = 2U,
        .related         = 0U,
        .relation        = 0U,
        .previous_active = 1U,
    };

    EXPECT_FALSE(
        grab::compat::eventgrab_v1::project_active_child_change( delta,
                                                                 "gnome-terminal",
                                                                 grab::Pid{ 4'242 },
                                                                 "x",
                                                                 "y" )
            .has_value()
    );
    EXPECT_FALSE(
        grab::compat::eventgrab_v1::project_active_child_change( delta,
                                                                 "firefox",
                                                                 grab::Pid{ 4'242 },
                                                                 "Same",
                                                                 "Same" )
            .has_value()
    );
}

TEST( BrowserProjection,
      WireRoundTripThroughCompat )
{
    const grab::compat::eventgrab_v1::BrowserTabProjection projection{
        .app            = "firefox",
        .pid            = grab::Pid{ 4'242 },
        .tab_title      = "Docs",
        .prev_tab_title = "Search",
    };

    const auto wire = grab::compat::eventgrab_v1::to_wire( projection );
    EXPECT_EQ( wire.kind(), eventgrab::v1::BROWSER_TAB_SWITCHED );
    EXPECT_EQ( wire.category(), eventgrab::v1::EVENT_CATEGORY_BROWSER );

    std::string bytes;
    ASSERT_TRUE( wire.SerializeToString( &bytes ) );

    eventgrab::v1::Event parsed;
    ASSERT_TRUE( parsed.ParseFromString( bytes ) );

    const auto decoded = grab::compat::eventgrab_v1::from_wire( parsed );
    ASSERT_TRUE( decoded.has_value() );
    EXPECT_EQ( *decoded, projection );
}
