#include "config/environment.hpp"

#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace grab::config
{
    namespace
    {

        constexpr char environmentSeparator = '=';

    }    // namespace

    std::vector<std::string>
    overlay_environment( std::span<const std::pair<std::string,
                                                   std::string>> overrides )
    {
        std::vector<std::string> environment;
        for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
             entry              = std::next( entry ) )
        {
            environment.emplace_back( *entry );
        }

        for( const auto& [key, value] : overrides )
        {
            const auto has_key = [&key]( const std::string& entry )
            {
                const std::string_view candidate{ entry };
                return candidate.size() >
                       key.size() &&
                       candidate.starts_with( key ) &&
                       candidate.at( key.size() ) == environmentSeparator;
            };

            std::erase_if( environment, has_key );
            std::string replacement{ key };
            replacement.push_back( environmentSeparator );
            replacement.append( value );
            environment.push_back( std::move( replacement ) );
        }
        return environment;
    }

}    // namespace grab::config
