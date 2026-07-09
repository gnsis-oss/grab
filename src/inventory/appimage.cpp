#include "grab/result.hpp"
#include "inventory/appimage.hpp"

// NOLINTBEGIN(llvm-include-order)
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <vector>
// NOLINTEND(llvm-include-order)

namespace grab::inventory
{

    namespace
    {

        constexpr int              posix_failure            = -1;
        constexpr int              child_exec_failure_exit  = 127;
        constexpr std::string_view app_run_name             = "AppRun";
        constexpr std::string_view app_image_extract_arg    = "--appimage-extract";
        constexpr std::string_view app_image_prefix         = "PlotJuggler-";
        constexpr std::string_view app_image_suffix         = ".AppImage";
        constexpr std::string_view app_image_directory      = "appimage";
        constexpr std::string_view build_directory          = "build";
        constexpr std::string_view null_device_path         = "/dev/null";
        constexpr std::string_view qt_im_module_environment = "QT_IM_MODULE=";
        constexpr std::string_view app_image_environment    = "PJ_UI_INVENTORY_APPIMAGE";

        [[nodiscard]]
        std::string
        posix_error_message( std::string_view operation,
                             int              error_number )
        {
            std::string message{ operation };
            message += ": ";
            message +=
                std::error_code{ error_number, std::generic_category() }.message();
            return message;
        }

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
        std::filesystem::path
        apprun_path( const std::filesystem::path& cache_dir )
        {
            return cache_dir / "squashfs-root" / std::string{ app_run_name };
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
        std::optional<std::string_view>
        read_environment( std::string_view name )
        {
            std::string prefix{ name };
            prefix += '=';
            for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
                 entry              = std::next( entry ) )
            {
                const std::string_view variable{ *entry };
                if( variable.starts_with( prefix ) )
                {
                    return variable.substr( prefix.size() );
                }
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::vector<char*>
        environment_without_qt_im_module()
        {
            std::vector<char*> filtered;
            for( char* const* entry = ::environ; entry != nullptr && *entry != nullptr;
                 entry              = std::next( entry ) )
            {
                const std::string_view variable{ *entry };
                if( !variable.starts_with( qt_im_module_environment ) )
                {
                    filtered.push_back( *entry );
                }
            }
            filtered.push_back( nullptr );
            return filtered;
        }

        [[nodiscard]]
        std::vector<char*>
        make_string_pointers( std::vector<std::string>& storage )
        {
            std::vector<char*> pointers;
            pointers.reserve( storage.size() + 1U );
            for( std::string& value : storage )
            {
                pointers.push_back( value.data() );
            }
            pointers.push_back( nullptr );
            return pointers;
        }

        [[nodiscard]]
        grab::Result<int>
        wait_for_child( pid_t pid )
        {
            int status = 0;
            while( true )
            {
                const pid_t result = ::waitpid( pid, &status, 0 );
                if( result == pid )
                {
                    return status;
                }
                if( result == posix_failure && errno == EINTR )
                {
                    continue;
                }
                return grab::fail( grab::ErrorCode::provider_failed,
                                   posix_error_message( "waitpid", errno ) );
            }
        }

        [[nodiscard]]
        grab::Result<void>
        run_extract_command( const std::filesystem::path& appimage,
                             const std::filesystem::path& cache_dir )
        {
            std::vector<std::string> argv_storage{
                appimage.string(),
                std::string{ app_image_extract_arg },
            };
            std::vector<char*> argv = make_string_pointers( argv_storage );
            std::vector<char*> envp = environment_without_qt_im_module();

            const std::string  null_device{ null_device_path };
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            const int null_fd = ::open( null_device.c_str(), O_WRONLY | O_CLOEXEC );
            if( null_fd == posix_failure )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   posix_error_message( "open /dev/null", errno ) );
            }

            const pid_t pid = ::fork();
            if( pid == posix_failure )
            {
                ( void )::close( null_fd );
                return grab::fail( grab::ErrorCode::provider_failed,
                                   posix_error_message( "fork", errno ) );
            }

            if( pid == 0 )
            {
                ( void )::dup2( null_fd, STDOUT_FILENO );
                ( void )::dup2( null_fd, STDERR_FILENO );
                ( void )::close( null_fd );
                const std::string cwd = cache_dir.string();
                if( ::chdir( cwd.c_str() ) == posix_failure )
                {
                    ::_exit( child_exec_failure_exit );
                }
                ( void )::execve( argv_storage.front().c_str(),
                                  argv.data(),
                                  envp.data() );
                ::_exit( child_exec_failure_exit );
            }

            ( void )::close( null_fd );
            auto status = wait_for_child( pid );
            if( !status.has_value() )
            {
                return grab::fail( status.error().code, status.error().message );
            }
            // NOLINTNEXTLINE(misc-include-cleaner)
            if( !WIFEXITED( *status ) || WEXITSTATUS( *status ) != 0 )
            {
                return grab::fail( grab::ErrorCode::provider_failed,
                                   "AppImage extraction failed" );
            }
            return {};
        }

        [[nodiscard]]
        bool
        filename_matches_appimage( const std::filesystem::path& path )
        {
            const std::string filename = path.filename().string();
            return filename.starts_with( app_image_prefix ) &&
                   filename.ends_with( app_image_suffix );
        }

        void
        collect_directory_appimages( const std::filesystem::path&        directory,
                                     std::vector<std::filesystem::path>& matches )
        {
            std::error_code                           error;
            std::filesystem::directory_iterator       iter{ directory, error };
            const std::filesystem::directory_iterator end;
            while( !error && iter != end )
            {
                const auto& entry = *iter;
                if( filename_matches_appimage( entry.path() ) &&
                    entry.is_regular_file( error ) )
                {
                    matches.push_back( entry.path() );
                }
                error.clear();
                iter.increment( error );
            }
        }

        void
        collect_recursive_appimages( const std::filesystem::path&        directory,
                                     std::vector<std::filesystem::path>& matches )
        {
            std::error_code                               error;
            std::filesystem::recursive_directory_iterator iter{
                directory,
                std::filesystem::directory_options::skip_permission_denied,
                error,
            };
            const std::filesystem::recursive_directory_iterator end;
            while( !error && iter != end )
            {
                const auto& entry = *iter;
                if( filename_matches_appimage( entry.path() ) &&
                    entry.is_regular_file( error ) )
                {
                    matches.push_back( entry.path() );
                }
                error.clear();
                iter.increment( error );
            }
        }

        [[nodiscard]]
        grab::Result<std::filesystem::path>
        newest_match( const std::vector<std::filesystem::path>& matches,
                      const std::filesystem::path&              root )
        {
            std::filesystem::path           best;
            std::filesystem::file_time_type best_time{};
            bool                            found = false;

            for( const auto& path : matches )
            {
                std::error_code error;
                const auto      time = std::filesystem::last_write_time( path, error );
                if( error )
                {
                    continue;
                }
                if( !found || time > best_time )
                {
                    best      = path;
                    best_time = time;
                    found     = true;
                }
            }

            if( found )
            {
                std::error_code error;
                auto            absolute = std::filesystem::absolute( best, error );
                if( error )
                {
                    return grab::fail( grab::ErrorCode::invalid_argument,
                                       filesystem_error_message( "absolute path",
                                                                 error ) );
                }
                return absolute;
            }

            const auto appimage_pattern = ( root /
                                            std::string{ app_image_directory } /
                                            ( std::string{ app_image_prefix } +
                                              "*" +
                                              std::string{ app_image_suffix } ) )
                                              .string();
            const auto build_pattern    = ( root /
                                            std::string{ build_directory } /
                                            "**" /
                                            ( std::string{ app_image_prefix } +
                                              "*" +
                                              std::string{ app_image_suffix } ) )
                                              .string();
            const auto root_pattern     = ( root / ( std::string{ app_image_prefix } +
                                                     "*" +
                                                     std::string{ app_image_suffix } ) )
                                              .string();

            return grab::fail( grab::ErrorCode::invalid_argument,
                               "no PlotJuggler AppImage found. Searched:\n  " +
                                   appimage_pattern +
                                   "\n  " +
                                   build_pattern +
                                   "\n  " +
                                   root_pattern +
                                   "\nOverride with the --appimage argument or the " +
                                   std::string{ app_image_environment } +
                                   " env var." );
        }

    }    // namespace

    grab::Result<std::string>
    extract_appimage( std::string_view appimage_path,
                      std::string_view cache_dir )
    {
        auto cache = absolute_path( cache_dir );
        if( !cache.has_value() )
        {
            return grab::fail( cache.error().code, cache.error().message );
        }

        const auto app_run = apprun_path( *cache );
        if( is_executable_file( app_run ) )
        {
            return app_run.string();
        }

        auto appimage = absolute_path( appimage_path );
        if( !appimage.has_value() )
        {
            return grab::fail( appimage.error().code, appimage.error().message );
        }
        if( !is_executable_file( *appimage ) )
        {
            return grab::fail( grab::ErrorCode::invalid_argument,
                               "AppImage not found at " + appimage->string() );
        }

        std::error_code error;
        std::filesystem::create_directories( *cache, error );
        if( error )
        {
            return grab::fail( grab::ErrorCode::internal_fault,
                               filesystem_error_message( "create AppImage cache",
                                                         error ) );
        }

        auto extracted = run_extract_command( *appimage, *cache );
        if( !extracted.has_value() )
        {
            return grab::fail( extracted.error().code, extracted.error().message );
        }
        if( !is_executable_file( app_run ) )
        {
            return grab::fail(
                grab::ErrorCode::provider_failed,
                "extraction did not produce an executable squashfs-root/AppRun"
            );
        }
        return app_run.string();
    }

    grab::Result<std::string>
    find_appimage( std::string_view root,
                   std::string_view override_path )
    {
        if( !override_path.empty() )
        {
            auto absolute = absolute_path( override_path );
            if( !absolute.has_value() )
            {
                return grab::fail( absolute.error().code, absolute.error().message );
            }
            return absolute->string();
        }

        const auto from_environment = read_environment( app_image_environment );
        if( from_environment.has_value() && !from_environment->empty() )
        {
            auto absolute = absolute_path( *from_environment );
            if( !absolute.has_value() )
            {
                return grab::fail( absolute.error().code, absolute.error().message );
            }
            return absolute->string();
        }

        auto root_path = absolute_path( root );
        if( !root_path.has_value() )
        {
            return grab::fail( root_path.error().code, root_path.error().message );
        }

        std::vector<std::filesystem::path> matches;
        collect_directory_appimages( *root_path / std::string{ app_image_directory },
                                     matches );
        collect_recursive_appimages( *root_path / std::string{ build_directory },
                                     matches );
        collect_directory_appimages( *root_path, matches );

        auto newest = newest_match( matches, *root_path );
        if( !newest.has_value() )
        {
            return grab::fail( newest.error().code, newest.error().message );
        }
        return newest->string();
    }

}    // namespace grab::inventory
