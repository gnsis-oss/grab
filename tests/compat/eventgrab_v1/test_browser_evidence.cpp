#include "compat/eventgrab_v1/browser_evidence.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <string>
#include <string_view>
// clang-format on

namespace
{

    constexpr std::string_view firefoxApp     = "Firefox";
    constexpr std::string_view chromeUpperApp = "CHROME";
    constexpr std::string_view firefoxWmClass = "Navigator.firefox-esr";
    constexpr std::string_view terminalApp    = "gnome-terminal";
    constexpr std::string_view codeApp        = "code";
    constexpr std::string_view filesApp       = "nautilus";
    constexpr std::string_view emptyApp;

}    // namespace

TEST( BrowserClassifier,
      IsBrowserAppMatchesKnownKeywords )
{
    for( const std::string_view keyword : grab::compat::eventgrab_v1::browserKeywords )
    {
        EXPECT_TRUE( grab::compat::eventgrab_v1::is_browser_app( keyword ) )
            << std::string{ keyword };
    }
}

TEST( BrowserClassifier,
      IsBrowserAppIsCaseInsensitive )
{
    EXPECT_TRUE( grab::compat::eventgrab_v1::is_browser_app( firefoxApp ) );
    EXPECT_TRUE( grab::compat::eventgrab_v1::is_browser_app( chromeUpperApp ) );
}

TEST( BrowserClassifier,
      IsBrowserAppMatchesWmClassSubstrings )
{
    EXPECT_TRUE( grab::compat::eventgrab_v1::is_browser_app( firefoxWmClass ) );
}

TEST( BrowserClassifier,
      IsBrowserAppRejectsNonBrowsers )
{
    EXPECT_FALSE( grab::compat::eventgrab_v1::is_browser_app( terminalApp ) );
    EXPECT_FALSE( grab::compat::eventgrab_v1::is_browser_app( codeApp ) );
    EXPECT_FALSE( grab::compat::eventgrab_v1::is_browser_app( filesApp ) );
    EXPECT_FALSE( grab::compat::eventgrab_v1::is_browser_app( emptyApp ) );
}
