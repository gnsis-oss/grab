#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "grab/context.hpp"
#include "grab/relation.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"
#include "kernel/graph/target_registry.hpp"
#include "spi/tree_source.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace grab::drivers::semantic::atspi
{
    namespace
    {

        [[nodiscard]]
        grab::UiProperty
        string_property( grab::PropertyId   id,
                         const std::string& value )
        {
            return grab::UiProperty{
                .id   = id,
                .read = value.empty() ? grab::PropertyRead{                                          }
                                      : grab::PropertyRead{
                                                           .state = grab::PropertyRead::State::Present,.value = value,
                                                           },
            };
        }

        [[nodiscard]]
        grab::kernel::AliasEdge
        object_alias( const AtspiAccessible&        accessible,
                      grab::kernel::AliasConfidence confidence )
        {
            const std::string native_id = accessible.object_path.empty()
                                            ? std::to_string( accessible.node.value )
                                            : accessible.object_path;
            return grab::kernel::AliasEdge{
                .authority  = grab::kernel::AliasAuthority{ "atspi.object" },
                .native_id  = grab::kernel::NativeAliasId{ native_id },
                .confidence = confidence,
                .validity   = grab::kernel::AliasValidity::Active,
            };
        }

    }    // namespace

    grab::RoleId
    map_role( AtspiRole role ) noexcept
    {
        switch( role )
        {
            case AtspiRole::Application :
                return grab::role::application;
            case AtspiRole::Window :
            case AtspiRole::Frame :
                return grab::role::window;
            case AtspiRole::Document :
                return grab::role::document;
            case AtspiRole::Dialog :
            case AtspiRole::Alert :
                return grab::role::dialog;
            case AtspiRole::Panel :
            case AtspiRole::Section :
            case AtspiRole::PageTabList :
                return grab::role::panel;
            case AtspiRole::Button :
            case AtspiRole::PushButton :
            case AtspiRole::ToggleButton :
                return grab::role::button;
            case AtspiRole::CheckBox :
                return grab::role::checkbox;
            case AtspiRole::RadioButton :
                return grab::role::radio_button;
            case AtspiRole::Entry :
            case AtspiRole::PasswordText :
                return grab::role::entry;
            case AtspiRole::Text :
            case AtspiRole::Paragraph :
            case AtspiRole::Heading :
                return grab::role::text;
            case AtspiRole::Link :
                return grab::role::link;
            case AtspiRole::Image :
                return grab::role::image;
            case AtspiRole::List :
                return grab::role::list;
            case AtspiRole::Table :
                return grab::role::table;
            case AtspiRole::Menu :
            case AtspiRole::MenuBar :
                return grab::role::menu;
            case AtspiRole::MenuItem :
                return grab::role::menu_item;
            case AtspiRole::PageTab :
                return grab::role::tab;
            case AtspiRole::Slider :
            case AtspiRole::SpinButton :
                return grab::role::slider;
            case AtspiRole::ComboBox :
            case AtspiRole::ListItem :
            case AtspiRole::TableCell :
                return grab::role::control;
            case AtspiRole::Unknown :
                return grab::role::unknown;
        }
        return grab::role::unknown;
    }

    grab::UiNodeRecord
    map_accessible( const AtspiAccessible& accessible,
                    grab::RuntimeId        runtime,
                    std::uint64_t          revision )
    {
        std::uint32_t facets{};
        if( accessible.interfaces.action )
        {
            facets |= grab::Facet::Invokable;
        }
        if( accessible.interfaces.text )
        {
            facets |= grab::Facet::Text;
        }
        if( accessible.interfaces.value )
        {
            facets |= grab::Facet::Value;
        }
        if( accessible.interfaces.selection )
        {
            facets |= grab::Facet::Selection;
        }

        std::vector<grab::UiProperty> properties;
        properties.reserve( 5U );
        properties.push_back( string_property( grab::property::accessible_name,
                                               accessible.name ) );
        properties.push_back( string_property( grab::property::title,
                                               accessible.title ) );
        properties.push_back( string_property( grab::property::text,
                                               accessible.text_content ) );
        properties.push_back( string_property( grab::property::url, accessible.url ) );
        properties.push_back( grab::UiProperty{
            .id = grab::property::process_id,
            .read = accessible.pid.has_value()
                        ? grab::PropertyRead{
                              .state = grab::PropertyRead::State::Present,
                              .value = static_cast<std::int64_t>( *accessible.pid ),
                          }
                        : grab::PropertyRead{},
        } );
        properties.push_back( grab::UiProperty{
            .id = grab::property::bounds,
            .read = accessible.bounds.has_value()
                        ? grab::PropertyRead{
                              .state = grab::PropertyRead::State::Present,
                              .value = *accessible.bounds,
                          }
                        : grab::PropertyRead{},
        } );

        return grab::UiNodeRecord{
            accessible.node,
            accessible.generation,
            map_role( accessible.role ),
            accessible.states,
            facets,
            std::move( properties ),
            grab::UiProvenance{
                               .runtime  = runtime,
                               .revision = revision,
                               },
        };
    }

    grab::Result<grab::kernel::TargetId>
    observe_atspi_target( grab::kernel::TargetRegistry& registry,
                          const AtspiAccessible&        accessible,
                          std::optional<std::string>    x11_alias_authority )
    {
        const bool exact_bridge = accessible.x11_window.has_value() &&
                                  x11_alias_authority.has_value() &&
                                  !x11_alias_authority->empty();
        const auto alias = exact_bridge
                               ? grab::kernel::AliasEdge{
                                     .authority = grab::kernel::AliasAuthority{
                                         *x11_alias_authority,
                                     },
                                     .native_id = grab::kernel::NativeAliasId{
                                         std::to_string( *accessible.x11_window ),
                                     },
                                     .confidence =
                                         grab::kernel::AliasConfidence::Exact,
                                     .validity = grab::kernel::AliasValidity::Active,
                                 }
                               : object_alias(
                                     accessible,
                                     grab::kernel::AliasConfidence::Candidate
                                 );

        auto target = registry.observe( grab::kernel::TargetObservation{
            .grade  = accessible.role == AtspiRole::Application
                        ? grab::kernel::TargetGrade::Application
                        : grab::kernel::TargetGrade::Window,
            .alias  = alias,
            .title  = accessible.title.empty() ? accessible.name : accessible.title,
            .pid    = accessible.pid,
            .bounds = accessible.bounds,
        } );
        if( !target.has_value() || !exact_bridge )
        {
            return target;
        }

        auto attached = registry.attach_alias(
            *target,
            object_alias( accessible, grab::kernel::AliasConfidence::Candidate )
        );
        if( !attached.has_value() )
        {
            return std::unexpected( std::move( attached.error() ) );
        }
        return target;
    }

    AtspiTreeSource::AtspiTreeSource( grab::RuntimeId               runtime,
                                      grab::kernel::TargetRegistry& targets,
                                      AccessibleEnumerator       enumerate_accessibles,
                                      std::optional<std::string> x11_alias_authority ) :
        runtime_( runtime ),
        targets_( &targets ),
        enumerate_accessibles_( std::move( enumerate_accessibles ) ),
        x11_alias_authority_( std::move( x11_alias_authority ) )
    {
        if( !enumerate_accessibles_ )
        {
            enumerate_accessibles_ = []()
            {
                return grab::Result<std::vector<AtspiAccessible>>{
                    std::vector<AtspiAccessible>{},
                };
            };
        }
    }

    grab::Result<grab::UiSnapshot>
    AtspiTreeSource::snapshot( std::uint32_t                 tree,
                               const grab::OperationContext& context )
    {
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        if( tree != firstTree )
        {
            return grab::fail( grab::ErrorCode::NoMatch,
                               "AT-SPI tree snapshot is not available" );
        }
        if( revision_ == std::numeric_limits<std::uint64_t>::max() )
        {
            return grab::fail( grab::ErrorCode::Overflowed,
                               "AT-SPI tree revision space is exhausted" );
        }

        auto accessibles = enumerate_accessibles_();
        if( !accessibles.has_value() )
        {
            return std::unexpected( std::move( accessibles.error() ) );
        }

        std::set<grab::NodeId> ids;
        for( const auto& accessible : *accessibles )
        {
            if( !ids.insert( accessible.node ).second )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "AT-SPI tree contains a duplicate node" );
            }
        }

        const std::uint64_t             next_revision = revision_ + 1U;
        std::vector<grab::UiNodeRecord> nodes;
        std::vector<grab::NodeId>       roots;
        std::vector<grab::UiRelation>   relations;
        nodes.reserve( accessibles->size() );
        roots.reserve( accessibles->size() );
        for( const auto& accessible : *accessibles )
        {
            nodes.push_back( map_accessible( accessible, runtime_, next_revision ) );
            if( !accessible.parent.has_value() || !ids.contains( *accessible.parent ) )
            {
                roots.push_back( accessible.node );
            }
            else
            {
                relations.push_back( grab::UiRelation{
                    .source   = *accessible.parent,
                    .target   = accessible.node,
                    .relation = grab::relation::contains,
                } );
            }

            if( !target_bindings_.contains( accessible.node ) )
            {
                auto target =
                    observe_atspi_target( *targets_, accessible, x11_alias_authority_ );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                target_bindings_.emplace( accessible.node, *target );
            }
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
            std::move( relations )
        );
    }

    grab::Result<std::optional<grab::spi::UiUpdate>>
    AtspiTreeSource::next_update( const grab::OperationContext& context )
    {
        const auto checked = context.check();
        if( !checked.has_value() )
        {
            return std::unexpected( checked.error() );
        }
        return std::optional<grab::spi::UiUpdate>{};
    }

}    // namespace grab::drivers::semantic::atspi
