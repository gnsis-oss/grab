#include "drivers/desktop/x11/coordinate_authority.hpp"
#include "drivers/desktop/x11/enumerate.hpp"
#include "grab/geometry/rectangle.hpp"
#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/space_graph.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        [[nodiscard]]
        bool
        same_bounds( const grab::geometry::Rectangle& first,
                     const grab::geometry::Rectangle& second ) noexcept
        {
            return first.x ==
                   second.x &&
                   first.y ==
                   second.y &&
                   first.width ==
                   second.width &&
                   first.height == second.height;
        }

    }    // namespace

    CoordinateAuthority::CoordinateAuthority( const char* display ) :
        display_{ display == nullptr ? "" : display },
        use_default_display_{ display == nullptr }
    {
    }

    grab::Result<void>
    CoordinateAuthority::refresh()
    {
        auto outputs =
            grab::screen::list_outputs( use_default_display_ ? nullptr
                                                             : display_.c_str() );
        if( !outputs.has_value() )
        {
            return std::unexpected( std::move( outputs.error() ) );
        }
        return refresh( *outputs );
    }

    grab::Result<void>
    CoordinateAuthority::refresh( std::span<const grab::screen::OutputInfo> outputs )
    {
        if( graph_ != nullptr && topology_matches( outputs ) )
        {
            return {};
        }

        if( generation_.value == std::numeric_limits<std::uint32_t>::max() )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "coordinate topology generation overflow" );
        }

        const auto next_generation = grab::DisplayGeneration{
            .value = generation_.value + 1U,
        };
        auto       next_graph  = std::make_shared<grab::detail::SpaceGraph>();
        const auto next_global = next_graph->add_space( next_generation.value );

        std::vector<OutputSpace> next_outputs;
        next_outputs.reserve( outputs.size() );
        std::vector<grab::TransformRecord> next_transforms;
        next_transforms.reserve( outputs.size() );
        std::uint64_t mapping_id = 1U;
        for( const auto& output : outputs )
        {
            const auto output_space = next_graph->add_space( next_generation.value );
            next_transforms.push_back( grab::TransformRecord{
                .source      = output_space,
                .destination = next_global,
                .map =
                    {
                          .tx = static_cast<double>( output.bounds.x ),
                          .ty = static_cast<double>( output.bounds.y ),
                          },
                .mapping_id = mapping_id,
                .generation = next_generation.value,
                .trust      = grab::TransformTrust::Exact,
            } );
            next_graph->add_transform( next_transforms.back() );
            ++mapping_id;
            next_outputs.push_back( OutputSpace{
                .name       = output.name,
                .bounds     = output.bounds,
                .space      = output_space,
                .scale      = 1.0,
                .generation = grab::CaptureGeneration{ next_generation.value },
            } );
        }

        if( graph_ != nullptr )
        {
            graph_->bump_generation( global_space_ );
            for( const auto& output : outputs_ )
            {
                graph_->bump_generation( output.space );
            }
        }

        graph_        = std::move( next_graph );
        global_space_ = next_global;
        generation_   = next_generation;
        outputs_      = std::move( next_outputs );
        transforms_   = std::move( next_transforms );
        return {};
    }

    std::shared_ptr<const grab::detail::SpaceGraph>
    CoordinateAuthority::graph() const noexcept
    {
        return graph_;
    }

    std::span<const grab::TransformRecord>
    CoordinateAuthority::transforms() const noexcept
    {
        return transforms_;
    }

    grab::CoordinateSpaceId
    CoordinateAuthority::global_space() const noexcept
    {
        return global_space_;
    }

    grab::DisplayGeneration
    CoordinateAuthority::generation() const noexcept
    {
        return generation_;
    }

    grab::CaptureGeneration
    CoordinateAuthority::capture_generation() const noexcept
    {
        return grab::CaptureGeneration{ generation_.value };
    }

    grab::Result<OutputSpace>
    CoordinateAuthority::output_space( std::string_view name ) const
    {
        const auto output = std::ranges::find( outputs_, name, &OutputSpace::name );
        if( output == outputs_.end() )
        {
            return grab::fail( grab::ErrorCode::RouteUnavailable,
                               "output coordinate space is unavailable" );
        }
        return *output;
    }

    std::span<const OutputSpace>
    CoordinateAuthority::output_spaces() const noexcept
    {
        return outputs_;
    }

    bool
    CoordinateAuthority::topology_matches(
        std::span<const grab::screen::OutputInfo> outputs
    ) const
    {
        if( outputs.size() != outputs_.size() )
        {
            return false;
        }

        return std::ranges::equal( outputs,
                                   outputs_,
                                   []( const grab::screen::OutputInfo& current,
                                       const OutputSpace& registered_output )
                                   {
                                       return current.name ==
                                              registered_output.name &&
                                              same_bounds( current.bounds,
                                                           registered_output.bounds );
                                   } );
    }

}    // namespace grab::drivers::desktop::x11
