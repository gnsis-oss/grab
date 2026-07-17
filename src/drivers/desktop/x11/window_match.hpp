#pragma once

#include "kernel/support/ascii.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace grab::screen
{

    [[nodiscard]]
    inline std::vector<std::string>
    normalized_wm_class_candidates( const std::vector<std::string>& candidates )
    {
        std::vector<std::string> result;
        result.reserve( candidates.size() );
        for( const std::string& candidate : candidates )
        {
            if( !candidate.empty() )
            {
                result.push_back( grab::core::ascii_lower_copy( candidate ) );
            }
        }
        return result;
    }

    [[nodiscard]]
    inline bool
    wm_class_matches_any( std::string_view                wm_class,
                          const std::vector<std::string>& normalized_candidates )
    {
        const std::string normalized_class = grab::core::ascii_lower_copy( wm_class );
        return std::ranges::any_of( normalized_candidates,
                                    [&normalized_class]( const std::string& candidate )
                                    {
                                        return normalized_class.contains( candidate );
                                    } );
    }

}    // namespace grab::screen
