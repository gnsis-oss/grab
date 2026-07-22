#pragma once

#include "grab/result.hpp"
#include "grab/space.hpp"

#include <cstdint>
#include <map>
#include <web/web.hpp>

namespace grab::detail
{

    class SpaceGraph final
    {
        public:

            [[nodiscard]]
            CoordinateSpaceId
            add_space( std::uint32_t generation = 0U );

            void
            add_transform( TransformRecord transform );

            void
            bump_generation( CoordinateSpaceId space );

            [[nodiscard]]
            Result<Affine>
            resolve_transform( CoordinateSpaceId source,
                               CoordinateSpaceId destination ) const;

            [[nodiscard]]
            Result<SpacePoint>
            map( SpacePoint        point,
                 CoordinateSpaceId destination ) const;

            [[nodiscard]]
            Result<TransformTrust>
            route_trust( CoordinateSpaceId source,
                         CoordinateSpaceId destination ) const;

        private:

            struct Route
            {
                    Affine         transform{};
                    TransformTrust trust{ TransformTrust::Exact };
                    bool           stale{};
            };

            [[nodiscard]]
            Result<Route>
            find_route( CoordinateSpaceId source,
                        CoordinateSpaceId destination ) const;

            web::Web<web::OneWay, TransformRecord>     graph_{};
            std::map<CoordinateSpaceId, web::Knot>     knots_{};
            std::map<web::Knot, CoordinateSpaceId>     spaces_{};
            std::map<CoordinateSpaceId, std::uint32_t> generations_{};
            std::uint32_t                              next_space_{ 1U };
    };

}    // namespace grab::detail
