#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/ids.hpp"
#include "grab/presentation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/target_registry.hpp"
#include "screen/enumerate.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <xcb/xcb.h>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        [[nodiscard]]
        grab::SpaceRect
        window_bounds( const grab::screen::WindowInfo& window ) noexcept
        {
            return grab::SpaceRect{
                .x     = static_cast<double>( window.bounds.x ),
                .y     = static_cast<double>( window.bounds.y ),
                .w     = static_cast<double>( window.bounds.width ),
                .h     = static_cast<double>( window.bounds.height ),
                .space = grab::CoordinateSpaceId{ 1U },
            };
        }

        [[nodiscard]]
        grab::UiProperty
        string_property( grab::PropertyId   id,
                         const std::string& value )
        {
            if( value.empty() )
            {
                return grab::UiProperty{
                    .id   = id,
                    .read = grab::PropertyRead{},
                };
            }
            return grab::UiProperty{
                .id   = id,
                .read = grab::PropertyRead{
                                           .state = grab::PropertyRead::State::Present,
                                           .value = value,
                                           },
            };
        }

        [[nodiscard]]
        grab::UiProperty
        pid_property( const std::optional<std::uint32_t>& pid )
        {
            if( !pid.has_value() )
            {
                return grab::UiProperty{
                    .id   = grab::property::process_id,
                    .read = grab::PropertyRead{},
                };
            }
            return grab::UiProperty{
                .id   = grab::property::process_id,
                .read = grab::PropertyRead{
                                           .state = grab::PropertyRead::State::Present,
                                           .value = static_cast<std::int64_t>( *pid ),
                                           },
            };
        }

    }    // namespace

    X11TreeSource::X11TreeSource( grab::RuntimeId               runtime,
                                  grab::DisplayGeneration       display_generation,
                                  grab::kernel::TargetRegistry& targets,
                                  xcb_connection_t*             connection,
                                  xcb_window_t                  root ) :
        X11TreeSource( runtime,
                       display_generation,
                       targets,
                       [connection,
                        root]()
                       {
                           return grab::screen::list_windows( connection, root );
                       } )
    {
    }

    X11TreeSource::X11TreeSource( grab::RuntimeId               runtime,
                                  grab::DisplayGeneration       display_generation,
                                  grab::kernel::TargetRegistry& targets,
                                  WindowEnumerator              enumerate_windows ) :
        runtime_( runtime ),
        display_generation_( display_generation ),
        enumerate_windows_( std::move( enumerate_windows ) ),
        targets_( &targets ),
        alias_authority_( std::string{ "x11.window.runtime." } +
                          std::to_string( runtime.value ) )
    {
    }

    grab::Result<grab::UiSnapshot>
    X11TreeSource::snapshot( std::uint32_t                 tree,
                             const grab::OperationContext& context )
    {
        const auto context_result = context.check();
        if( !context_result.has_value() )
        {
            return std::unexpected( context_result.error() );
        }
        if( tree != firstTree )
        {
            return grab::fail( grab::ErrorCode::NoMatch,
                               "X11 tree snapshot is not available" );
        }

        auto windows = enumerate_windows_();
        if( !windows.has_value() )
        {
            return std::unexpected( std::move( windows.error() ) );
        }

        std::set<std::uint32_t> active_xids;
        for( const auto& window : *windows )
        {
            if( !active_xids.insert( window.id ).second )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "X11 client list contains a duplicate window" );
            }
        }

        const std::scoped_lock lock{ mutex_ };
        if( revision_ == std::numeric_limits<std::uint64_t>::max() )
        {
            return grab::fail( grab::ErrorCode::Overflowed,
                               "X11 tree revision space is exhausted" );
        }
        const std::uint64_t             next_revision = revision_ + 1U;

        std::vector<grab::UiNodeRecord> nodes;
        std::vector<grab::NodeId>       roots;
        nodes.reserve( windows->size() );
        roots.reserve( windows->size() );
        for( const auto& window : *windows )
        {
            const auto bounds  = window_bounds( window );
            auto       binding = observe_window( window, bounds );
            if( !binding.has_value() )
            {
                return std::unexpected( std::move( binding.error() ) );
            }

            roots.push_back( binding->node );
            nodes.push_back( node_record( window, *binding, bounds, next_revision ) );
        }

        auto retired = retire_missing_windows( active_xids );
        if( !retired.has_value() )
        {
            return std::unexpected( std::move( retired.error() ) );
        }

        revision_ = next_revision;
        return grab::UiSnapshot::from_records(
            grab::UiSnapshotMetadata{
                .runtime  = runtime_,
                .tree     = firstTree,
                .epoch    = grab::TreeEpoch{ firstEpoch },
                .revision = revision_,
                .complete = true,
            },
            std::move( nodes ),
            std::move( roots ),
            {}
        );
    }

    grab::Result<std::optional<grab::spi::UiUpdate>>
    X11TreeSource::next_update( const grab::OperationContext& context )
    {
        const auto context_result = context.check();
        if( !context_result.has_value() )
        {
            return std::unexpected( context_result.error() );
        }
        return std::optional<grab::spi::UiUpdate>{};
    }

    const grab::kernel::TargetRegistry&
    X11TreeSource::target_registry() const noexcept
    {
        return *targets_;
    }

    grab::Result<xcb_window_t>
    X11TreeSource::resolve_xid( const grab::WidgetRef& widget ) const
    {
        if( widget.runtime !=
            runtime_ ||
            widget.tree !=
            firstTree ||
            widget.epoch !=
            grab::TreeEpoch{ firstEpoch } ||
            widget.generation != grab::NodeGeneration{ 1U } )
        {
            return grab::fail( grab::ErrorCode::NoMatch,
                               "widget does not belong to the X11 tree" );
        }

        const std::scoped_lock lock{ mutex_ };
        for( const auto& [xid, binding] : bindings_ )
        {
            if( binding.node.value == widget.node )
            {
                return static_cast<xcb_window_t>( xid );
            }
        }
        return grab::fail( grab::ErrorCode::NoMatch,
                           "X11 widget is no longer available" );
    }

    grab::Result<X11TreeSource::WindowBinding>
    X11TreeSource::observe_window( const grab::screen::WindowInfo& window,
                                   const grab::SpaceRect&          bounds )
    {
        const auto existing = bindings_.find( window.id );
        const auto target   = targets_->observe( grab::kernel::TargetObservation{
            .grade  = grab::kernel::TargetGrade::Window,
            .alias  = alias_for( window.id ),
            .title  = window.title,
            .pid    = window.pid,
            .bounds = bounds,
        } );
        if( !target.has_value() )
        {
            return std::unexpected( target.error() );
        }

        if( existing != bindings_.end() )
        {
            if( existing->second.target != *target )
            {
                return grab::fail( grab::ErrorCode::InternalFault,
                                   "X11 target identity changed while active" );
            }
            auto registered =
                targets_->register_surface( *target,
                                            grab::SurfaceRecord{
                                                .id         = existing->second.surface,
                                                .generation = display_generation_,
                                                .space =
                                                    grab::CoordinateSpaceId{ rootSpace },
                                                .bounds = bounds,
                                            } );
            if( !registered.has_value() )
            {
                return std::unexpected( std::move( registered.error() ) );
            }
            return existing->second;
        }

        if( next_node_ ==
            std::numeric_limits<std::uint64_t>::max() ||
            next_surface_ == std::numeric_limits<std::uint64_t>::max() )
        {
            return grab::fail( grab::ErrorCode::Overflowed,
                               "X11 tree identity space is exhausted" );
        }

        const WindowBinding binding{
            .node    = grab::NodeId{ next_node_ },
            .target  = *target,
            .surface = grab::SurfaceId{ next_surface_ },
        };
        auto registered =
            targets_->register_surface( binding.target,
                                        grab::SurfaceRecord{
                                            .id         = binding.surface,
                                            .generation = display_generation_,
                                            .space =
                                                grab::CoordinateSpaceId{ rootSpace },
                                            .bounds = bounds,
                                        } );
        if( !registered.has_value() )
        {
            return std::unexpected( std::move( registered.error() ) );
        }

        const auto [position, inserted] = bindings_.emplace( window.id, binding );
        if( !inserted )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               "X11 window binding insertion failed" );
        }
        ++next_node_;
        ++next_surface_;
        return position->second;
    }

    grab::Result<void>
    X11TreeSource::retire_missing_windows( const std::set<std::uint32_t>& active_xids )
    {
        auto binding = bindings_.begin();
        while( binding != bindings_.end() )
        {
            if( active_xids.contains( binding->first ) )
            {
                ++binding;
                continue;
            }

            auto removed = targets_->remove_surface( binding->second.target,
                                                     binding->second.surface );
            if( !removed.has_value() )
            {
                return std::unexpected( std::move( removed.error() ) );
            }

            const auto alias = alias_for( binding->first );
            auto       invalidated =
                targets_->invalidate_alias( alias.authority, alias.native_id );
            if( !invalidated.has_value() )
            {
                return std::unexpected( std::move( invalidated.error() ) );
            }
            binding = bindings_.erase( binding );
        }
        return {};
    }

    grab::kernel::AliasEdge
    X11TreeSource::alias_for( std::uint32_t xid ) const
    {
        return grab::kernel::AliasEdge{
            .authority =
                grab::kernel::AliasAuthority{
                                             alias_authority_, },
            .native_id =
                grab::kernel::NativeAliasId{
                                             std::to_string( xid ),
                                             },
            .confidence = grab::kernel::AliasConfidence::Exact,
            .validity   = grab::kernel::AliasValidity::Active,
        };
    }

    grab::UiNodeRecord
    X11TreeSource::node_record( const grab::screen::WindowInfo& window,
                                const WindowBinding&            binding,
                                const grab::SpaceRect&          bounds,
                                std::uint64_t                   revision ) const
    {
        std::vector<grab::UiProperty> properties;
        properties.reserve( 5U );
        properties.push_back( string_property( grab::property::accessible_name,
                                               window.title ) );
        properties.push_back( string_property( grab::property::title, window.title ) );
        properties.push_back( string_property( grab::property::window_class,
                                               window.wm_class ) );
        properties.push_back( pid_property( window.pid ) );
        properties.push_back( grab::UiProperty{
            .id   = grab::property::bounds,
            .read = grab::PropertyRead{
                                       .state = grab::PropertyRead::State::Present,
                                       .value = bounds,
                                       },
        } );

        return grab::UiNodeRecord{
            binding.node,
            grab::NodeGeneration{1U                                  },
            grab::role::window,
            0U,
            std::move( properties ),
            grab::UiProvenance{
                                 .runtime  = runtime_,.revision = revision,
                                 },
        };
    }

}    // namespace grab::drivers::desktop::x11
