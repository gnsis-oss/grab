#include "inventory/action.hpp"
#include "inventory/surface.hpp"
#include "inventory/surface_registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
// clang-format on

namespace
{

    namespace inventory                               = grab::inventory;

    constexpr auto             expected_surface_count = 35U;
    constexpr auto             first_surface_index    = 0U;
    constexpr auto             first_step_index       = 0U;
    constexpr auto             second_step_index      = 1U;
    constexpr std::size_t      about_step_count       = 2U;
    constexpr std::size_t      load_file_step_count   = 1U;
    constexpr std::size_t      parser_step_count      = 2U;

    constexpr double           help_header_fx         = 0.093;
    constexpr double           help_item_fx           = 0.120;
    constexpr double           about_sleep_seconds    = 0.6;
    constexpr int              about_menu_index       = 0;
    constexpr int              fft_menu_index         = 1;
    constexpr std::uint8_t     right_button           = 3U;

    constexpr std::string_view main_window_name       = "main-window";
    constexpr std::string_view about_name             = "about";
    constexpr std::string_view context_plot_name      = "context-plot";
    constexpr std::string_view toolbox_fft_name       = "toolbox-fft";
    constexpr std::string_view dataload_csv_name      = "dataload-csv";
    constexpr std::string_view json_parser_name       = "json-parser-options";
    constexpr std::string_view extension_detail_name  = "extension-detail-dialog";
    constexpr std::string_view mosaico_panel_name     = "toolbox-mosaico-panel";
    constexpr std::string_view dialog_category        = "dialog-wizard";
    constexpr std::string_view csv_load_path          = "@csv";
    constexpr std::string_view json_parser_value      = "json";
    constexpr std::string_view udp_server_source      = "UDP Server";
    constexpr std::string_view csv_sample_key         = "csv";
    constexpr std::string_view missing_sample_key     = "missing";
    constexpr std::string_view csv_sample_path =
        "pj-official-plugins/data_load_csv/test_data/simple.csv";

    [[nodiscard]]
    const inventory::Surface*
    find_surface( std::string_view name ) noexcept
    {
        const auto& surfaces = inventory::all_surfaces();
        const auto  match =
            std::ranges::find_if( surfaces,
                                  [name]( const inventory::Surface& surface )
                                  {
                                      return surface.name == name;
                                  } );
        if( match == surfaces.end() )
        {
            return nullptr;
        }
        return &*match;
    }

}    // namespace

TEST( SurfaceRegistry,
      ContainsExpectedSurfacesInOrder )
{
    const auto& surfaces = inventory::all_surfaces();

    ASSERT_EQ( surfaces.size(), expected_surface_count );
    EXPECT_EQ( surfaces.at( first_surface_index ).name, main_window_name );
}

TEST( SurfaceRegistry,
      AboutSurfaceKeepsMenuItemAndSleepSteps )
{
    const auto* const surface = find_surface( about_name );

    ASSERT_NE( surface, nullptr );
    EXPECT_EQ( surface->category, dialog_category );
    ASSERT_EQ( surface->steps.size(), about_step_count );

    const auto* const menu =
        std::get_if<inventory::MenuItemStep>( &surface->steps.at( first_step_index ) );
    ASSERT_NE( menu, nullptr );
    EXPECT_DOUBLE_EQ( menu->header_fx, help_header_fx );
    EXPECT_DOUBLE_EQ( menu->item_fx, help_item_fx );
    EXPECT_EQ( menu->index, about_menu_index );

    const auto* const sleep =
        std::get_if<inventory::SleepStep>( &surface->steps.at( second_step_index ) );
    ASSERT_NE( sleep, nullptr );
    EXPECT_DOUBLE_EQ( sleep->seconds, about_sleep_seconds );
}

TEST( SurfaceRegistry,
      ContextPlotUsesRightClickStep )
{
    const auto* const surface = find_surface( context_plot_name );

    ASSERT_NE( surface, nullptr );
    ASSERT_GT( surface->steps.size(), second_step_index );

    const auto* const click =
        std::get_if<inventory::ClickFracStep>( &surface->steps.at( second_step_index ) );
    ASSERT_NE( click, nullptr );
    EXPECT_EQ( click->button, right_button );
}

TEST( SurfaceRegistry,
      ToolboxFftUsesSecondMenuItem )
{
    const auto* const surface = find_surface( toolbox_fft_name );

    ASSERT_NE( surface, nullptr );
    ASSERT_FALSE( surface->steps.empty() );

    const auto* const menu =
        std::get_if<inventory::MenuItemStep>( &surface->steps.at( first_step_index ) );
    ASSERT_NE( menu, nullptr );
    EXPECT_EQ( menu->index, fft_menu_index );
}

TEST( SurfaceRegistry,
      DataloadCsvUsesLiteralSampleKeyPath )
{
    const auto* const surface = find_surface( dataload_csv_name );

    ASSERT_NE( surface, nullptr );
    ASSERT_EQ( surface->steps.size(), load_file_step_count );
    ASSERT_TRUE( std::holds_alternative<inventory::LoadFileStep>(
        surface->steps.at( first_step_index )
    ) );

    const auto* const load =
        std::get_if<inventory::LoadFileStep>( &surface->steps.at( first_step_index ) );
    ASSERT_NE( load, nullptr );
    EXPECT_EQ( load->path, csv_load_path );
}

TEST( SurfaceRegistry,
      JsonParserOptionsOpenUdpAndSelectJson )
{
    const auto* const surface = find_surface( json_parser_name );

    ASSERT_NE( surface, nullptr );
    ASSERT_EQ( surface->steps.size(), parser_step_count );

    const auto* const stream =
        std::get_if<inventory::OpenStreamStep>( &surface->steps.at( first_step_index ) );
    ASSERT_NE( stream, nullptr );
    EXPECT_EQ( stream->source, udp_server_source );

    const auto* const combo =
        std::get_if<inventory::SetComboStep>( &surface->steps.at( second_step_index ) );
    ASSERT_NE( combo, nullptr );
    EXPECT_EQ( combo->value, json_parser_value );
}

TEST( SurfaceRegistry,
      AttemptableRequiresAttemptableTierAndSteps )
{
    const auto* const live_surface      = find_surface( main_window_name );
    const auto* const extension_surface = find_surface( extension_detail_name );
    const auto* const needs_server      = find_surface( mosaico_panel_name );

    ASSERT_NE( live_surface, nullptr );
    ASSERT_NE( extension_surface, nullptr );
    ASSERT_NE( needs_server, nullptr );

    EXPECT_TRUE( inventory::attemptable( *live_surface ) );
    EXPECT_FALSE( inventory::attemptable( *extension_surface ) );
    EXPECT_FALSE( inventory::attemptable( *needs_server ) );
}

TEST( SurfaceRegistry,
      ExposesSampleDataRelativePaths )
{
    const auto csv_sample = inventory::surface_sample_rel( csv_sample_key );

    ASSERT_TRUE( csv_sample.has_value() );
    EXPECT_EQ( *csv_sample, csv_sample_path );
    EXPECT_FALSE( inventory::surface_sample_rel( missing_sample_key ).has_value() );
}
