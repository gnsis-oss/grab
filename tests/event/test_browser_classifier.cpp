#include "event/browser_classifier.hpp"
#include "grab/pid.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::int64_t     browserPidValue = 42;
    constexpr grab::Pid        browserPid{ browserPidValue };
    constexpr std::string_view firefoxApp     = "Firefox";
    constexpr std::string_view chromeUpperApp = "CHROME";
    constexpr std::string_view firefoxWmClass = "Navigator.firefox-esr";
    constexpr std::string_view browserApp     = "browser-app";
    constexpr std::string_view terminalApp    = "gnome-terminal";
    constexpr std::string_view codeApp        = "code";
    constexpr std::string_view filesApp       = "nautilus";
    constexpr std::string_view emptyApp;
    constexpr std::string_view initialTabTitle = "Initial tab";
    constexpr std::string_view changedTabTitle = "Changed tab";
    constexpr std::string_view otherBrowserApp = "other-browser-app";

}    // namespace

TEST( BrowserClassifier,
      IsBrowserAppMatchesKnownKeywords )
{
    for( const std::string_view keyword : grab::event::browserKeywords )
    {
        EXPECT_TRUE( grab::event::is_browser_app( keyword ) ) << std::string{ keyword };
    }
}

TEST( BrowserClassifier,
      IsBrowserAppIsCaseInsensitive )
{
    EXPECT_TRUE( grab::event::is_browser_app( firefoxApp ) );
    EXPECT_TRUE( grab::event::is_browser_app( chromeUpperApp ) );
}

TEST( BrowserClassifier,
      IsBrowserAppMatchesWmClassSubstrings )
{
    EXPECT_TRUE( grab::event::is_browser_app( firefoxWmClass ) );
}

TEST( BrowserClassifier,
      IsBrowserAppRejectsNonBrowsers )
{
    EXPECT_FALSE( grab::event::is_browser_app( terminalApp ) );
    EXPECT_FALSE( grab::event::is_browser_app( codeApp ) );
    EXPECT_FALSE( grab::event::is_browser_app( filesApp ) );
    EXPECT_FALSE( grab::event::is_browser_app( emptyApp ) );
}

TEST( BrowserClassifier,
      TitleChangeSynthesizesBrowserTab )
{
    const auto tab = grab::event::browser_tab_on_title_change( browserApp,
                                                               browserPid,
                                                               true,
                                                               changedTabTitle,
                                                               initialTabTitle );
    ASSERT_TRUE( tab.has_value() );
    EXPECT_EQ( tab->app, std::string{ browserApp } );
    EXPECT_EQ( tab->pid.value(), browserPidValue );
    EXPECT_EQ( tab->tab_title, std::string{ changedTabTitle } );
    EXPECT_EQ( tab->prev_tab_title, std::string{ initialTabTitle } );
}

TEST( BrowserClassifier,
      TitleChangeRejectsUnchangedEmptyAndNonBrowser )
{
    EXPECT_FALSE( grab::event::browser_tab_on_title_change( browserApp,
                                                            browserPid,
                                                            true,
                                                            initialTabTitle,
                                                            initialTabTitle )
                      .has_value() );
    EXPECT_FALSE( grab::event::browser_tab_on_title_change( browserApp,
                                                            browserPid,
                                                            true,
                                                            emptyApp,
                                                            initialTabTitle )
                      .has_value() );
    EXPECT_FALSE( grab::event::browser_tab_on_title_change( terminalApp,
                                                            browserPid,
                                                            false,
                                                            changedTabTitle,
                                                            initialTabTitle )
                      .has_value() );
}

TEST( BrowserClassifier,
      FocusIntoBrowserSynthesizesBrowserTab )
{
    const auto tab = grab::event::browser_tab_on_focus_change( browserApp,
                                                               browserPid,
                                                               true,
                                                               initialTabTitle,
                                                               true,
                                                               false,
                                                               terminalApp );
    ASSERT_TRUE( tab.has_value() );
    EXPECT_EQ( tab->app, std::string{ browserApp } );
    EXPECT_EQ( tab->pid.value(), browserPidValue );
    EXPECT_EQ( tab->tab_title, std::string{ initialTabTitle } );
    EXPECT_TRUE( tab->prev_tab_title.empty() );
}

TEST( BrowserClassifier,
      FocusBrowserToSameAppBrowserDoesNotSynthesize )
{
    EXPECT_FALSE( grab::event::browser_tab_on_focus_change( browserApp,
                                                            browserPid,
                                                            true,
                                                            initialTabTitle,
                                                            true,
                                                            true,
                                                            browserApp )
                      .has_value() );
}

TEST( BrowserClassifier,
      FocusBrowserToDifferentAppBrowserSynthesizesBrowserTab )
{
    const auto tab = grab::event::browser_tab_on_focus_change( browserApp,
                                                               browserPid,
                                                               true,
                                                               changedTabTitle,
                                                               true,
                                                               true,
                                                               otherBrowserApp );
    ASSERT_TRUE( tab.has_value() );
    EXPECT_EQ( tab->app, std::string{ browserApp } );
    EXPECT_EQ( tab->pid.value(), browserPidValue );
    EXPECT_EQ( tab->tab_title, std::string{ changedTabTitle } );
    EXPECT_TRUE( tab->prev_tab_title.empty() );
}

TEST( BrowserClassifier,
      FocusChangeRejectsNonBrowserCurrentWindow )
{
    EXPECT_FALSE( grab::event::browser_tab_on_focus_change( terminalApp,
                                                            browserPid,
                                                            false,
                                                            initialTabTitle,
                                                            true,
                                                            false,
                                                            codeApp )
                      .has_value() );
}
