#pragma once

#include "grab/geometry/point.hpp"

#include <cstddef>
#include <vector>

namespace grab::geometry
{

    struct Curve
    {
            std::vector<PointF> control;

            [[nodiscard]]
            std::size_t
            degree() const noexcept
            {
                if( control.empty() )
                {
                    return 0U;
                }
                return control.size() - 1U;
            }

            [[nodiscard]]
            PointF
            evaluate( double t ) const
            {
                if( control.empty() )
                {
                    return PointF{};
                }

                std::vector<PointF> points = control;
                for( std::size_t count = points.size(); count > 1U; --count )
                {
                    for( std::size_t index = 0U; index < count - 1U; ++index )
                    {
                        points.at( index ) = PointF::lerp( points.at( index ),
                                                           points.at( index + 1U ),
                                                           t );
                    }
                }
                return points.front();
            }

            [[nodiscard]]
            std::vector<Point>
            sample( std::size_t count ) const
            {
                std::vector<Point> points;
                points.reserve( count );
                if( count == 0U )
                {
                    return points;
                }
                if( count == 1U )
                {
                    points.push_back( evaluate( 0.0 ).to_point() );
                    return points;
                }

                const auto denominator = static_cast<double>( count - 1U );
                for( std::size_t index = 0U; index < count; ++index )
                {
                    const auto t = static_cast<double>( index ) / denominator;
                    points.push_back( evaluate( t ).to_point() );
                }
                return points;
            }

            [[nodiscard]]
            static Curve
            line( PointF a,
                  PointF b )
            {
                return Curve{
                    .control = { a, b },
                };
            }

            [[nodiscard]]
            static Curve
            cubic( PointF p0,
                   PointF p1,
                   PointF p2,
                   PointF p3 )
            {
                return Curve{
                    .control = { p0, p1, p2, p3 },
                };
            }
    };

}    // namespace grab::geometry
