#include "config/batch_manifest.hpp"
#include "grab/result.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
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

        using Json                                   = nlohmann::json;
        using OrderedJson                            = nlohmann::ordered_json;

        constexpr std::string_view manifestFilename  = "manifest.json";
        constexpr std::string_view temporaryFilename = "manifest.json.tmp";

        constexpr std::string_view profileField      = "profile";
        constexpr std::string_view startedAtField    = "started_at";
        constexpr std::string_view endedAtField      = "ended_at";
        constexpr std::string_view stateField        = "state";
        constexpr std::string_view targetsField      = "targets";
        constexpr std::string_view compareField      = "compare";
        constexpr std::string_view nameField         = "name";
        constexpr std::string_view argvField         = "argv";
        constexpr std::string_view pidField          = "pid";
        constexpr std::string_view windowIdField     = "window_id";
        constexpr std::string_view filesField        = "files";
        constexpr std::string_view errorField        = "error";
        constexpr std::string_view scoreField        = "score";
        constexpr std::string_view passedField       = "passed";

        constexpr std::string_view runningState      = "running";
        constexpr std::string_view doneState         = "done";
        constexpr std::string_view failedState       = "failed";

        [[nodiscard]]
        std::string
        file_prefix( const std::filesystem::path& path )
        {
            return path.string() + ":";
        }

        [[nodiscard]]
        std::string
        field_pointer( std::string_view base,
                       std::string_view field )
        {
            return std::string{ base } + "/" + std::string{ field };
        }

        [[nodiscard]]
        std::string
        index_pointer( std::string_view base,
                       std::size_t      index )
        {
            return std::string{ base } + "/" + std::to_string( index );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        malformed_field( const std::filesystem::path& path,
                         std::string_view             pointer )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               file_prefix( path ) +
                                   std::string{ pointer } +
                                   ": missing or malformed manifest field" );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        read_error( const std::filesystem::path& path,
                    std::string                  reason )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               file_prefix( path ) + "/: " + std::move( reason ) );
        }

        [[nodiscard]]
        std::unexpected<grab::Error>
        write_error( const std::filesystem::path& path,
                     std::string                  reason )
        {
            return grab::fail( ErrorCode::InternalFault,
                               file_prefix( path ) + "/: " + std::move( reason ) );
        }

        [[nodiscard]]
        const Json*
        find_field( const Json&      object,
                    std::string_view field )
        {
            const auto member = object.find( std::string{ field } );
            if( member == object.end() )
            {
                return nullptr;
            }
            return &*member;
        }

        [[nodiscard]]
        std::optional<std::string>
        string_field( const Json&      object,
                      std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_string() )
            {
                return std::nullopt;
            }
            return value->get<std::string>();
        }

        [[nodiscard]]
        std::optional<std::vector<std::string>>
        string_array_field( const Json&      object,
                            std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_array() )
            {
                return std::nullopt;
            }

            std::vector<std::string> strings;
            strings.reserve( value->size() );
            for( const Json& item : *value )
            {
                if( !item.is_string() )
                {
                    return std::nullopt;
                }
                strings.emplace_back( item.get<std::string>() );
            }
            return strings;
        }

        [[nodiscard]]
        std::optional<std::int64_t>
        int64_field( const Json&      object,
                     std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_number_integer() )
            {
                return std::nullopt;
            }
            if( !value->is_number_unsigned() )
            {
                return value->get<std::int64_t>();
            }

            const auto     unsigned_value = value->get<std::uint64_t>();
            constexpr auto maxSigned =
                static_cast<std::uint64_t>( std::numeric_limits<std::int64_t>::max() );
            if( unsigned_value > maxSigned )
            {
                return std::nullopt;
            }
            return static_cast<std::int64_t>( unsigned_value );
        }

        [[nodiscard]]
        std::optional<std::uint32_t>
        uint32_field( const Json&      object,
                      std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_number_integer() )
            {
                return std::nullopt;
            }

            std::uint64_t unsigned_value{};
            if( value->is_number_unsigned() )
            {
                unsigned_value = value->get<std::uint64_t>();
            }
            else
            {
                const auto signed_value = value->get<std::int64_t>();
                if( signed_value < 0 )
                {
                    return std::nullopt;
                }
                unsigned_value = static_cast<std::uint64_t>( signed_value );
            }

            if( unsigned_value > std::numeric_limits<std::uint32_t>::max() )
            {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>( unsigned_value );
        }

        [[nodiscard]]
        std::optional<double>
        double_field( const Json&      object,
                      std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_number() )
            {
                return std::nullopt;
            }
            const double number = value->get<double>();
            if( !std::isfinite( number ) )
            {
                return std::nullopt;
            }
            return number;
        }

        [[nodiscard]]
        std::optional<bool>
        bool_field( const Json&      object,
                    std::string_view field )
        {
            const Json* value = find_field( object, field );
            if( value == nullptr || !value->is_boolean() )
            {
                return std::nullopt;
            }
            return value->get<bool>();
        }

        [[nodiscard]]
        constexpr std::optional<std::string_view>
        run_state_name( RunState state ) noexcept
        {
            switch( state )
            {
                case RunState::Running :
                    return runningState;
                case RunState::Done :
                    return doneState;
                case RunState::Failed :
                    return failedState;
                case RunState::Count :
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        constexpr std::optional<RunState>
        run_state_from_name( std::string_view state ) noexcept
        {
            if( state == runningState )
            {
                return RunState::Running;
            }
            if( state == doneState )
            {
                return RunState::Done;
            }
            if( state == failedState )
            {
                return RunState::Failed;
            }
            return std::nullopt;
        }

        [[nodiscard]]
        grab::Result<TargetOutcome>
        decode_target( const Json&                  object,
                       const std::filesystem::path& path,
                       std::size_t                  index )
        {
            const std::string pointer = index_pointer( "/targets", index );
            if( !object.is_object() )
            {
                return malformed_field( path, pointer );
            }

            auto name = string_field( object, nameField );
            if( !name.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, nameField ) );
            }
            auto argv = string_array_field( object, argvField );
            if( !argv.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, argvField ) );
            }
            const auto pid = int64_field( object, pidField );
            if( !pid.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, pidField ) );
            }
            const auto window_id = uint32_field( object, windowIdField );
            if( !window_id.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, windowIdField ) );
            }
            auto files = string_array_field( object, filesField );
            if( !files.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, filesField ) );
            }
            auto error = string_field( object, errorField );
            if( !error.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, errorField ) );
            }

            return TargetOutcome{
                .name      = std::move( *name ),
                .argv      = std::move( *argv ),
                .pid       = *pid,
                .window_id = *window_id,
                .files     = std::move( *files ),
                .error     = std::move( *error ),
            };
        }

        [[nodiscard]]
        grab::Result<FileCompareEntry>
        decode_compare_entry( const Json&                  object,
                              const std::filesystem::path& path,
                              std::size_t                  index )
        {
            const std::string pointer = index_pointer( "/compare", index );
            if( !object.is_object() )
            {
                return malformed_field( path, pointer );
            }

            auto name = string_field( object, nameField );
            if( !name.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, nameField ) );
            }
            const auto score = double_field( object, scoreField );
            if( !score.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, scoreField ) );
            }
            const auto passed = bool_field( object, passedField );
            if( !passed.has_value() )
            {
                return malformed_field( path, field_pointer( pointer, passedField ) );
            }

            return FileCompareEntry{
                .name   = std::move( *name ),
                .score  = *score,
                .passed = *passed,
            };
        }

        [[nodiscard]]
        grab::Result<BatchManifest>
        decode_manifest( const Json&                  object,
                         const std::filesystem::path& path )
        {
            if( !object.is_object() )
            {
                return malformed_field( path, "/" );
            }

            auto profile = string_field( object, profileField );
            if( !profile.has_value() )
            {
                return malformed_field( path, "/profile" );
            }
            auto started_at = string_field( object, startedAtField );
            if( !started_at.has_value() )
            {
                return malformed_field( path, "/started_at" );
            }
            auto ended_at = string_field( object, endedAtField );
            if( !ended_at.has_value() )
            {
                return malformed_field( path, "/ended_at" );
            }
            const auto state_text = string_field( object, stateField );
            if( !state_text.has_value() )
            {
                return malformed_field( path, "/state" );
            }
            const auto state = run_state_from_name( *state_text );
            if( !state.has_value() )
            {
                return malformed_field( path, "/state" );
            }

            const Json* target_array = find_field( object, targetsField );
            if( target_array == nullptr || !target_array->is_array() )
            {
                return malformed_field( path, "/targets" );
            }
            std::vector<TargetOutcome> targets;
            targets.reserve( target_array->size() );
            std::size_t target_index{};
            for( const Json& target_object : *target_array )
            {
                auto target = decode_target( target_object, path, target_index );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                targets.push_back( std::move( *target ) );
                ++target_index;
            }

            const Json* compare_array = find_field( object, compareField );
            if( compare_array == nullptr || !compare_array->is_array() )
            {
                return malformed_field( path, "/compare" );
            }
            std::vector<FileCompareEntry> compare;
            compare.reserve( compare_array->size() );
            std::size_t compare_index{};
            for( const Json& compare_object : *compare_array )
            {
                auto entry = decode_compare_entry( compare_object, path, compare_index );
                if( !entry.has_value() )
                {
                    return std::unexpected( std::move( entry.error() ) );
                }
                compare.push_back( std::move( *entry ) );
                ++compare_index;
            }

            return BatchManifest{
                .profile    = std::filesystem::path{ std::move( *profile ) },
                .started_at = std::move( *started_at ),
                .ended_at   = std::move( *ended_at ),
                .state      = *state,
                .targets    = std::move( targets ),
                .compare    = std::move( compare ),
            };
        }

        [[nodiscard]]
        grab::Result<OrderedJson>
        encode_manifest( const BatchManifest&         manifest,
                         const std::filesystem::path& path )
        {
            const auto state = run_state_name( manifest.state );
            if( !state.has_value() )
            {
                return malformed_field( path, "/state" );
            }

            OrderedJson target_array = OrderedJson::array();
            for( const TargetOutcome& target : manifest.targets )
            {
                OrderedJson target_object = OrderedJson::object();
                target_object.emplace( std::string{ nameField }, target.name );
                target_object.emplace( std::string{ argvField }, target.argv );
                target_object.emplace( std::string{ pidField }, target.pid );
                target_object.emplace( std::string{ windowIdField }, target.window_id );
                target_object.emplace( std::string{ filesField }, target.files );
                target_object.emplace( std::string{ errorField }, target.error );
                target_array.push_back( std::move( target_object ) );
            }

            OrderedJson compare_array = OrderedJson::array();
            std::size_t compare_index{};
            for( const FileCompareEntry& entry : manifest.compare )
            {
                if( !std::isfinite( entry.score ) )
                {
                    return malformed_field(
                        path,
                        field_pointer( index_pointer( "/compare", compare_index ),
                                       scoreField )
                    );
                }
                OrderedJson compare_object = OrderedJson::object();
                compare_object.emplace( std::string{ nameField }, entry.name );
                compare_object.emplace( std::string{ scoreField }, entry.score );
                compare_object.emplace( std::string{ passedField }, entry.passed );
                compare_array.push_back( std::move( compare_object ) );
                ++compare_index;
            }

            OrderedJson object = OrderedJson::object();
            object.emplace( std::string{ profileField }, manifest.profile.string() );
            object.emplace( std::string{ startedAtField }, manifest.started_at );
            object.emplace( std::string{ endedAtField }, manifest.ended_at );
            object.emplace( std::string{ stateField }, std::string{ *state } );
            object.emplace( std::string{ targetsField }, std::move( target_array ) );
            object.emplace( std::string{ compareField }, std::move( compare_array ) );
            return object;
        }

        void
        remove_temporary( const std::filesystem::path& path ) noexcept
        {
            std::error_code ignored;
            static_cast<void>( std::filesystem::remove( path, ignored ) );
        }

    }    // namespace

    grab::Result<void>
    BatchManifest::write( const std::filesystem::path& session_dir ) const
    {
        std::filesystem::path manifest_path;
        std::filesystem::path temporary_path;
        try
        {
            manifest_path  = session_dir / manifestFilename;
            temporary_path = session_dir / temporaryFilename;

            auto document  = encode_manifest( *this, manifest_path );
            if( !document.has_value() )
            {
                return std::unexpected( std::move( document.error() ) );
            }
            const std::string serialized = document->dump();

            std::ofstream     output{
                temporary_path,
                std::ios::binary | std::ios::trunc,
            };
            if( !output.is_open() )
            {
                remove_temporary( temporary_path );
                return write_error( temporary_path, "cannot open temporary file" );
            }

            output << serialized;
            if( !output.good() )
            {
                output.close();
                remove_temporary( temporary_path );
                return write_error( temporary_path, "cannot write temporary file" );
            }
            output.close();
            if( !output )
            {
                remove_temporary( temporary_path );
                return write_error( temporary_path, "cannot close temporary file" );
            }

            std::error_code rename_error;
            std::filesystem::rename( temporary_path, manifest_path, rename_error );
            if( rename_error )
            {
                remove_temporary( temporary_path );
                return write_error( manifest_path,
                                    "cannot replace manifest: " +
                                        rename_error.message() );
            }
            return {};
        }
        catch( const Json::exception& error )
        {
            remove_temporary( temporary_path );
            const auto& path = manifest_path.empty() ? session_dir : manifest_path;
            return write_error( path,
                                std::string{ "cannot serialize manifest: " } +
                                    error.what() );
        }
        catch( const std::exception& error )
        {
            remove_temporary( temporary_path );
            const auto& path = manifest_path.empty() ? session_dir : manifest_path;
            return write_error( path,
                                std::string{ "cannot write manifest: " } +
                                    error.what() );
        }
    }

    grab::Result<BatchManifest>
    BatchManifest::read( const std::filesystem::path& session_dir )
    {
        std::filesystem::path manifest_path;
        try
        {
            manifest_path = session_dir / manifestFilename;
            std::ifstream input{ manifest_path, std::ios::binary };
            if( !input.is_open() )
            {
                std::error_code exists_error;
                if( !std::filesystem::exists( manifest_path, exists_error ) &&
                    !exists_error )
                {
                    return read_error( manifest_path, "file not found" );
                }
                return read_error( manifest_path, "cannot open file" );
            }

            const Json object = Json::parse( input, nullptr, true, false );
            return decode_manifest( object, manifest_path );
        }
        catch( const Json::parse_error& error )
        {
            return read_error( manifest_path,
                               "parse error at byte " +
                                   std::to_string( error.byte ) +
                                   ": " +
                                   error.what() );
        }
        catch( const Json::exception& error )
        {
            return read_error( manifest_path,
                               std::string{ "malformed manifest: " } + error.what() );
        }
        catch( const std::exception& error )
        {
            const auto& path = manifest_path.empty() ? session_dir : manifest_path;
            return read_error( path,
                               std::string{ "cannot read manifest: " } + error.what() );
        }
    }

}    // namespace grab::config
