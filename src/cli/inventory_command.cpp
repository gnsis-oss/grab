#include "cli/inventory_command.hpp"
#include "grab/result.hpp"
#include "inventory/appimage.hpp"
#include "inventory/manifest.hpp"
#include "inventory/run.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace grab::cli
{

    namespace
    {

        constexpr int              success_exit_code = 0;
        constexpr int              error_exit_code   = 1;
        constexpr std::string_view root_flag         = "--root";
        constexpr std::string_view out_flag          = "--out";
        constexpr std::string_view app_image_flag    = "--appimage";
        constexpr std::string_view cache_dir_flag    = "--cache-dir";
        constexpr std::string_view default_root      = ".";
        constexpr std::string_view app_image_cache   = ".appimage";
        constexpr std::string_view manifest_name     = "manifest.json";
        constexpr std::string_view ok_status         = "ok";

        struct InventoryOptions
        {
                std::string root = std::string{ default_root };
                std::string out;
                std::string appimage;
                std::string cache_dir;
                bool        has_out = false;
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

        void
        print_error( std::string_view message )
        {
            ( void )std::fputs( "grab: ", stderr );
            ( void )std::fputs( std::string{ message }.c_str(), stderr );
            ( void )std::fputc( '\n', stderr );
        }

        [[nodiscard]]
        grab::Result<std::filesystem::path>
        absolute_path( std::string_view path )
        {
            std::error_code error;
            auto            absolute =
                std::filesystem::absolute( std::filesystem::path{ std::string{ path } },
                                           error );
            if( error )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   filesystem_error_message( "absolute path", error ) );
            }
            return absolute;
        }

        [[nodiscard]]
        grab::Result<InventoryOptions>
        parse_inventory_args( std::span<char* const> args )
        {
            InventoryOptions options;
            auto             current = args.begin();
            while( current != args.end() )
            {
                if( *current == nullptr )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "argument is null" );
                }
                const std::string_view flag{ *current };
                const auto             value_position = std::next( current );
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

                if( flag == root_flag )
                {
                    options.root = value;
                }
                else if( flag == out_flag )
                {
                    options.out     = value;
                    options.has_out = true;
                }
                else if( flag == app_image_flag )
                {
                    options.appimage = value;
                }
                else if( flag == cache_dir_flag )
                {
                    options.cache_dir = value;
                }
                else
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       "unknown inventory argument: " +
                                           std::string{ flag } );
                }
                current = std::next( value_position );
            }

            if( !options.has_out )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   "--out is required" );
            }
            return options;
        }

        [[nodiscard]]
        grab::Result<void>
        run_inventory( const InventoryOptions& options )
        {
            auto root = absolute_path( options.root );
            if( !root.has_value() )
            {
                return grab::fail( root.error().code, root.error().message );
            }
            auto out = absolute_path( options.out );
            if( !out.has_value() )
            {
                return grab::fail( out.error().code, out.error().message );
            }

            std::error_code error;
            std::filesystem::create_directories( *out, error );
            if( error )
            {
                return grab::fail( grab::ErrorCode::internal_fault,
                                   filesystem_error_message( "create output directory",
                                                             error ) );
            }

            const std::filesystem::path cache_dir =
                options.cache_dir.empty() ? *out / std::string{ app_image_cache }
                                          : std::filesystem::path{ options.cache_dir };
            auto appimage =
                grab::inventory::find_appimage( root->string(), options.appimage );
            if( !appimage.has_value() )
            {
                return grab::fail( appimage.error().code, appimage.error().message );
            }
            auto apprun =
                grab::inventory::extract_appimage( *appimage, cache_dir.string() );
            if( !apprun.has_value() )
            {
                return grab::fail( apprun.error().code, apprun.error().message );
            }

            std::vector<grab::inventory::Entry> entries =
                grab::inventory::capture_live( root->string(), *apprun, out->string() );
            const auto ok_count = static_cast<std::size_t>(
                std::ranges::count_if( entries,
                                       []( const grab::inventory::Entry& entry )
                                       {
                                           return entry.status == ok_status;
                                       } )
            );
            const auto manifest_path = *out / std::string{ manifest_name };
            auto       manifest =
                grab::inventory::write_manifest( manifest_path.string(), entries );
            if( !manifest.has_value() )
            {
                return grab::fail( manifest.error().code, manifest.error().message );
            }

            std::string message  = "grab inventory: ";
            message             += std::to_string( entries.size() );
            message             += " surfaces, ";
            message             += std::to_string( ok_count );
            message             += " ok, manifest at ";
            message             += manifest_path.string();
            message             += '\n';
            ( void )std::fputs( message.c_str(), stdout );
            return {};
        }

    }    // namespace

    int
    run_inventory_command( std::span<char* const> args )
    {
        auto options = parse_inventory_args( args );
        if( !options.has_value() )
        {
            print_error( options.error().message );
            return error_exit_code;
        }

        auto result = run_inventory( *options );
        if( !result.has_value() )
        {
            print_error( result.error().message );
            return error_exit_code;
        }
        return success_exit_code;
    }

}    // namespace grab::cli
