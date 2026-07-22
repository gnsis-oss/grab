#include "config/environment.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>
// clang-format on

namespace
{

    constexpr std::size_t      environmentTerminatorCount = 1U;
    constexpr std::size_t      singleOverride             = 1U;
    constexpr std::string_view pathKey                    = "PATH";
    constexpr std::string_view inheritedPath              = "/usr/bin:/bin";
    constexpr std::string_view replaceKey       = "GRAB_CONFIG_OVERLAY_REPLACE";
    constexpr std::string_view inheritedValue   = "inherited";
    constexpr std::string_view replacementValue = "replacement";
    constexpr std::string_view addKey           = "GRAB_CONFIG_OVERLAY_ADD";
    constexpr std::string_view addedValue       = "added";

    class ScopedEnvironment
    {
        public:

            ScopedEnvironment( std::string_view                name,
                               std::optional<std::string_view> value ) :
                original_environment_( ::environ )
            {
                for( char* const* entry = original_environment_;
                     entry != nullptr && *entry != nullptr;
                     entry = std::next( entry ) )
                {
                    const std::string_view current{ *entry };
                    const auto             separator = current.find( '=' );
                    if( current.substr( 0U, separator ) != name )
                    {
                        entries_.emplace_back( current );
                    }
                }
                if( value.has_value() )
                {
                    entries_.emplace_back(
                        std::string{ name } + "=" + std::string{ *value }
                    );
                }

                environment_.reserve( entries_.size() + environmentTerminatorCount );
                for( std::string& entry : entries_ )
                {
                    environment_.push_back( entry.data() );
                }
                environment_.push_back( nullptr );
                ::environ = environment_.data();
            }

            ~ScopedEnvironment()
            {
                ::environ = original_environment_;
            }

            ScopedEnvironment( const ScopedEnvironment& ) = delete;
            ScopedEnvironment&
            operator=( const ScopedEnvironment& )    = delete;
            ScopedEnvironment( ScopedEnvironment&& ) = delete;
            ScopedEnvironment&
            operator=( ScopedEnvironment&& ) = delete;

        private:

            char**                   original_environment_{};
            std::vector<std::string> entries_;
            std::vector<char*>       environment_;
    };

    [[nodiscard]]
    std::string
    environment_entry( std::string_view key,
                       std::string_view value )
    {
        return std::string{ key } + "=" + std::string{ value };
    }

    [[nodiscard]]
    std::size_t
    count_key( const std::vector<std::string>& environment,
               std::string_view                key )
    {
        const std::string prefix = std::string{ key } + "=";
        return static_cast<std::size_t>(
            std::ranges::count_if( environment,
                                   [&prefix]( const std::string& entry )
                                   {
                                       return entry.starts_with( prefix );
                                   } )
        );
    }

}    // namespace

TEST( ConfigEnvironment,
      OverlayKeepsInherited )
{
    const ScopedEnvironment environment{ pathKey, inheritedPath };

    const auto              overlay = grab::config::overlay_environment( {} );

    EXPECT_NE( std::ranges::find( overlay, environment_entry( pathKey, inheritedPath ) ),
               overlay.end() );
}

TEST( ConfigEnvironment,
      OverlayReplacesKey )
{
    const ScopedEnvironment                   environment{ replaceKey, inheritedValue };
    const std::pair<std::string, std::string> replacement{
        std::string{ replaceKey },
        std::string{ replacementValue },
    };

    const auto overlay =
        grab::config::overlay_environment( std::span{ &replacement, singleOverride } );

    EXPECT_EQ( count_key( overlay, replaceKey ), singleOverride );
    EXPECT_NE( std::ranges::find( overlay,
                                  environment_entry( replaceKey, replacementValue ) ),
               overlay.end() );
}

TEST( ConfigEnvironment,
      OverlayAddsKey )
{
    const ScopedEnvironment                   environment{ addKey, std::nullopt };
    const std::pair<std::string, std::string> addition{
        std::string{ addKey },
        std::string{ addedValue },
    };

    const auto overlay =
        grab::config::overlay_environment( std::span{ &addition, singleOverride } );

    EXPECT_EQ( count_key( overlay, addKey ), singleOverride );
    EXPECT_NE( std::ranges::find( overlay, environment_entry( addKey, addedValue ) ),
               overlay.end() );
}
