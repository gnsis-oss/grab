#include "inventory/environment_merge.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::inventory
{
    namespace
    {

        constexpr char             env_assign       = '=';
        constexpr std::string_view qt_im_module_key = "QT_IM_MODULE";

        [[nodiscard]]
        std::string_view
        key_of( std::string_view entry )
        {
            const std::size_t assign = entry.find( env_assign );
            return assign == std::string_view::npos ? entry : entry.substr( 0U, assign );
        }

        [[nodiscard]]
        bool
        is_overridden( std::string_view                        key,
                       std::span<const std::pair<std::string,
                                                 std::string>> overrides )
        {
            return std::ranges::any_of(
                overrides,
                [key]( const std::pair<std::string, std::string>& entry )
                {
                    return entry.first == key;
                }
            );
        }

    }    // namespace

    std::vector<std::string>
    merge_environment( std::span<const std::string_view>       base,
                       std::span<const std::pair<std::string,
                                                 std::string>> overrides )
    {
        std::vector<std::string> merged;
        merged.reserve( base.size() + overrides.size() );

        for( const std::string_view entry : base )
        {
            const std::string_view key = key_of( entry );
            if( key == qt_im_module_key || is_overridden( key, overrides ) )
            {
                continue;
            }
            merged.emplace_back( entry );
        }

        for( const std::pair<std::string, std::string>& override_entry : overrides )
        {
            merged.push_back(
                override_entry.first + env_assign + override_entry.second
            );
        }

        return merged;
    }

}    // namespace grab::inventory
