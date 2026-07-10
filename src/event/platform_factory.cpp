#include "event/platform_factory.hpp"
#include "event/source.hpp"

#include <memory>
#include <vector>

namespace grab::event
{

    std::vector<std::unique_ptr<EventSource>>
    PlatformFactory::build( const SourceConfig& config )
    {
        static_cast<void>( config );
        return {};
    }

}    // namespace grab::event
