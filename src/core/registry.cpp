#include "core/log.hpp"
#include "core/provider.hpp"
#include "core/registry.hpp"
#include "grab/capability.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>
#include <vector>

namespace grab::core
{

    Registry::Registry( std::vector<std::unique_ptr<Provider>> providers ) :
        providers_( std::move( providers ) )
    {
    }

    std::vector<const Provider*>
    Registry::providers_for( Capability capability ) const
    {
        std::vector<const Provider*> matches;
        for( const auto& provider : providers_ )
        {
            const auto& caps = provider->info().capabilities;
            if( std::ranges::find( caps, capability ) != caps.end() )
            {
                matches.push_back( provider.get() );
            }
        }
        return matches;
    }

    std::vector<const Provider*>
    Registry::all() const
    {
        std::vector<const Provider*> result;
        result.reserve( providers_.size() );
        for( const auto& provider : providers_ )
        {
            result.push_back( provider.get() );
        }
        return result;
    }

    RegistryBuilder&
    RegistryBuilder::add( std::unique_ptr<Provider> provider )
    {
        assert( provider != nullptr );
        log::verbose(
            [&provider]( auto& event )
            {
                event.tag( "registry.add" )
                    .value( "provider", provider->info().name )
                    .value( "capabilities", provider->info().capabilities.size() );
            }
        );
        providers_.push_back( std::move( provider ) );
        return *this;
    }

    Registry
    RegistryBuilder::build() &&
    {
        return Registry( std::move( providers_ ) );
    }

    Registry
    builtin_registry()
    {
        RegistryBuilder builder;
        return std::move( builder ).build();
    }

}    // namespace grab::core
