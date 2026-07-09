#include "inventory/action.hpp"
#include "inventory/surface.hpp"
#include "inventory/surface_registry.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::inventory
{

    namespace
    {

        constexpr std::size_t surface_count      = 35U;

        constexpr double      file_header_fx     = 0.031;
        constexpr double      file_item_fx       = 0.075;
        constexpr double      toolbox_header_fx  = 0.0615;
        constexpr double      toolbox_item_fx    = 0.090;
        constexpr double      help_header_fx     = 0.093;
        constexpr double      help_item_fx       = 0.120;
        constexpr double      bell_fx            = 0.862;
        constexpr double      bell_fy            = 0.012;
        constexpr double      test_data_node_fx  = 0.035;
        constexpr double      test_data_node_fy  = 0.168;
        constexpr double      center_fx          = 0.5;
        constexpr double      center_fy          = 0.5;
        constexpr double      preferences_nav_fx = 0.345;
        constexpr double      scene2d_nav_fy     = 0.404;
        constexpr double scene3d_nav_fy = 0.434;    // NOLINT(modernize-use-std-numbers)
        constexpr double protocol_fx    = 0.499;
        constexpr double protocol_fy    = 0.488;

        constexpr double menu_surface_sleep_seconds       = 0.3;
        constexpr double main_window_sleep_seconds        = 0.4;
        constexpr double dialog_open_sleep_seconds        = 0.6;
        constexpr double marketplace_open_sleep_seconds   = 0.9;
        constexpr double preferences_open_sleep_seconds   = 0.7;
        constexpr double preferences_select_sleep_seconds = 0.5;
        constexpr double toolbox_open_sleep_seconds       = 1.0;

        constexpr int    preferences_menu_index           = 3;
        constexpr int    marketplace_menu_index           = 2;
        constexpr int    about_menu_index                 = 0;
        constexpr int    colormap_toolbox_index           = 0;
        constexpr int    fft_toolbox_index                = 1;
        constexpr int    quaternion_toolbox_index         = 2;

        constexpr std::string_view pj_app_module          = "pj_app";
        constexpr std::string_view pj_marketplace_module  = "pj_marketplace";
        constexpr std::string_view udp_server_source      = "UDP Server";

        constexpr std::string_view diagnostics_skip_reason =
            "needs the notifications bell at the calibrated position";
        constexpr std::string_view extension_detail_skip_reason =
            "reached only from inside the Marketplace window; no standalone trigger";
        constexpr std::string_view reactive_scripts_skip_reason =
            "Reactive Scripts toolbox is not bundled in this AppImage build (not in the "
            "Toolbox menu)";
        constexpr std::string_view transform_editor_skip_reason =
            "Transform Editor toolbox is not bundled in this AppImage build (not in the "
            "Toolbox menu)";
        constexpr std::string_view mosaico_panel_skip_reason =
            "Mosaico panel needs a reachable Arrow-Flight server + connect flow";
        constexpr std::string_view mosaico_cert_skip_reason =
            "Mosaico cert dialog needs the cloud connect flow / server";
        constexpr std::string_view lerobot_skip_reason =
            "no LeRobot dataset on disk (needs a HuggingFace meta/info.json download)";
        constexpr std::string_view ros2_skip_reason =
            "ROS 2 stream plugin is not bundled in this AppImage build (not in the "
            "streamer list)";

        struct SampleData
        {
                std::string_view key;
                std::string_view relative_path;
        };

        constexpr auto sample_data = std::array{
            SampleData{
                       .key           = "csv",
                       .relative_path = "pj-official-plugins/data_load_csv/test_data/"
                       "simple.csv", },
            SampleData{
                       .key           = "mcap",
                       .relative_path = "pj-official-plugins/data_load_mcap/test_data/"
                       "test_embedded_timestamp.mcap", },
            SampleData{
                       .key           = "parquet",
                       .relative_path = "pj-official-plugins/data_load_parquet/test_data/"
                       "test_tz.parquet", },
            SampleData{
                       .key           = "ulg",
                       .relative_path = "pj-official-plugins/data_load_ulog/test_data/"
                       "sample_log_small.ulg", },
        };

        void
        add_surface( std::vector<Surface>& surfaces,
                     std::string_view      name,
                     std::string_view      output,
                     std::string_view      category,
                     std::string_view      module,
                     Reachable             reachable,
                     std::vector<Step>     steps,
                     std::string_view      skip_reason = "" )
        {
            surfaces.push_back( Surface{
                .name        = std::string{ name },
                .output      = std::string{ output },
                .category    = std::string{ category },
                .module      = std::string{ module },
                .reachable   = reachable,
                .steps       = std::move( steps ),
                .skip_reason = std::string{ skip_reason },
            } );
        }

        void
        add_menu_surface( std::vector<Surface>& surfaces,
                          std::string_view      name,
                          double                header_fx )
        {
            std::string output{ "PJ4/app/" };
            output.append( name );
            output.append( ".png" );

            add_surface( surfaces,
                         name,
                         output,
                         category_menu,
                         pj_app_module,
                         Reachable::live,
                         {
                             menu_open( header_fx ),
                             sleep( menu_surface_sleep_seconds ),
                         } );
        }

        void
        add_toolbox_surface( std::vector<Surface>& surfaces,
                             std::string_view      name,
                             std::string_view      output,
                             std::string_view      module,
                             int                   index )
        {
            add_surface( surfaces,
                         name,
                         output,
                         category_dialog,
                         module,
                         Reachable::live,
                         {
                             menu_item( toolbox_header_fx, toolbox_item_fx, index ),
                             sleep( toolbox_open_sleep_seconds ),
                         } );
        }

        void
        add_loader_surface( std::vector<Surface>& surfaces,
                            std::string_view      name,
                            std::string_view      output,
                            std::string_view      module,
                            std::string_view      key )
        {
            add_surface( surfaces,
                         name,
                         output,
                         category_dialog,
                         module,
                         Reachable::live,
                         { load( key ) } );
        }

        void
        add_stream_surface( std::vector<Surface>& surfaces,
                            std::string_view      name,
                            std::string_view      output,
                            std::string_view      module,
                            std::string_view      source,
                            Reachable             reachable,
                            std::string_view      skip_reason )
        {
            add_surface( surfaces,
                         name,
                         output,
                         category_dialog,
                         module,
                         reachable,
                         { open_stream( source ) },
                         skip_reason );
        }

        void
        add_live_stream_surface( std::vector<Surface>& surfaces,
                                 std::string_view      name,
                                 std::string_view      output,
                                 std::string_view      module,
                                 std::string_view      source )
        {
            add_stream_surface( surfaces,
                                name,
                                output,
                                module,
                                source,
                                Reachable::live,
                                "" );
        }

        void
        add_parser_surface( std::vector<Surface>& surfaces,
                            std::string_view      name,
                            std::string_view      output,
                            std::string_view      module,
                            std::string_view      value )
        {
            add_surface( surfaces,
                         name,
                         output,
                         category_dialog,
                         module,
                         Reachable::live,
                         {
                             open_stream( udp_server_source ),
                             set_combo( protocol_fx, protocol_fy, value ),
                         } );
        }

        void
        append_core_surfaces( std::vector<Surface>& surfaces )
        {
            add_surface( surfaces,
                         "main-window",
                         "PJ4/app/main-window.png",
                         category_dock,
                         pj_app_module,
                         Reachable::live,
                         { activate(), sleep( main_window_sleep_seconds ) } );
            add_menu_surface( surfaces, "menu-file", file_header_fx );
            add_menu_surface( surfaces, "menu-toolbox", toolbox_header_fx );
            add_menu_surface( surfaces, "menu-help", help_header_fx );
            add_surface(
                surfaces,
                "preferences",
                "PJ4/app/preferences.png",
                category_dialog,
                pj_app_module,
                Reachable::live,
                {
                    menu_item( file_header_fx, file_item_fx, preferences_menu_index ),
                    sleep( dialog_open_sleep_seconds ),
                }
            );
            add_surface( surfaces,
                         "about",
                         "PJ4/app/about.png",
                         category_dialog,
                         pj_app_module,
                         Reachable::live,
                         {
                             menu_item( help_header_fx, help_item_fx, about_menu_index ),
                             sleep( dialog_open_sleep_seconds ),
                         } );
            add_surface(
                surfaces,
                "marketplace-window",
                "PJ4/marketplace/marketplace-window.png",
                category_dialog,
                pj_marketplace_module,
                Reachable::live,
                {
                    menu_item( file_header_fx, file_item_fx, marketplace_menu_index ),
                    sleep( marketplace_open_sleep_seconds ),
                }
            );
            add_surface( surfaces,
                         "diagnostics-popup",
                         "PJ4/app/diagnostics-popup.png",
                         category_dock,
                         pj_app_module,
                         Reachable::best_effort,
                         {
                             activate(),
                             click( bell_fx, bell_fy ),
                             sleep( dialog_open_sleep_seconds ),
                         },
                         diagnostics_skip_reason );
            add_surface( surfaces,
                         "context-curve-list",
                         "PJ4/app/context-curve-list.png",
                         category_menu,
                         pj_app_module,
                         Reachable::live,
                         {
                             activate(),
                             rclick( test_data_node_fx, test_data_node_fy ),
                             sleep( dialog_open_sleep_seconds ),
                         } );
            add_surface( surfaces,
                         "context-plot",
                         "PJ4/app/context-plot.png",
                         category_menu,
                         pj_app_module,
                         Reachable::live,
                         {
                             activate(),
                             rclick( center_fx, center_fy ),
                             sleep( dialog_open_sleep_seconds ),
                         } );
            add_surface(
                surfaces,
                "scene2d-config",
                "PJ4/scene2d/scene2d-config.png",
                category_dock,
                pj_app_module,
                Reachable::live,
                {
                    menu_item( file_header_fx, file_item_fx, preferences_menu_index ),
                    sleep( preferences_open_sleep_seconds ),
                    click( preferences_nav_fx, scene2d_nav_fy ),
                    sleep( preferences_select_sleep_seconds ),
                }
            );
            add_surface(
                surfaces,
                "scene3d-config",
                "PJ4/scene3d/scene3d-config.png",
                category_dock,
                pj_app_module,
                Reachable::live,
                {
                    menu_item( file_header_fx, file_item_fx, preferences_menu_index ),
                    sleep( preferences_open_sleep_seconds ),
                    click( preferences_nav_fx, scene3d_nav_fy ),
                    sleep( preferences_select_sleep_seconds ),
                }
            );
            add_surface( surfaces,
                         "extension-detail-dialog",
                         "PJ4/marketplace/extension-detail-dialog.png",
                         category_dialog,
                         pj_marketplace_module,
                         Reachable::best_effort,
                         {},
                         extension_detail_skip_reason );
        }

        void
        append_toolbox_surfaces( std::vector<Surface>& surfaces )
        {
            add_toolbox_surface( surfaces,
                                 "toolbox-colormap",
                                 "plugins/toolbox_colormap/colormap-dialog.png",
                                 "toolbox_colormap",
                                 colormap_toolbox_index );
            add_toolbox_surface( surfaces,
                                 "toolbox-fft",
                                 "plugins/toolbox_fft/fft-dialog.png",
                                 "toolbox_fft",
                                 fft_toolbox_index );
            add_toolbox_surface( surfaces,
                                 "toolbox-quaternion",
                                 "plugins/toolbox_quaternion/quaternion-dialog.png",
                                 "toolbox_quaternion",
                                 quaternion_toolbox_index );
            add_surface( surfaces,
                         "toolbox-reactive-scripts",
                         "plugins/toolbox_reactive_scripts_editor/"
                         "reactive-script-editor-dialog.png",
                         category_dialog,
                         "toolbox_reactive_scripts_editor",
                         Reachable::best_effort,
                         {},
                         reactive_scripts_skip_reason );
            add_surface( surfaces,
                         "toolbox-transform-editor",
                         "plugins/toolbox_transform_editor/transform-editor-dialog.png",
                         category_dialog,
                         "toolbox_transform_editor",
                         Reachable::best_effort,
                         {},
                         transform_editor_skip_reason );
            add_surface( surfaces,
                         "toolbox-mosaico-panel",
                         "plugins/toolbox_mosaico/mosaico-panel.png",
                         category_dock,
                         "toolbox_mosaico",
                         Reachable::needs_server,
                         {},
                         mosaico_panel_skip_reason );
            add_surface( surfaces,
                         "mosaico-cert-dialog",
                         "plugins/toolbox_mosaico/cert-dialog.png",
                         category_dialog,
                         "toolbox_mosaico",
                         Reachable::needs_server,
                         {},
                         mosaico_cert_skip_reason );
        }

        void
        append_dataload_surfaces( std::vector<Surface>& surfaces )
        {
            add_loader_surface( surfaces,
                                "dataload-csv",
                                "plugins/data_load_csv/dataload-csv.png",
                                "data_load_csv",
                                "csv" );
            add_loader_surface( surfaces,
                                "dialog-mcap",
                                "plugins/data_load_mcap/dialog-mcap.png",
                                "data_load_mcap",
                                "mcap" );
            add_loader_surface( surfaces,
                                "dataload-parquet",
                                "plugins/data_load_parquet/dataload-parquet.png",
                                "data_load_parquet",
                                "parquet" );
            add_loader_surface( surfaces,
                                "ulog-params",
                                "plugins/data_load_ulog/ulog-params.png",
                                "data_load_ulog",
                                "ulg" );
            add_surface( surfaces,
                         "dialog-lerobot",
                         "plugins/data_load_lerobot/dialog-lerobot.png",
                         category_dialog,
                         "data_load_lerobot",
                         Reachable::needs_data,
                         {},
                         lerobot_skip_reason );
        }

        void
        append_stream_surfaces( std::vector<Surface>& surfaces )
        {
            add_live_stream_surface( surfaces,
                                     "datastream-udp",
                                     "plugins/data_stream_udp/datastream-udp.png",
                                     "data_stream_udp",
                                     udp_server_source );
            add_live_stream_surface( surfaces,
                                     "datastream-zmq",
                                     "plugins/data_stream_zmq/datastream-zmq.png",
                                     "data_stream_zmq",
                                     "ZMQ Subscriber" );
            add_live_stream_surface( surfaces,
                                     "datastream-mqtt",
                                     "plugins/data_stream_mqtt/datastream-mqtt.png",
                                     "data_stream_mqtt",
                                     "MQTT Subscriber" );
            add_live_stream_surface( surfaces,
                                     "datastream-webrtc",
                                     "plugins/data_stream_webrtc/datastream-webrtc.png",
                                     "data_stream_webrtc",
                                     "WebRTC Video Client" );
            add_live_stream_surface(
                surfaces,
                "foxglove-client",
                "plugins/data_stream_foxglove_bridge/foxglove-client.png",
                "data_stream_foxglove_bridge",
                "Foxglove Bridge"
            );
            add_live_stream_surface(
                surfaces,
                "websocket-client",
                "plugins/data_stream_pj_bridge/websocket-client.png",
                "data_stream_pj_bridge",
                "PlotJuggler Bridge"
            );
            add_surface( surfaces,
                         "datastream-ros2",
                         "plugins/data_stream_ros2/datastream-ros2.png",
                         category_dialog,
                         "data_stream_ros2",
                         Reachable::needs_server,
                         {},
                         ros2_skip_reason );
            add_parser_surface( surfaces,
                                "json-parser-options",
                                "plugins/parser_json/json-parser-options.png",
                                "parser_json",
                                "json" );
            add_parser_surface( surfaces,
                                "protobuf-parser-options",
                                "plugins/parser_protobuf/protobuf-parser-options.png",
                                "parser_protobuf",
                                "protobuf" );
            add_parser_surface( surfaces,
                                "ros-parser-options",
                                "plugins/parser_ros/ros-parser-options.png",
                                "parser_ros",
                                "ros2msg" );
        }

        [[nodiscard]]
        std::vector<Surface>
        make_all_surfaces()
        {
            std::vector<Surface> surfaces;
            surfaces.reserve( surface_count );
            append_core_surfaces( surfaces );
            append_toolbox_surfaces( surfaces );
            append_dataload_surfaces( surfaces );
            append_stream_surfaces( surfaces );
            return surfaces;
        }

    }    // namespace

    const std::vector<Surface>&
    all_surfaces()
    {
        static const auto surfaces = make_all_surfaces();
        return surfaces;
    }

    std::optional<std::string_view>
    surface_sample_rel( std::string_view key ) noexcept
    {
        for( const auto& item : sample_data )
        {
            if( item.key == key )
            {
                return item.relative_path;
            }
        }
        return std::nullopt;
    }

}    // namespace grab::inventory
