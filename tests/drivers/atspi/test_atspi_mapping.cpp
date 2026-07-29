#include "drivers/semantic/atspi/atspi_tree_source.hpp"
#include "grab/ids.hpp"
#include "grab/role.hpp"
#include "grab/ui.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    namespace atspi = grab::drivers::semantic::atspi;

    constexpr grab::RuntimeId runtimeId{ 9U };
    constexpr std::uint64_t   revision = 17U;

    [[nodiscard]]
    atspi::AtspiAccessible
    accessible( atspi::AtspiRole         role,
                atspi::AtspiInterfaceSet interfaces )
    {
        return atspi::AtspiAccessible{
            .node         = grab::NodeId{ 41U },
            .generation   = grab::NodeGeneration{ 1U },
            .role         = role,
            .interfaces   = interfaces,
            .object_path  = std::string{ "/org/example/save" },
            .parent       = std::nullopt,
            .name         = std::string{ "Save" },
            .title        = std::string{},
            .text_content = std::string{},
            .url          = std::string{},
            .states       = grab::NodeState::Visible | grab::NodeState::Enabled,
            .pid          = 4'242U,
            .bounds       = std::nullopt,
            .x11_window   = std::nullopt,
        };
    }

}    // namespace

TEST( AtspiMapping,
      MapsSemanticRoles )
{
    const auto cases = std::to_array<std::pair<atspi::AtspiRole, grab::RoleId>>( {
        {atspi::AtspiRole::Document, grab::role::document},
        {  atspi::AtspiRole::Dialog,   grab::role::dialog},
        {   atspi::AtspiRole::Panel,    grab::role::panel},
        {  atspi::AtspiRole::Button,   grab::role::button},
        {    atspi::AtspiRole::Text,     grab::role::text},
        {    atspi::AtspiRole::List,     grab::role::list},
        { atspi::AtspiRole::Unknown,  grab::role::unknown},
    } );

    for( const auto& [atspi_role, expected_role] : cases )
    {
        const auto record =
            atspi::map_accessible( accessible( atspi_role, atspi::AtspiInterfaceSet{} ),
                                   runtimeId,
                                   revision );

        EXPECT_EQ( record.role, expected_role );
        EXPECT_EQ( record.id, grab::NodeId{ 41U } );
        const grab::UiProvenance expected_provenance{
            .runtime  = runtimeId,
            .revision = revision,
        };
        EXPECT_EQ( record.provenance(), expected_provenance );
    }
}

TEST( AtspiMapping,
      MapsInterfacesToObjectScopedFacets )
{
    const auto rich_record = atspi::map_accessible( accessible( atspi::AtspiRole::Text,
                                                                atspi::AtspiInterfaceSet{
                                                                    .action    = true,
                                                                    .text      = true,
                                                                    .value     = true,
                                                                    .selection = true,
                                                                } ),
                                                    runtimeId,
                                                    revision );

    EXPECT_TRUE( grab::has_facet( rich_record.facets, grab::Facet::Invokable ) );
    EXPECT_TRUE( grab::has_facet( rich_record.facets, grab::Facet::Text ) );
    EXPECT_TRUE( grab::has_facet( rich_record.facets, grab::Facet::Value ) );
    EXPECT_TRUE( grab::has_facet( rich_record.facets, grab::Facet::Selection ) );

    const auto plain_record =
        atspi::map_accessible( accessible( atspi::AtspiRole::Panel,
                                           atspi::AtspiInterfaceSet{} ),
                               runtimeId,
                               revision );

    EXPECT_FALSE( grab::has_facet( plain_record.facets, grab::Facet::Invokable ) );
    EXPECT_FALSE( grab::has_facet( plain_record.facets, grab::Facet::Text ) );
    EXPECT_FALSE( grab::has_facet( plain_record.facets, grab::Facet::Value ) );
    EXPECT_FALSE( grab::has_facet( plain_record.facets, grab::Facet::Selection ) );
}

TEST( AtspiMapping,
      MapsLinkUrlToProperty )
{
    const std::string linkUrl = "https://en.wikipedia.org/wiki/Tiger";
    auto              link     = accessible( atspi::AtspiRole::Link,
                                atspi::AtspiInterfaceSet{} );
    link.url                   = linkUrl;

    const auto record = atspi::map_accessible( link, runtimeId, revision );

    const auto url = record.property( grab::property::url );
    EXPECT_EQ( url.state, grab::PropertyRead::State::Present );
    const auto* const value = std::get_if<std::string>( &url.value );
    ASSERT_NE( value, nullptr );
    EXPECT_EQ( *value, linkUrl );
}
