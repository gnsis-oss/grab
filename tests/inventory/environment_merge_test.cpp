#include "inventory/environment_merge.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::array<std::string_view, 3U> base{
        "PATH=/usr/bin",
        "QT_IM_MODULE=ibus",
        "DISPLAY=:0",
    };

    [[nodiscard]]
    bool
    contains( const std::vector<std::string>& env,
              std::string_view                entry )
    {
        return std::ranges::find( env, entry ) != env.end();
    }

}    // namespace

TEST( MergeEnvironment,
      OverrideReplacesExistingKey )
{
    const std::array<std::pair<std::string, std::string>, 1U> overrides{
        { { "DISPLAY", ":9" } },
    };
    const auto merged = grab::inventory::merge_environment( base, overrides );

    EXPECT_TRUE( contains( merged, "DISPLAY=:9" ) );
    EXPECT_FALSE( contains( merged, "DISPLAY=:0" ) );
    EXPECT_TRUE( contains( merged, "PATH=/usr/bin" ) );
}

TEST( MergeEnvironment,
      AddsNewKeyAndDropsQtImModule )
{
    const std::array<std::pair<std::string, std::string>, 1U> overrides{
        { { "WAYLAND_DISPLAY", "wayland-1" } },
    };
    const auto merged = grab::inventory::merge_environment( base, overrides );

    EXPECT_TRUE( contains( merged, "WAYLAND_DISPLAY=wayland-1" ) );
    EXPECT_FALSE( contains( merged, "QT_IM_MODULE=ibus" ) );
}
