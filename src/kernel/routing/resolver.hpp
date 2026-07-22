#pragma once

#include "grab/capability.hpp"
#include "grab/result.hpp"
#include "kernel/routing/registry.hpp"

#include <compare>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace grab::core
{

    struct ResolveOptions
    {
            bool prefer_permission_over_degraded = true;
    };

    struct CapabilityRequest
    {
            Capability     capability = Capability::ScreenDisplayImage;
            std::string    target_class;    // empty = targetless
            std::string    target_key;
            ResolveOptions options;
    };

    struct Resolution
    {
            std::vector<const Provider*> chain;    // best first
            Availability                 best;
    };

    class Resolver
    {
        public:

            explicit Resolver( const Registry& registry );

            [[nodiscard]]
            Result<Resolution>
            resolve( const CapabilityRequest& request,
                     const Environment&       env ) const;

        private:

            struct CacheKey
            {
                    std::uint64_t generation = 0;
                    Capability    capability = Capability::ScreenDisplayImage;
                    std::string   target_class;
                    std::string   target_key;
                    bool          prefer_permission_over_degraded = true;

                    [[nodiscard]]
                    auto
                    operator<=>( const CacheKey& ) const = default;
            };

            const Registry&                        registry_;
            mutable std::mutex                     mutex_;
            mutable std::map<CacheKey, Resolution> cache_;
    };

}    // namespace grab::core
