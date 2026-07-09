#include "grab/window.hpp"
#include "input/fake_input_sink.hpp"
#include "input/input_sink.hpp"
#include "inventory/action.hpp"
#include "inventory/manifest.hpp"
#include "inventory/sample.hpp"
#include "inventory/step_runner.hpp"
#include "inventory/surface_registry.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    namespace inventory                          = grab::inventory;
    namespace test                               = grab::test;

    constexpr std::string_view temp_directory    = "grab_inventory_driver_test";
    constexpr std::string_view csv_key           = "csv";
    constexpr std::string_view missing_key       = "missing";
    constexpr std::string_view csv_reference     = "@csv";
    constexpr std::string_view missing_reference = "@missing";
    constexpr std::string_view sample_contents   = "time,value\n0,1\n";
    constexpr std::string_view missing_message   = "no sample data for 'missing'";

    constexpr grab::WindowRect step_rect{
        .x      = 10,
        .y      = 20,
        .width  = 100U,
        .height = 100U,
    };
    constexpr double             click_fx       = 0.5;
    constexpr double             click_fy       = 0.25;
    constexpr std::uint8_t       right_button   = 3U;
    constexpr bool               press          = true;
    constexpr bool               release        = false;
    constexpr bool               keep_modifiers = false;
    constexpr double             sleep_seconds  = 0.4;
    constexpr std::uint32_t      sleep_millis   = 400U;
    constexpr grab::input::Point right_click_point{
        .x = 60,
        .y = 45,
    };

    class TempTree
    {
        public:

            TempTree()
            {
                std::error_code error;
                std::filesystem::remove_all( root_path, error );
                error.clear();
                std::filesystem::create_directories( root_path, error );
                EXPECT_FALSE( error );
            }

            TempTree( const TempTree& )     = delete;
            TempTree( TempTree&& ) noexcept = delete;
            TempTree&
            operator=( const TempTree& ) = delete;
            TempTree&
            operator=( TempTree&& ) noexcept = delete;

            ~TempTree()
            {
                std::error_code error;
                std::filesystem::remove_all( root_path, error );
            }

            [[nodiscard]]
            const std::filesystem::path&
            root() const noexcept
            {
                return root_path;
            }

        private:

            std::filesystem::path root_path =
                std::filesystem::temp_directory_path() / std::string{ temp_directory };
    };

    void
    write_text_file( const std::filesystem::path& path,
                     std::string_view             contents )
    {
        std::error_code error;
        std::filesystem::create_directories( path.parent_path(), error );
        ASSERT_FALSE( error );

        std::ofstream output{ path };
        ASSERT_TRUE( output.is_open() );
        output << contents;
        ASSERT_TRUE( output );
    }

    [[nodiscard]]
    std::string
    read_text_file( const std::filesystem::path& path )
    {
        std::ifstream input{ path };
        EXPECT_TRUE( input.is_open() );
        return std::string{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
    }

    [[nodiscard]]
    std::filesystem::path
    create_csv_sample( const TempTree& tree )
    {
        const auto        relative = inventory::surface_sample_rel( csv_key );
        const std::string relative_path{ relative.value_or( "" ) };
        EXPECT_FALSE( relative_path.empty() );
        const auto path = tree.root() / std::filesystem::path{ relative_path };
        write_text_file( path, sample_contents );
        return path;
    }

}    // namespace

TEST( InventoryDriver,
      ResolveSampleReturnsAbsoluteExistingSample )
{
    const TempTree tree;
    const auto     csv_path = create_csv_sample( tree );

    const auto     resolved = inventory::resolve_sample( tree.root().string(), csv_key );

    ASSERT_TRUE( resolved.has_value() );
    EXPECT_EQ( *resolved, std::filesystem::absolute( csv_path ).string() );
    EXPECT_FALSE(
        inventory::resolve_sample( tree.root().string(), missing_key ).has_value()
    );
}

TEST( InventoryDriver,
      ResolveStepSamplesRewritesSampleReferences )
{
    const TempTree                     tree;
    const auto                         csv_path = create_csv_sample( tree );
    const std::vector<inventory::Step> steps{
        inventory::Step{ inventory::LoadFileStep{
            .path = std::string{ csv_reference },
        } },
        inventory::Step{ inventory::TypeStep{ .text = std::string{ csv_reference } } },
    };

    const auto resolved = inventory::resolve_step_samples( steps, tree.root().string() );

    ASSERT_TRUE( resolved.has_value() ) << resolved.error().message;
    ASSERT_EQ( resolved->size(), steps.size() );
    const auto* const load = std::get_if<inventory::LoadFileStep>( &resolved->at( 0U ) );
    ASSERT_NE( load, nullptr );
    EXPECT_EQ( load->path, std::filesystem::absolute( csv_path ).string() );
    const auto* const type = std::get_if<inventory::TypeStep>( &resolved->at( 1U ) );
    ASSERT_NE( type, nullptr );
    EXPECT_EQ( type->text, std::filesystem::absolute( csv_path ).string() );
}

TEST( InventoryDriver,
      ResolveStepSamplesErrorsOnMissingSampleKey )
{
    const TempTree                     tree;
    const std::vector<inventory::Step> steps{
        inventory::Step{ inventory::LoadFileStep{
            .path = std::string{ missing_reference },
        } },
    };

    const auto resolved = inventory::resolve_step_samples( steps, tree.root().string() );

    ASSERT_FALSE( resolved.has_value() );
    EXPECT_EQ( resolved.error().message, missing_message );
}

TEST( InventoryDriver,
      RunStepDispatchesRightClick )
{
    test::FakeInputSink sink;

    inventory::run_step( sink,
                         step_rect,
                         inventory::Step{
                             inventory::ClickFracStep{
                                                      .fx     = click_fx,
                                                      .fy     = click_fy,
                                                      .button = right_button,
                                                      }
    } );

    const std::vector<test::Op> expected{
        test::Move{ .p = right_click_point },
        test::Button{
                   .code            = right_button,
                   .press           = press,
                   .clear_modifiers = keep_modifiers,
                   },
        test::Button{
                   .code            = right_button,
                   .press           = release,
                   .clear_modifiers = keep_modifiers,
                   },
    };
    EXPECT_EQ( sink.ops(), expected );
}

TEST( InventoryDriver,
      RunStepDispatchesSleepSecondsAsMilliseconds )
{
    test::FakeInputSink sink;

    inventory::run_step( sink,
                         step_rect,
                         inventory::Step{
                             inventory::SleepStep{
                                                  .seconds = sleep_seconds,
                                                  }
    } );

    const std::vector<test::Op> expected{
        test::Wait{ .millis = sleep_millis },
    };
    EXPECT_EQ( sink.ops(), expected );
}

TEST( InventoryDriver,
      ManifestSortsEntriesAndKeepsFieldOrder )
{
    const TempTree                      tree;
    const auto                          manifest_path = tree.root() / "manifest.json";
    const std::vector<inventory::Entry> entries{
        inventory::Entry{
                         .name          = "zeta",
                         .category      = "dock-panel",
                         .module        = "pj_app",
                         .source_file   = "",
                         .render_method = "live",
                         .output_path   = "b.png",
                         .status        = "skipped",
                         .notes         = "later",
                         },
        inventory::Entry{
                         .name          = "alpha",
                         .category      = "dialog-wizard",
                         .module        = "parser_json",
                         .source_file   = "source.ui",
                         .render_method = "live",
                         .output_path   = "a.png",
                         .status        = "ok",
                         .notes         = "",
                         },
    };
    constexpr std::string_view expected_json = "[\n"
                                               "  {\n"
                                               "    \"name\": \"alpha\",\n"
                                               "    \"category\": \"dialog-wizard\",\n"
                                               "    \"module\": \"parser_json\",\n"
                                               "    \"source_file\": \"source.ui\",\n"
                                               "    \"render_method\": \"live\",\n"
                                               "    \"output_path\": \"a.png\",\n"
                                               "    \"status\": \"ok\",\n"
                                               "    \"notes\": \"\"\n"
                                               "  },\n"
                                               "  {\n"
                                               "    \"name\": \"zeta\",\n"
                                               "    \"category\": \"dock-panel\",\n"
                                               "    \"module\": \"pj_app\",\n"
                                               "    \"source_file\": \"\",\n"
                                               "    \"render_method\": \"live\",\n"
                                               "    \"output_path\": \"b.png\",\n"
                                               "    \"status\": \"skipped\",\n"
                                               "    \"notes\": \"later\"\n"
                                               "  }\n"
                                               "]\n";

    const auto result = inventory::write_manifest( manifest_path.string(), entries );

    ASSERT_TRUE( result.has_value() ) << result.error().message;
    EXPECT_EQ( read_text_file( manifest_path ), expected_json );
}
