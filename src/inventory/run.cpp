#include "codec/png/png_encoder.hpp"
#include "core/checked.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/window.hpp"
#include "inventory/action.hpp"
#include "inventory/manifest.hpp"
#include "inventory/process.hpp"
#include "inventory/run.hpp"
#include "inventory/sample.hpp"
#include "inventory/step_runner.hpp"
#include "inventory/surface.hpp"
#include "inventory/surface_registry.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_screen_provider.hpp"
#include "platform/x11/xcb_window.hpp"
#include "platform/x11/xkb_keymap.hpp"
#include "platform/x11/xtest_input.hpp"

// NOLINTBEGIN(llvm-include-order)
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
// NOLINTEND(llvm-include-order)

namespace grab::inventory
{

    namespace
    {

        namespace x11                                      = grab::platform::x11;

        constexpr pid_t            child_still_running     = 0;
        constexpr pid_t            wait_failed             = -1;
        constexpr std::size_t      minimum_png_byte_count  = 8'000U;
        constexpr std::string_view display_environment     = "DISPLAY=";
        constexpr std::string_view live_render_method      = "live";
        constexpr std::string_view ok_status               = "ok";
        constexpr std::string_view skipped_status          = "skipped";
        constexpr std::string_view test_data_arg           = "--test-data";
        constexpr std::string_view no_splash_arg           = "--nosplash";
        constexpr std::string_view plot_juggler_window_app = "plotjuggler";
        constexpr auto             window_poll_timeout     = std::chrono::seconds{ 12 };
        constexpr auto window_poll_interval = std::chrono::milliseconds{ 500 };
        // Mirror driver/live.py's fixed _APP_START_SECONDS = 9.0: wait after launch
        // so transient startup state (e.g. a launch notification) settles before the
        // window is captured, keeping grabs stable across runs.
        constexpr auto app_settle_delay = std::chrono::seconds{ 9 };

        struct CaptureRegion
        {
                std::int16_t  x      = 0;
                std::int16_t  y      = 0;
                std::uint16_t width  = 0U;
                std::uint16_t height = 0U;
        };

        struct WindowSearchResult
        {
                WindowRef window;
                bool      found  = false;
                bool      exited = false;
        };

        class AppProcess
        {
            public:

                explicit AppProcess( pid_t pid ) noexcept :
                    process_id( pid )
                {
                }

                AppProcess( const AppProcess& )     = delete;
                AppProcess( AppProcess&& ) noexcept = delete;
                AppProcess&
                operator=( const AppProcess& ) = delete;
                AppProcess&
                operator=( AppProcess&& ) noexcept = delete;

                ~AppProcess()
                {
                    if( !reaped )
                    {
                        terminate_app( process_id );
                    }
                }

                [[nodiscard]]
                pid_t
                pid() const noexcept
                {
                    return process_id;
                }

                void
                mark_reaped() noexcept
                {
                    reaped = true;
                }

            private:

                pid_t process_id = 0;
                bool  reaped     = false;
        };

        [[nodiscard]]
        std::string
        filesystem_error_message( std::string_view       operation,
                                  const std::error_code& error )
        {
            std::string message{ operation };
            message += ": ";
            message += error.message();
            return message;
        }

        [[nodiscard]]
        bool
        is_executable_file( const std::filesystem::path& path )
        {
            std::error_code error;
            if( !std::filesystem::is_regular_file( path, error ) )
            {
                return false;
            }
            const std::string path_string = path.string();
            return ::access( path_string.c_str(), X_OK ) == 0;
        }

        [[nodiscard]]
        bool
        display_available()
        {
            for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
                 entry              = std::next( entry ) )
            {
                const std::string_view variable{ *entry };
                if( variable.starts_with( display_environment ) )
                {
                    return variable.size() > display_environment.size();
                }
            }
            return false;
        }

        [[nodiscard]]
        Entry
        make_entry( const Surface&   surface,
                    std::string_view status,
                    std::string_view notes )
        {
            return Entry{
                .name          = surface.name,
                .category      = surface.category,
                .module        = surface.module,
                .source_file   = {},
                .render_method = std::string{ live_render_method },
                .output_path   = surface.output,
                .status        = std::string{ status },
                .notes         = std::string{ notes },
            };
        }

        [[nodiscard]]
        std::vector<Entry>
        environment_skip_entries( std::string_view reason )
        {
            std::vector<Entry> entries;
            const auto&        surfaces = all_surfaces();
            entries.reserve( surfaces.size() );
            for( const Surface& surface : surfaces )
            {
                const std::string_view notes =
                    !attemptable( surface ) && !surface.skip_reason.empty()
                        ? std::string_view{ surface.skip_reason }
                        : reason;
                entries.push_back( make_entry( surface, skipped_status, notes ) );
            }
            return entries;
        }

        [[nodiscard]]
        bool
        child_exited( pid_t pid )
        {
            int         status = 0;
            // NOLINTNEXTLINE(misc-include-cleaner)
            const pid_t result = ::waitpid( pid, &status, WNOHANG );
            if( result == pid )
            {
                return true;
            }
            if( result == wait_failed )
            {
                return errno == ECHILD;
            }
            return result != child_still_running;
        }

        [[nodiscard]]
        WindowSearchResult
        wait_for_window( const x11::XcbConnection& conn,
                         AppProcess&               app )
        {
            const auto deadline = std::chrono::steady_clock::now() + window_poll_timeout;
            while( std::chrono::steady_clock::now() < deadline )
            {
                auto window = x11::find_window(
                    conn,
                    WindowMatch{ .app = std::string{ plot_juggler_window_app } }
                );
                if( window.has_value() )
                {
                    WindowSearchResult result;
                    result.window = *window;
                    result.found  = true;
                    return result;
                }

                if( child_exited( app.pid() ) )
                {
                    app.mark_reaped();
                    WindowSearchResult result;
                    result.exited = true;
                    return result;
                }

                std::this_thread::sleep_for( window_poll_interval );
            }

            return WindowSearchResult{};
        }

        template<typename To,
                 typename From>
        [[nodiscard]]
        grab::Result<To>
        checked_capture_value( From value )
        {
            return grab::checked_cast<To>( value,
                                           grab::ErrorCode::invalid_argument,
                                           "window geometry is out of capture range" );
        }

        [[nodiscard]]
        grab::Result<CaptureRegion>
        capture_region_from_rect( const WindowRect& rect )
        {
            auto x = checked_capture_value<std::int16_t>( rect.x );
            if( !x.has_value() )
            {
                return grab::fail( x.error().code, x.error().message );
            }
            auto y = checked_capture_value<std::int16_t>( rect.y );
            if( !y.has_value() )
            {
                return grab::fail( y.error().code, y.error().message );
            }
            auto width = checked_capture_value<std::uint16_t>( rect.width );
            if( !width.has_value() )
            {
                return grab::fail( width.error().code, width.error().message );
            }
            auto height = checked_capture_value<std::uint16_t>( rect.height );
            if( !height.has_value() )
            {
                return grab::fail( height.error().code, height.error().message );
            }
            return CaptureRegion{
                .x      = *x,
                .y      = *y,
                .width  = *width,
                .height = *height,
            };
        }

        [[nodiscard]]
        grab::Result<void>
        write_binary_file( const std::filesystem::path&     path,
                           const std::vector<std::uint8_t>& bytes )
        {
            const std::filesystem::path parent = path.parent_path();
            if( !parent.empty() )
            {
                std::error_code error;
                std::filesystem::create_directories( parent, error );
                if( error )
                {
                    return grab::fail(
                        grab::ErrorCode::internal_fault,
                        filesystem_error_message( "create output directory", error )
                    );
                }
            }

            std::ofstream output{ path, std::ios::binary };
            if( !output.is_open() )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   "failed to open output path" );
            }
            const std::string contents{ bytes.begin(), bytes.end() };
            output << contents;
            if( !output )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   "failed to write output path" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::size_t>
        capture_png( const x11::XcbConnection&    conn,
                     const WindowRect&            rect,
                     const std::filesystem::path& output_path )
        {
            auto region = capture_region_from_rect( rect );
            if( !region.has_value() )
            {
                return grab::fail( region.error().code, region.error().message );
            }

            auto image = x11::capture_region( conn,
                                              region->x,
                                              region->y,
                                              region->width,
                                              region->height,
                                              true );
            if( !image.has_value() )
            {
                return grab::fail( image.error().code, image.error().message );
            }

            auto encoded = grab::codec::encode_png( image->view() );
            if( !encoded.has_value() )
            {
                return grab::fail( encoded.error().code, encoded.error().message );
            }

            auto written = write_binary_file( output_path, *encoded );
            if( !written.has_value() )
            {
                return grab::fail( written.error().code, written.error().message );
            }
            return encoded->size();
        }

        [[nodiscard]]
        grab::Result<void>
        drive_and_capture( const x11::XcbConnection&    conn,
                           const x11::XkbKeymap&        keymap,
                           const WindowRef&             window,
                           const std::vector<Step>&     steps,
                           const std::filesystem::path& output_path )
        {
            auto rect = x11::window_geometry( conn, window );
            if( !rect.has_value() )
            {
                return grab::fail( rect.error().code, rect.error().message );
            }

            x11::XtestInputSink sink{ conn, keymap, window };
            for( const Step& step : steps )
            {
                run_step( sink, *rect, step );
            }

            auto png_size = capture_png( conn, *rect, output_path );
            if( !png_size.has_value() )
            {
                return grab::fail( png_size.error().code, png_size.error().message );
            }
            if( *png_size < minimum_png_byte_count )
            {
                std::error_code error;
                std::filesystem::remove( output_path, error );
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "grabbed PNG too small" );
            }
            return {};
        }

        [[nodiscard]]
        Entry
        capture_surface( const Surface&               surface,
                         const x11::XcbConnection&    conn,
                         const x11::XkbKeymap&        keymap,
                         std::string_view             root,
                         const std::filesystem::path& out_dir,
                         std::string_view             apprun )
        {
            if( !attemptable( surface ) )
            {
                return make_entry( surface, skipped_status, surface.skip_reason );
            }

            auto resolved_steps = resolve_step_samples( surface.steps, root );
            if( !resolved_steps.has_value() )
            {
                return make_entry( surface,
                                   skipped_status,
                                   resolved_steps.error().message );
            }

            const std::vector<std::string> launch_args{
                std::string{ test_data_arg },
                std::string{ no_splash_arg },
            };
            const std::vector<std::pair<std::string, std::string>> environment;
            auto launched = launch_app( apprun,
                                        std::span<const std::string>{ launch_args },
                                        environment );
            if( !launched.has_value() )
            {
                const std::string notes =
                    "live capture error: " + launched.error().message;
                return make_entry( surface, skipped_status, notes );
            }

            AppProcess app{ *launched };
            std::this_thread::sleep_for( app_settle_delay );
            const auto window = wait_for_window( conn, app );
            if( !window.found )
            {
                return make_entry( surface,
                                   skipped_status,
                                   window.exited ? "app exited before a window appeared"
                                                 : "could not locate the app window" );
            }

            const auto output_path = out_dir / surface.output;
            auto       captured    = drive_and_capture( conn,
                                                        keymap,
                                                        window.window,
                                                        *resolved_steps,
                                                        output_path );
            if( !captured.has_value() )
            {
                return make_entry( surface, skipped_status, captured.error().message );
            }

            return make_entry( surface, ok_status, "" );
        }

    }    // namespace

    std::vector<Entry>
    capture_live( std::string_view root,
                  std::string_view apprun,
                  std::string_view out_dir )
    {
        if( !display_available() )
        {
            return environment_skip_entries( "no X display (DISPLAY unset)" );
        }
        if( !is_executable_file( std::filesystem::path{ std::string{ apprun } } ) )
        {
            return environment_skip_entries( "AppRun not found at " +
                                             std::string{ apprun } );
        }

        auto conn = x11::XcbConnection::open( "" );
        if( !conn.has_value() )
        {
            return environment_skip_entries( conn.error().message );
        }
        auto keymap = x11::XkbKeymap::from_connection( *conn );
        if( !keymap.has_value() )
        {
            return environment_skip_entries( keymap.error().message );
        }

        std::vector<Entry> entries;
        const auto&        surfaces = all_surfaces();
        entries.reserve( surfaces.size() );
        const std::filesystem::path output_root{ std::string{ out_dir } };
        for( const Surface& surface : surfaces )
        {
            entries.push_back(
                capture_surface( surface, *conn, *keymap, root, output_root, apprun )
            );
        }
        return entries;
    }

}    // namespace grab::inventory
