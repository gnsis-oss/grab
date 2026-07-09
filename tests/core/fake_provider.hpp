#pragma once

#include "core/provider.hpp"
#include "grab/capability.hpp"

#include <string>
#include <utility>
#include <vector>

namespace grab::test
{

    class FakeProvider final : public grab::core::Provider
    {
        public:

            FakeProvider( std::string             name,
                          std::vector<Capability> capabilities,
                          grab::Availability      availability ) :
                info_{
                    std::move( name ),
                    std::move( capabilities ),
                    availability.quality,
                },
                availability_( std::move( availability ) )
            {
            }

            [[nodiscard]]
            const grab::core::ProviderInfo&
            info() const noexcept override
            {
                return info_;
            }

            [[nodiscard]]
            grab::Availability
            probe( const grab::core::Environment& /*env*/ ) const override
            {
                return availability_;
            }

        private:

            grab::core::ProviderInfo info_;
            grab::Availability       availability_;
    };

}    // namespace grab::test
