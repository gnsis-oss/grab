#include "drivers/desktop/x11/coordinate_authority.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "grab/capture.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "screen/x11_capture.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        [[nodiscard]]
        grab::Result<void>
        validate_capture_bounds( const grab::geometry::Rectangle& bounds )
        {
            constexpr auto maximumCoordinate =
                static_cast<std::int32_t>( std::numeric_limits<std::int16_t>::max() );
            constexpr auto maximumExtent =
                static_cast<std::uint32_t>( std::numeric_limits<std::uint16_t>::max() );

            if( bounds.x <
                0 ||
                bounds.x >
                maximumCoordinate ||
                bounds.y <
                0 ||
                bounds.y >
                maximumCoordinate ||
                bounds.width ==
                0U ||
                bounds.width >
                maximumExtent ||
                bounds.height ==
                0U ||
                bounds.height > maximumExtent )
            {
                return grab::fail(
                    grab::ErrorCode::GeometryUntrusted,
                    "X11 output bounds cannot be represented for capture"
                );
            }
            return {};
        }

    }    // namespace

    X11CaptureRoute::X11CaptureRoute( grab::screen::X11Capturer capturer,
                                      CoordinateAuthority       authority ) noexcept :
        capturer_{ std::move( capturer ) },
        authority_{ std::move( authority ) }
    {
    }

    grab::Result<X11CaptureRoute>
    X11CaptureRoute::open( const char* display )
    {
        auto capturer = grab::screen::X11Capturer::open( display );
        if( !capturer.has_value() )
        {
            return std::unexpected( std::move( capturer.error() ) );
        }

        CoordinateAuthority authority{ display };
        auto                refreshed = authority.refresh();
        if( !refreshed.has_value() )
        {
            return std::unexpected( std::move( refreshed.error() ) );
        }

        return X11CaptureRoute{ std::move( *capturer ), std::move( authority ) };
    }

    grab::Result<grab::Frame>
    X11CaptureRoute::capture_output( std::string_view name )
    {
        auto refreshed = authority_.refresh();
        if( !refreshed.has_value() )
        {
            return std::unexpected( std::move( refreshed.error() ) );
        }

        auto output = authority_.output_space( name );
        if( !output.has_value() )
        {
            return std::unexpected( std::move( output.error() ) );
        }

        auto valid_bounds = validate_capture_bounds( output->bounds );
        if( !valid_bounds.has_value() )
        {
            return std::unexpected( std::move( valid_bounds.error() ) );
        }

        const auto width  = static_cast<std::uint16_t>( output->bounds.width );
        const auto height = static_cast<std::uint16_t>( output->bounds.height );
        return capturer_.capture_region_frame(
            static_cast<std::int16_t>( output->bounds.x ),
            static_cast<std::int16_t>( output->bounds.y ),
            width,
            height,
            output->space,
            output->generation,
            output->scale,
            grab::SpaceRect{
                .x     = 0.0,
                .y     = 0.0,
                .w     = static_cast<double>( width ),
                .h     = static_cast<double>( height ),
                .space = output->space,
            }
        );
    }

    grab::Result<grab::Frame>
    X11CaptureRoute::capture_window( std::uint32_t window )
    {
        auto refreshed = authority_.refresh();
        if( !refreshed.has_value() )
        {
            return std::unexpected( std::move( refreshed.error() ) );
        }

        // The frame's pixels are window-local while the stamped space is the
        // display-global space; per-window coordinate spaces land with the Wave-3
        // single-connection unification.
        return capturer_.capture_window_frame( window,
                                               authority_.global_space(),
                                               authority_.capture_generation() );
    }

    grab::Result<std::vector<grab::TransformRecord>>
    X11CaptureRoute::refresh_transforms()
    {
        auto refreshed = authority_.refresh();
        if( !refreshed.has_value() )
        {
            return std::unexpected( std::move( refreshed.error() ) );
        }
        return std::vector<grab::TransformRecord>{
            authority_.transforms().begin(),
            authority_.transforms().end()
        };
    }

    const CoordinateAuthority&
    X11CaptureRoute::coordinate_authority() const noexcept
    {
        return authority_;
    }

    std::shared_ptr<const grab::detail::SpaceGraph>
    X11CaptureRoute::graph() const noexcept
    {
        return authority_.graph();
    }

    grab::CoordinateSpaceId
    X11CaptureRoute::global_space() const noexcept
    {
        return authority_.global_space();
    }

}    // namespace grab::drivers::desktop::x11
