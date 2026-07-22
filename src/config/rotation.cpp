#include "config/pattern.hpp"
#include "config/rotation.hpp"
#include "grab/config.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace grab::config
{
    namespace
    {

        constexpr std::uintmax_t bytesPerKibibyte  = 1'024U;
        constexpr std::uintmax_t bytesPerMebibyte  = bytesPerKibibyte * bytesPerKibibyte;
        constexpr std::uintmax_t unlimitedLimit    = 0U;
        constexpr std::uintmax_t oneUnit           = 1U;
        constexpr std::uintmax_t resumeNumerator   = 9U;
        constexpr std::uintmax_t resumeDenominator = 10U;
        constexpr std::size_t    stringBeginning   = 0U;
        constexpr char           tokenOpen         = '{';
        constexpr std::string_view errorPrefix     = "rotation ";
        constexpr std::string_view pathSeparator   = ": ";
        constexpr std::string_view parentComponent = "..";
        constexpr std::string_view currentComponent = ".";
        constexpr std::string_view outsidePathMessage =
            "rotation adopt path is outside the output directory: ";

        struct ScanScope
        {
                std::filesystem::path root;
                bool                  recursive{};
        };

        [[nodiscard]]
        std::unexpected<grab::Error>
        filesystem_failure( std::string_view             operation,
                            const std::filesystem::path& path,
                            const std::error_code&       error )
        {
            return grab::fail( grab::ErrorCode::InternalFault,
                               std::string{ errorPrefix } +
                                   std::string{ operation } +
                                   std::string{ pathSeparator } +
                                   path.string() +
                                   std::string{ pathSeparator } +
                                   error.message() );
        }

        [[nodiscard]]
        bool
        is_missing( const std::error_code& error ) noexcept
        {
            return error == std::errc::no_such_file_or_directory;
        }

        [[nodiscard]]
        std::uintmax_t
        disk_cap_bytes( const WatchLimits& limits ) noexcept
        {
            if( limits.max_disk_mib == unlimitedLimit )
            {
                return unlimitedLimit;
            }

            constexpr auto maximum = std::numeric_limits<std::uintmax_t>::max();
            const auto mebibytes   = static_cast<std::uintmax_t>( limits.max_disk_mib );
            if( mebibytes > maximum / bytesPerMebibyte )
            {
                return maximum;
            }
            return mebibytes * bytesPerMebibyte;
        }

        [[nodiscard]]
        std::uintmax_t
        resume_threshold( std::uintmax_t cap ) noexcept
        {
            const std::uintmax_t whole = ( cap / resumeDenominator ) * resumeNumerator;
            const std::uintmax_t remainder =
                ( cap % resumeDenominator ) * resumeNumerator;
            const std::uintmax_t rounded_remainder =
                ( remainder + ( resumeDenominator - oneUnit ) ) / resumeDenominator;
            return whole + rounded_remainder;
        }

        [[nodiscard]]
        std::optional<std::filesystem::path>
        relative_descendant( const std::filesystem::path& directory,
                             const std::filesystem::path& path )
        {
            std::filesystem::path relative = path.lexically_relative( directory );
            if( relative.empty() ||
                relative.is_absolute() ||
                relative.generic_string() == currentComponent )
            {
                return std::nullopt;
            }
            for( const std::filesystem::path& component : relative )
            {
                if( component.generic_string() == parentComponent )
                {
                    return std::nullopt;
                }
            }
            return relative;
        }

        [[nodiscard]]
        grab::Result<bool>
        safe_directory_chain( const std::filesystem::path& output_directory,
                              const std::filesystem::path& candidate_directory )
        {
            if( candidate_directory == output_directory )
            {
                return true;
            }
            const auto relative =
                relative_descendant( output_directory, candidate_directory );
            if( !relative.has_value() )
            {
                return false;
            }

            std::filesystem::path ancestor = output_directory;
            for( const std::filesystem::path& component : *relative )
            {
                ancestor /= component;
                std::error_code error;
                const auto status = std::filesystem::symlink_status( ancestor, error );
                if( error )
                {
                    if( is_missing( error ) )
                    {
                        return false;
                    }
                    return filesystem_failure( "inspect ancestor", ancestor, error );
                }
                if( !std::filesystem::is_directory( status ) )
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]]
        ScanScope
        scan_scope( const std::filesystem::path& dir,
                    std::string_view             pattern )
        {
            const std::filesystem::path pattern_parent =
                std::filesystem::path{ pattern }.parent_path();
            const std::string parent_text = pattern_parent.generic_string();
            const std::size_t token       = parent_text.find( tokenOpen );
            if( token == std::string::npos )
            {
                std::filesystem::path root = ( dir / pattern_parent ).lexically_normal();
                if( root != dir && !relative_descendant( dir, root ).has_value() )
                {
                    root = dir;
                }
                return ScanScope{
                    .root      = std::move( root ),
                    .recursive = false,
                };
            }

            const std::filesystem::path literal_prefix =
                parent_text.substr( stringBeginning, token );
            std::filesystem::path root =
                ( dir / literal_prefix.parent_path() ).lexically_normal();
            if( root != dir && !relative_descendant( dir, root ).has_value() )
            {
                root = dir;
            }
            return ScanScope{
                .root      = std::move( root ),
                .recursive = true,
            };
        }

        [[nodiscard]]
        std::filesystem::file_time_type
        age_cutoff( std::chrono::system_clock::time_point now,
                    std::uint32_t                         max_age_days ) noexcept
        {
            using FileTime          = std::filesystem::file_time_type;
            using FileDuration      = FileTime::duration;
            using FloatingSeconds   = std::chrono::duration<double>;

            const FileTime file_now = FileTime::clock::from_sys( now );
            const auto     age =
                std::chrono::days{ static_cast<std::chrono::days::rep>( max_age_days ) };
            const FloatingSeconds available =
                FloatingSeconds{ file_now.time_since_epoch() } -
                FloatingSeconds{ FileTime::min().time_since_epoch() };
            if( FloatingSeconds{ age } >= available )
            {
                return FileTime::min();
            }
            return file_now - std::chrono::duration_cast<FileDuration>( age );
        }

    }    // namespace

    RotationLedger::RotationLedger( std::filesystem::path dir,
                                    std::string           pattern,
                                    WatchLimits           limits ) :
        dir_( std::move( dir ).lexically_normal() ),
        pattern_( std::move( pattern ) ),
        limits_( limits )
    {
    }

    grab::Result<std::optional<RotationLedger::LedgerFile>>
    RotationLedger::inspect_regular_file( const std::filesystem::path& file ) const
    {
        const auto relative = relative_descendant( dir_, file );
        if( !relative.has_value() )
        {
            return std::optional<LedgerFile>{};
        }

        auto safe_parent = safe_directory_chain( dir_, file.parent_path() );
        if( !safe_parent.has_value() )
        {
            return std::unexpected( std::move( safe_parent.error() ) );
        }
        if( !*safe_parent )
        {
            return std::optional<LedgerFile>{};
        }

        std::error_code error;
        const auto      status = std::filesystem::symlink_status( file, error );
        if( error )
        {
            if( is_missing( error ) )
            {
                return std::optional<LedgerFile>{};
            }
            return filesystem_failure( "inspect", file, error );
        }
        if( !std::filesystem::is_regular_file( status ) )
        {
            return std::optional<LedgerFile>{};
        }

        const std::uintmax_t size = std::filesystem::file_size( file, error );
        if( error )
        {
            if( is_missing( error ) )
            {
                return std::optional<LedgerFile>{};
            }
            return filesystem_failure( "stat size", file, error );
        }
        const auto modified_at = std::filesystem::last_write_time( file, error );
        if( error )
        {
            if( is_missing( error ) )
            {
                return std::optional<LedgerFile>{};
            }
            return filesystem_failure( "stat mtime", file, error );
        }

        const auto final_status = std::filesystem::symlink_status( file, error );
        if( error )
        {
            if( is_missing( error ) )
            {
                return std::optional<LedgerFile>{};
            }
            return filesystem_failure( "recheck", file, error );
        }
        if( !std::filesystem::is_regular_file( final_status ) )
        {
            return std::optional<LedgerFile>{};
        }

        return LedgerFile{
            .path        = file,
            .modified_at = modified_at,
            .size        = size,
        };
    }

    grab::Result<void>
    RotationLedger::scan()
    {
        std::vector<LedgerFile> adopted;
        std::error_code         error;
        const ScanScope         scope     = scan_scope( dir_, pattern_ );
        auto                    safe_root = safe_directory_chain( dir_, scope.root );
        if( !safe_root.has_value() )
        {
            return std::unexpected( std::move( safe_root.error() ) );
        }
        if( !*safe_root )
        {
            files_.clear();
            recalculate_total();
            update_pause_state();
            return {};
        }
        const auto root_status = std::filesystem::symlink_status( scope.root, error );
        if( error )
        {
            if( is_missing( error ) )
            {
                files_.clear();
                recalculate_total();
                update_pause_state();
                return {};
            }
            return filesystem_failure( "scan", scope.root, error );
        }
        if( !std::filesystem::is_directory( root_status ) )
        {
            files_.clear();
            recalculate_total();
            update_pause_state();
            return {};
        }

        constexpr auto options =
            std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator iterator{
            scope.root,
            options,
            error,
        };
        if( error )
        {
            return filesystem_failure( "scan", scope.root, error );
        }

        const std::filesystem::recursive_directory_iterator end;
        while( iterator != end )
        {
            const std::filesystem::path file = iterator->path().lexically_normal();
            if( !scope.recursive )
            {
                iterator.disable_recursion_pending();
            }

            const auto relative = relative_descendant( dir_, file );
            if( relative.has_value() &&
                matches_pattern( pattern_, relative->generic_string() ) )
            {
                auto inspected = inspect_regular_file( file );
                if( !inspected.has_value() )
                {
                    return std::unexpected( std::move( inspected.error() ) );
                }
                if( inspected->has_value() )
                {
                    adopted.push_back( std::move( **inspected ) );
                }
            }

            iterator.increment( error );
            if( error )
            {
                return filesystem_failure( "scan", scope.root, error );
            }
        }

        files_ = std::move( adopted );
        recalculate_total();
        update_pause_state();
        return {};
    }

    grab::Result<void>
    RotationLedger::reconcile()
    {
        std::vector<LedgerFile> refreshed;
        refreshed.reserve( files_.size() );
        for( const LedgerFile& file : files_ )
        {
            const auto relative = relative_descendant( dir_, file.path );
            if( !relative.has_value() ||
                !matches_pattern( pattern_, relative->generic_string() ) )
            {
                continue;
            }

            auto inspected = inspect_regular_file( file.path );
            if( !inspected.has_value() )
            {
                return std::unexpected( std::move( inspected.error() ) );
            }
            if( inspected->has_value() )
            {
                refreshed.push_back( std::move( **inspected ) );
            }
        }

        files_ = std::move( refreshed );
        recalculate_total();
        update_pause_state();
        return {};
    }

    grab::Result<void>
    RotationLedger::adopt( const std::filesystem::path& file )
    {
        std::filesystem::path candidate = file.lexically_normal();
        if( candidate.is_relative() &&
            !relative_descendant( dir_, candidate ).has_value() )
        {
            candidate = ( dir_ / candidate ).lexically_normal();
        }
        const auto relative = relative_descendant( dir_, candidate );
        if( !relative.has_value() )
        {
            return grab::fail( grab::ErrorCode::InvalidArgument,
                               std::string{ outsidePathMessage } + candidate.string() );
        }
        if( !matches_pattern( pattern_, relative->generic_string() ) )
        {
            return {};
        }

        auto reconciled = reconcile();
        if( !reconciled.has_value() )
        {
            return std::unexpected( std::move( reconciled.error() ) );
        }

        auto inspected = inspect_regular_file( candidate );
        if( !inspected.has_value() )
        {
            return std::unexpected( std::move( inspected.error() ) );
        }
        if( !inspected->has_value() )
        {
            return {};
        }

        const auto existing =
            std::ranges::find_if( files_,
                                  [&candidate]( const LedgerFile& entry )
                                  {
                                      return entry.path == candidate;
                                  } );
        if( existing == files_.end() )
        {
            files_.push_back( std::move( **inspected ) );
        }
        else
        {
            *existing = std::move( **inspected );
        }
        recalculate_total();

        auto enforced = enforce_max_files();
        if( !enforced.has_value() )
        {
            return std::unexpected( std::move( enforced.error() ) );
        }
        update_pause_state();
        return {};
    }

    grab::Result<RotationLedger::RemoveState>
    RotationLedger::remove_file( std::size_t index )
    {
        const LedgerFile recorded  = files_.at( index );
        auto             inspected = inspect_regular_file( recorded.path );
        if( !inspected.has_value() )
        {
            return std::unexpected( std::move( inspected.error() ) );
        }
        if( !inspected->has_value() )
        {
            files_.erase( files_.begin() + static_cast<std::ptrdiff_t>( index ) );
            recalculate_total();
            update_pause_state();
            return RemoveState::Dropped;
        }

        const LedgerFile& current = **inspected;
        if( current.modified_at !=
            recorded.modified_at ||
            current.size != recorded.size )
        {
            files_.at( index ) = std::move( **inspected );
            recalculate_total();
            update_pause_state();
            return RemoveState::Refreshed;
        }

        std::error_code error;
        const bool      removed = std::filesystem::remove( recorded.path, error );
        if( error )
        {
            return filesystem_failure( "remove", recorded.path, error );
        }

        files_.erase( files_.begin() + static_cast<std::ptrdiff_t>( index ) );
        recalculate_total();
        update_pause_state();
        return removed ? RemoveState::Removed : RemoveState::Dropped;
    }

    grab::Result<void>
    RotationLedger::enforce_max_files()
    {
        if( limits_.max_files == unlimitedLimit )
        {
            return {};
        }

        const auto maximum_files = static_cast<std::uintmax_t>( limits_.max_files );
        while( static_cast<std::uintmax_t>( files_.size() ) > maximum_files )
        {
            const auto oldest = std::ranges::min_element(
                files_,
                []( const LedgerFile& left, const LedgerFile& right )
                {
                    if( left.modified_at != right.modified_at )
                    {
                        return left.modified_at < right.modified_at;
                    }
                    return left.path.native() < right.path.native();
                }
            );
            const auto index =
                static_cast<std::size_t>( std::distance( files_.begin(), oldest ) );
            auto removal = remove_file( index );
            if( !removal.has_value() )
            {
                return std::unexpected( std::move( removal.error() ) );
            }
        }
        return {};
    }

    grab::Result<std::size_t>
    RotationLedger::prune_age( std::chrono::system_clock::time_point now )
    {
        auto reconciled = reconcile();
        if( !reconciled.has_value() )
        {
            return std::unexpected( std::move( reconciled.error() ) );
        }
        if( limits_.max_age_days == unlimitedLimit )
        {
            return std::size_t{};
        }

        const auto  cutoff = age_cutoff( now, limits_.max_age_days );

        std::size_t removed_count{};
        std::size_t index{};
        while( index < files_.size() )
        {
            if( files_.at( index ).modified_at >= cutoff )
            {
                ++index;
                continue;
            }

            auto removal = remove_file( index );
            if( !removal.has_value() )
            {
                return std::unexpected( std::move( removal.error() ) );
            }
            if( *removal == RemoveState::Removed )
            {
                ++removed_count;
            }
            else if( *removal == RemoveState::Refreshed )
            {
                ++index;
            }
        }
        update_pause_state();
        return removed_count;
    }

    bool
    RotationLedger::paused() const noexcept
    {
        return paused_;
    }

    grab::Result<void>
    RotationLedger::refresh_disk()
    {
        return reconcile();
    }

    void
    RotationLedger::recalculate_total() noexcept
    {
        constexpr auto maximum = std::numeric_limits<std::uintmax_t>::max();
        total_bytes_           = 0U;
        for( const LedgerFile& file : files_ )
        {
            if( file.size > maximum - total_bytes_ )
            {
                total_bytes_ = maximum;
                return;
            }
            total_bytes_ += file.size;
        }
    }

    void
    RotationLedger::update_pause_state() noexcept
    {
        const std::uintmax_t cap = disk_cap_bytes( limits_ );
        if( cap == unlimitedLimit )
        {
            paused_ = false;
            return;
        }
        if( !paused_ )
        {
            paused_ = total_bytes_ >= cap;
            return;
        }
        paused_ = total_bytes_ >= resume_threshold( cap );
    }

}    // namespace grab::config
