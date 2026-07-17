#pragma once

#include "grab/capability.hpp"
#include "kernel/support/environment.hpp"

#include <string>
#include <vector>

namespace grab::core
{

    struct ProviderInfo
    {
            std::string             name;
            std::vector<Capability> capabilities;
            int                     quality = 0;
    };

    class Provider
    {
        public:

            Provider()                  = default;
            Provider( const Provider& ) = delete;
            Provider&
            operator=( const Provider& ) = delete;
            Provider( Provider&& )       = delete;
            Provider&
            operator=( Provider&& ) = delete;
            virtual ~Provider()     = default;

            [[nodiscard]]
            virtual const ProviderInfo&
            info() const noexcept = 0;

            [[nodiscard]]
            virtual Availability
            probe( const Environment& env ) const = 0;
    };

}    // namespace grab::core
