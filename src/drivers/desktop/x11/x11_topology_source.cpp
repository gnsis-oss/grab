#include "drivers/desktop/x11/x11_topology_source.hpp"

#include <algorithm>
#include <expected>
#include <utility>

namespace grab::drivers::desktop::x11
{

    X11TopologySource::X11TopologySource( RefreshHook on_change,
                                          const char* display ) :
        on_change_( std::move( on_change ) )
    {
        if( display != nullptr )
        {
            display_             = display;
            use_default_display_ = false;
        }
    }

    grab::Result<TopologyRecord>
    X11TopologySource::poll()
    {
        // list_outputs() opens a short-lived enumeration connection per poll;
        // unifying enumeration with the runtime connection is out of scope.
        auto outputs =
            grab::screen::list_outputs( use_default_display_ ? nullptr
                                                             : display_.c_str() );
        if( !outputs.has_value() )
        {
            return std::unexpected( std::move( outputs.error() ) );
        }

        const bool same_outputs =
            last_outputs_.size() ==
            outputs->size() &&
            std::ranges::equal( last_outputs_,
                                *outputs,
                                []( const grab::screen::OutputInfo& lhs,
                                    const grab::screen::OutputInfo& rhs )
                                {
                                    return lhs.name ==
                                           rhs.name &&
                                           lhs.bounds == rhs.bounds;
                                } );

        bool changed{};
        if( !has_baseline_ )
        {
            // The initial topology establishes a baseline; it is not a change.
            last_outputs_ = *outputs;
            has_baseline_ = true;
        }
        else if( !same_outputs )
        {
            ++generation_;
            last_outputs_ = *outputs;
            changed       = true;
            if( on_change_ )
            {
                on_change_();
            }
        }

        return TopologyRecord{
            .outputs    = std::move( *outputs ),
            .generation = generation_,
            .changed    = changed,
        };
    }

}    // namespace grab::drivers::desktop::x11
