#include "cli/capture_command.hpp"
#include "codec/png/png_encoder.hpp"
#include "grab/image.hpp"
#include "grab/result.hpp"
#include "grab/window.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_screen_provider.hpp"
#include "platform/x11/xcb_window.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::cli
{

    namespace
    {

        constexpr int              success_exit_code = 0;
        constexpr int              error_exit_code   = 1;
        constexpr std::string_view display_flag      = "--display";
        constexpr std::string_view geometry_flag     = "--geometry";
        constexpr std::string_view window_flag       = "--window";
        constexpr std::string_view out_flag          = "--out";
        constexpr std::string_view no_cursor_flag    = "--no-cursor";

        struct CaptureOptions
        {
                std::string           display;
                Geometry              geometry;
                WindowMatch           window_match;
                std::filesystem::path output;
                bool                  has_geometry = false;
                bool                  has_window   = false;
                bool                  has_output   = false;
                bool                  draw_cursor  = true;
        };

        [[nodiscard]]
        std::int16_t
        clamp_i16( std::int32_t value ) noexcept
        {
            constexpr auto int16_min =
                static_cast<std::int32_t>( std::numeric_limits<std::int16_t>::min() );
            constexpr auto int16_max =
                static_cast<std::int32_t>( std::numeric_limits<std::int16_t>::max() );
            return static_cast<std::int16_t>(
                std::clamp( value, int16_min, int16_max )
            );
        }

        [[nodiscard]]
        std::uint16_t
        clamp_u16( std::uint32_t value ) noexcept
        {
            constexpr auto u_int16_max =
                static_cast<std::uint32_t>( std::numeric_limits<std::uint16_t>::max() );
            return static_cast<std::uint16_t>( std::min( value, u_int16_max ) );
        }

        [[nodiscard]]
        Geometry
        geometry_from_window_rect( const WindowRect& rect ) noexcept
        {
            return Geometry{
                .width  = clamp_u16( rect.width ),
                .height = clamp_u16( rect.height ),
                .x      = clamp_i16( rect.x ),
                .y      = clamp_i16( rect.y ),
            };
        }

        [[nodiscard]]
        grab::Result<CaptureOptions>
        parse_capture_args( std::span<char* const> args )
        {
            CaptureOptions options;
            auto           current = args.begin();

            while( current != args.end() )
            {
                if( *current == nullptr )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "argument is null" );
                }

                const std::string_view flag{ *current };
                if( flag == no_cursor_flag )
                {
                    options.draw_cursor = false;
                    current             = std::next( current );
                    continue;
                }

                const auto value_position = std::next( current );
                if( value_position == args.end() )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       std::string{ flag } + " requires a value" );
                }
                if( *value_position == nullptr )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       std::string{ flag } + " value is null" );
                }
                const std::string_view value{ *value_position };

                if( flag == display_flag )
                {
                    options.display = value;
                }
                else if( flag == geometry_flag )
                {
                    auto geometry = parse_geometry( value );
                    if( !geometry.has_value() )
                    {
                        return grab::fail( geometry.error().code,
                                           geometry.error().message );
                    }
                    options.geometry     = *geometry;
                    options.has_geometry = true;
                }
                else if( flag == window_flag )
                {
                    options.window_match = WindowMatch{ .app = std::string{ value } };
                    options.has_window   = true;
                }
                else if( flag == out_flag )
                {
                    options.output     = std::filesystem::path{ std::string{ value } };
                    options.has_output = true;
                }
                else
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "unknown capture argument: " +
                                           std::string{ flag } );
                }

                current = std::next( value_position );
            }

            if( options.has_geometry == options.has_window )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "exactly one of --geometry or --window is required" );
            }
            if( !options.has_output )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--out is required" );
            }
            return options;
        }

        [[nodiscard]]
        grab::Result<void>
        write_binary_file( const std::filesystem::path&     path,
                           const std::vector<std::uint8_t>& bytes )
        {
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
        grab::Result<Geometry>
        resolve_capture_geometry( const grab::platform::x11::XcbConnection& conn,
                                  const CaptureOptions&                     options )
        {
            if( options.has_geometry )
            {
                return options.geometry;
            }

            auto window = grab::platform::x11::find_window( conn, options.window_match );
            if( !window.has_value() )
            {
                return grab::fail( window.error().code, window.error().message );
            }

            auto rect = grab::platform::x11::window_geometry( conn, *window );
            if( !rect.has_value() )
            {
                return grab::fail( rect.error().code, rect.error().message );
            }
            return geometry_from_window_rect( *rect );
        }

        [[nodiscard]]
        grab::Result<void>
        capture_to_file( const CaptureOptions& options )
        {
            auto connection =
                grab::platform::x11::XcbConnection::open( options.display );
            if( !connection.has_value() )
            {
                return grab::fail( connection.error().code, connection.error().message );
            }

            auto geometry = resolve_capture_geometry( *connection, options );
            if( !geometry.has_value() )
            {
                return grab::fail( geometry.error().code, geometry.error().message );
            }

            auto image = grab::platform::x11::capture_region( *connection,
                                                              geometry->x,
                                                              geometry->y,
                                                              geometry->width,
                                                              geometry->height,
                                                              options.draw_cursor );
            if( !image.has_value() )
            {
                return grab::fail( image.error().code, image.error().message );
            }

            auto encoded = grab::codec::encode_png( image->view() );
            if( !encoded.has_value() )
            {
                return grab::fail( encoded.error().code, encoded.error().message );
            }

            return write_binary_file( options.output, *encoded );
        }

        void
        print_error( std::string_view message )
        {
            ( void )std::fputs( "grab: ", stderr );
            ( void )std::fputs( std::string{ message }.c_str(), stderr );
            ( void )std::fputc( '\n', stderr );
        }

    }    // namespace

    int
    run_capture_command( std::span<char* const> args )
    {
        auto options = parse_capture_args( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return error_exit_code;
        }

        auto capture = capture_to_file( *options );
        if( !capture.has_value() )
        {
            print_error( capture.error().message );
            return error_exit_code;
        }
        return success_exit_code;
    }

}    // namespace grab::cli
