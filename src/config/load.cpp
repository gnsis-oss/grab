#include "grab/config.hpp"
#include "grab/result.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace grab::config
{
    namespace
    {

        using Json                                         = nlohmann::json;

        constexpr int              maximumNestingDepth     = 64;
        constexpr std::int64_t     supportedSchemaVersion  = 1;
        constexpr std::uint64_t    minimumWatchIntervalMs  = 20U;
        constexpr std::uint64_t    minimumLoopDelayMs      = 100U;
        constexpr std::uint64_t    minimumButton           = 1U;
        constexpr std::uint64_t    maximumButton           = 9U;
        constexpr int              hexadecimalBase         = 16;
        constexpr std::uint8_t     hexadecimalLetterOffset = 10U;
        constexpr std::string_view defaultWatchFilename    = "capture_{timestamp}";
        constexpr std::string_view supportedImageFormat    = "png";
        constexpr std::string_view defaultBatchDirectory   = "sessions";
        constexpr auto             getEnvironment          = &std::getenv;
        constexpr std::array<std::uint64_t, 4U> supportedDepths{
            8U,
            16U,
            24U,
            32U,
        };

        struct ParseState
        {
                bool                                         duplicate_key{};
                std::string                                  duplicate_name;
                bool                                         depth_exceeded{};
                std::vector<std::unordered_set<std::string>> object_keys;
        };

        struct DecodeContext
        {
                std::filesystem::path file;
                std::filesystem::path config_directory;
        };

        [[nodiscard]]
        std::string
        file_prefix( const std::filesystem::path& path )
        {
            return path.string() + ":";
        }

        [[nodiscard]]
        std::string
        escape_pointer_token( std::string_view token )
        {
            std::string escaped;
            escaped.reserve( token.size() );
            for( const char character : token )
            {
                if( character == '~' )
                {
                    escaped += "~0";
                }
                else if( character == '/' )
                {
                    escaped += "~1";
                }
                else
                {
                    escaped.push_back( character );
                }
            }
            return escaped;
        }

        [[nodiscard]]
        std::string
        field_pointer( std::string_view base,
                       std::string_view field )
        {
            return std::string{ base } + "/" + escape_pointer_token( field );
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
        decode_error( const DecodeContext& context,
                      std::string_view     pointer,
                      std::string          reason )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               file_prefix( context.file ) +
                                   std::string{ pointer } +
                                   ": " +
                                   std::move( reason ) );
        }

        [[nodiscard]]
        grab::Result<Json>
        parse_document( const std::filesystem::path& path )
        {
            std::ifstream input{ path };
            if( !input.is_open() )
            {
                std::error_code error;
                if( !std::filesystem::exists( path, error ) && !error )
                {
                    return grab::fail( ErrorCode::InvalidArgument,
                                       file_prefix( path ) + "/: file not found" );
                }
                return grab::fail( ErrorCode::InvalidArgument,
                                   file_prefix( path ) + "/: cannot open file" );
            }

            ParseState                    state;
            const Json::parser_callback_t callback =
                [&state]( int depth, Json::parse_event_t event, Json& parsed )
            {
                if( event == Json::parse_event_t::object_start )
                {
                    state.object_keys.emplace_back();
                }
                else if( event == Json::parse_event_t::key )
                {
                    const std::string key = parsed.get<std::string>();
                    if( !state.object_keys.back().insert( key ).second )
                    {
                        state.duplicate_key  = true;
                        state.duplicate_name = key;
                    }
                }
                else if( event == Json::parse_event_t::object_end )
                {
                    state.object_keys.pop_back();
                }

                if( ( event ==
                      Json::parse_event_t::object_start ||
                      event == Json::parse_event_t::array_start ) &&
                    depth >= maximumNestingDepth )
                {
                    state.depth_exceeded = true;
                }
                return true;
            };

            try
            {
                Json document = Json::parse( input, callback, true, false );
                if( state.duplicate_key )
                {
                    return grab::fail( ErrorCode::InvalidArgument,
                                       file_prefix( path ) +
                                           "/: duplicate key '" +
                                           state.duplicate_name +
                                           "'" );
                }
                if( state.depth_exceeded )
                {
                    return grab::fail( ErrorCode::InvalidArgument,
                                       file_prefix( path ) +
                                           "/: nesting depth exceeds " +
                                           std::to_string( maximumNestingDepth ) );
                }
                return document;
            }
            catch( const Json::parse_error& error )
            {
                return grab::fail( ErrorCode::InvalidArgument,
                                   file_prefix( path ) +
                                       " byte " +
                                       std::to_string( error.byte ) +
                                       ": " +
                                       error.what() );
            }
            catch( const Json::exception& error )
            {
                return grab::fail( ErrorCode::InvalidArgument,
                                   file_prefix( path ) + "/: " + error.what() );
            }
        }

        [[nodiscard]]
        const Json*
        find_member( const Json&      object,
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
        grab::Result<const Json*>
        require_member( const Json&          object,
                        std::string_view     field,
                        const DecodeContext& context,
                        std::string_view     base )
        {
            const Json* const value = find_member( object, field );
            if( value == nullptr )
            {
                return decode_error( context,
                                     field_pointer( base, field ),
                                     "required field is missing" );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<void>
        require_object( const Json&          value,
                        const DecodeContext& context,
                        std::string_view     pointer )
        {
            if( !value.is_object() )
            {
                return decode_error( context, pointer, "must be an object" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        reject_unknown_keys( const Json&                             object,
                             const DecodeContext&                    context,
                             std::string_view                        base,
                             std::initializer_list<std::string_view> allowed )
        {
            for( auto member = object.begin(); member != object.end(); ++member )
            {
                const std::string_view key{ member.key() };
                if( std::ranges::find( allowed, key ) == allowed.end() )
                {
                    return decode_error( context,
                                         field_pointer( base, key ),
                                         "unknown key" );
                }
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::string>
        decode_string( const Json&          value,
                       const DecodeContext& context,
                       std::string_view     pointer )
        {
            if( !value.is_string() )
            {
                return decode_error( context, pointer, "must be a string" );
            }
            return value.get<std::string>();
        }

        [[nodiscard]]
        grab::Result<bool>
        decode_bool( const Json&          value,
                     const DecodeContext& context,
                     std::string_view     pointer )
        {
            if( !value.is_boolean() )
            {
                return decode_error( context, pointer, "must be a boolean" );
            }
            return value.get<bool>();
        }

        [[nodiscard]]
        grab::Result<std::uint64_t>
        decode_unsigned( const Json&          value,
                         const DecodeContext& context,
                         std::string_view     pointer,
                         std::uint64_t        minimum,
                         std::uint64_t        maximum )
        {
            if( !value.is_number_integer() )
            {
                return decode_error( context, pointer, "must be an integer" );
            }

            std::uint64_t parsed{};
            if( value.is_number_unsigned() )
            {
                parsed = value.get<std::uint64_t>();
            }
            else
            {
                const auto signed_value = value.get<std::int64_t>();
                if( signed_value < 0 )
                {
                    return decode_error( context, pointer, "must not be negative" );
                }
                parsed = static_cast<std::uint64_t>( signed_value );
            }

            if( parsed < minimum || parsed > maximum )
            {
                return decode_error( context, pointer, "integer is out of range" );
            }
            return parsed;
        }

        [[nodiscard]]
        grab::Result<std::int64_t>
        decode_signed( const Json&          value,
                       const DecodeContext& context,
                       std::string_view     pointer,
                       std::int64_t         minimum,
                       std::int64_t         maximum )
        {
            if( !value.is_number_integer() )
            {
                return decode_error( context, pointer, "must be an integer" );
            }

            std::int64_t parsed{};
            if( value.is_number_unsigned() )
            {
                const auto unsigned_value = value.get<std::uint64_t>();
                if( !std::in_range<std::int64_t>( unsigned_value ) )
                {
                    return decode_error( context, pointer, "integer is out of range" );
                }
                parsed = static_cast<std::int64_t>( unsigned_value );
            }
            else
            {
                parsed = value.get<std::int64_t>();
            }

            if( parsed < minimum || parsed > maximum )
            {
                return decode_error( context, pointer, "integer is out of range" );
            }
            return parsed;
        }

        [[nodiscard]]
        grab::Result<double>
        decode_number( const Json&          value,
                       const DecodeContext& context,
                       std::string_view     pointer,
                       double               minimum,
                       bool                 minimum_inclusive )
        {
            if( !value.is_number() )
            {
                return decode_error( context, pointer, "must be a number" );
            }
            const double parsed = value.get<double>();
            if( !std::isfinite( parsed ) ||
                ( minimum_inclusive ? parsed < minimum : parsed <= minimum ) )
            {
                return decode_error( context, pointer, "number is out of range" );
            }
            return parsed;
        }

        [[nodiscard]]
        grab::Result<std::filesystem::path>
        decode_path( const Json&                  value,
                     const DecodeContext&         context,
                     std::string_view             pointer,
                     const std::filesystem::path& relative_base )
        {
            auto text = decode_string( value, context, pointer );
            if( !text.has_value() )
            {
                return std::unexpected( std::move( text.error() ) );
            }
            if( text->starts_with( "~/" ) )
            {
                const char* const home = getEnvironment( "HOME" );
                if( home == nullptr || std::string_view{ home }.empty() )
                {
                    return decode_error( context,
                                         pointer,
                                         "cannot expand ~/ because HOME is unset" );
                }
                const std::filesystem::path remainder{ text->substr( 2U ) };
                return ( std::filesystem::path{ home } / remainder.relative_path() )
                    .lexically_normal();
            }
            if( text->starts_with( '~' ) )
            {
                return decode_error( context,
                                     pointer,
                                     "only the ~/ home-directory form is supported" );
            }

            const std::filesystem::path decoded{ *text };
            if( decoded.is_absolute() )
            {
                return decoded.lexically_normal();
            }
            return ( relative_base / decoded ).lexically_normal();
        }

        [[nodiscard]]
        grab::Result<void>
        decode_schema_version( const Json&          document,
                               const DecodeContext& context )
        {
            auto value = require_member( document, "schema_version", context, "" );
            if( !value.has_value() )
            {
                return std::unexpected( std::move( value.error() ) );
            }
            if( !( *value )->is_number_integer() )
            {
                return decode_error( context, "/schema_version", "must be an integer" );
            }

            bool supported{};
            if( ( *value )->is_number_unsigned() )
            {
                supported = ( *value )->get<std::uint64_t>() ==
                            static_cast<std::uint64_t>( supportedSchemaVersion );
            }
            else
            {
                supported = ( *value )->get<std::int64_t>() == supportedSchemaVersion;
            }
            if( !supported )
            {
                return decode_error( context,
                                     "/schema_version",
                                     "unsupported version; supported version is 1" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<std::string>
        required_string( const Json&          object,
                         std::string_view     field,
                         const DecodeContext& context,
                         std::string_view     base )
        {
            auto value = require_member( object, field, context, base );
            if( !value.has_value() )
            {
                return std::unexpected( std::move( value.error() ) );
            }
            return decode_string( **value, context, field_pointer( base, field ) );
        }

        [[nodiscard]]
        grab::Result<std::uint64_t>
        required_unsigned( const Json&          object,
                           std::string_view     field,
                           const DecodeContext& context,
                           std::string_view     base,
                           std::uint64_t        minimum,
                           std::uint64_t        maximum )
        {
            auto value = require_member( object, field, context, base );
            if( !value.has_value() )
            {
                return std::unexpected( std::move( value.error() ) );
            }
            return decode_unsigned( **value,
                                    context,
                                    field_pointer( base, field ),
                                    minimum,
                                    maximum );
        }

        [[nodiscard]]
        grab::Result<DefaultsSection>
        decode_defaults( const Json*          section,
                         const DecodeContext& context )
        {
            DefaultsSection defaults;
            defaults.output_root = context.config_directory;
            if( section == nullptr )
            {
                return defaults;
            }

            auto object = require_object( *section, context, "/defaults" );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys = reject_unknown_keys(
                *section,
                context,
                "/defaults",
                { "format", "timeout_s", "kill_after", "output_root" }
            );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            if( const Json* value = find_member( *section, "format" ); value != nullptr )
            {
                auto format = decode_string( *value, context, "/defaults/format" );
                if( !format.has_value() )
                {
                    return std::unexpected( std::move( format.error() ) );
                }
                if( *format != supportedImageFormat )
                {
                    return decode_error( context,
                                         "/defaults/format",
                                         "unsupported image format" );
                }
                defaults.format = std::move( *format );
            }
            if( const Json* value = find_member( *section, "timeout_s" );
                value != nullptr )
            {
                auto timeout =
                    decode_number( *value, context, "/defaults/timeout_s", 0.0, false );
                if( !timeout.has_value() )
                {
                    return std::unexpected( std::move( timeout.error() ) );
                }
                defaults.timeout_s = *timeout;
            }
            if( const Json* value = find_member( *section, "kill_after" );
                value != nullptr )
            {
                auto kill = decode_bool( *value, context, "/defaults/kill_after" );
                if( !kill.has_value() )
                {
                    return std::unexpected( std::move( kill.error() ) );
                }
                defaults.kill_after = *kill;
            }
            if( const Json* value = find_member( *section, "output_root" );
                value != nullptr )
            {
                auto root = decode_path( *value,
                                         context,
                                         "/defaults/output_root",
                                         context.config_directory );
                if( !root.has_value() )
                {
                    return std::unexpected( std::move( root.error() ) );
                }
                defaults.output_root = std::move( *root );
            }
            return defaults;
        }

        [[nodiscard]]
        grab::Result<DisplayBackend>
        decode_backend( const Json&          value,
                        const DecodeContext& context )
        {
            auto backend = decode_string( value, context, "/display/backend" );
            if( !backend.has_value() )
            {
                return std::unexpected( std::move( backend.error() ) );
            }
            if( *backend == "native" )
            {
                return DisplayBackend::Native;
            }
            if( *backend == "xvfb" )
            {
                return DisplayBackend::Xvfb;
            }
            if( *backend == "weston" || *backend == "xvfb_vgl" )
            {
                return decode_error( context,
                                     "/display/backend",
                                     "backend is deferred" );
            }
            return decode_error( context,
                                 "/display/backend",
                                 "unsupported display backend" );
        }

        template<typename Integer>
        [[nodiscard]]
        grab::Result<void>
        decode_optional_unsigned( const Json&          object,
                                  std::string_view     field,
                                  const DecodeContext& context,
                                  std::string_view     base,
                                  std::uint64_t        minimum,
                                  std::uint64_t        maximum,
                                  Integer&             destination )
        {
            const Json* const value = find_member( object, field );
            if( value == nullptr )
            {
                return {};
            }
            auto parsed = decode_unsigned( *value,
                                           context,
                                           field_pointer( base, field ),
                                           minimum,
                                           maximum );
            if( !parsed.has_value() )
            {
                return std::unexpected( std::move( parsed.error() ) );
            }
            destination = static_cast<Integer>( *parsed );
            return {};
        }

        [[nodiscard]]
        grab::Result<DisplaySection>
        decode_display( const Json*          section,
                        const DecodeContext& context )
        {
            DisplaySection display;
            if( section == nullptr )
            {
                return display;
            }
            auto object = require_object( *section, context, "/display" );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys = reject_unknown_keys( *section,
                                             context,
                                             "/display",
                                             { "backend", "width", "height", "depth" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            if( const Json* value = find_member( *section, "backend" );
                value != nullptr )
            {
                auto backend = decode_backend( *value, context );
                if( !backend.has_value() )
                {
                    return std::unexpected( std::move( backend.error() ) );
                }
                display.backend = *backend;
            }
            auto width =
                decode_optional_unsigned( *section,
                                          "width",
                                          context,
                                          "/display",
                                          1U,
                                          std::numeric_limits<std::uint16_t>::max(),
                                          display.width );
            if( !width.has_value() )
            {
                return std::unexpected( std::move( width.error() ) );
            }
            auto height =
                decode_optional_unsigned( *section,
                                          "height",
                                          context,
                                          "/display",
                                          1U,
                                          std::numeric_limits<std::uint16_t>::max(),
                                          display.height );
            if( !height.has_value() )
            {
                return std::unexpected( std::move( height.error() ) );
            }
            if( const Json* value = find_member( *section, "depth" ); value != nullptr )
            {
                auto depth = decode_unsigned( *value,
                                              context,
                                              "/display/depth",
                                              0U,
                                              std::numeric_limits<std::uint8_t>::max() );
                if( !depth.has_value() )
                {
                    return std::unexpected( std::move( depth.error() ) );
                }
                if( std::ranges::find( supportedDepths, *depth ) ==
                    supportedDepths.end() )
                {
                    return decode_error( context,
                                         "/display/depth",
                                         "unsupported display depth" );
                }
                display.depth = static_cast<std::uint8_t>( *depth );
            }
            return display;
        }

        [[nodiscard]]
        std::optional<std::uint8_t>
        hexadecimal_digit( char character ) noexcept
        {
            if( character >= '0' && character <= '9' )
            {
                return static_cast<std::uint8_t>( character - '0' );
            }
            if( character >= 'a' && character <= 'f' )
            {
                return static_cast<std::uint8_t>( hexadecimalLetterOffset +
                                                  static_cast<std::uint8_t>( character -
                                                                             'a' ) );
            }
            if( character >= 'A' && character <= 'F' )
            {
                return static_cast<std::uint8_t>( hexadecimalLetterOffset +
                                                  static_cast<std::uint8_t>( character -
                                                                             'A' ) );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        decode_window_id( const Json&          value,
                          const DecodeContext& context,
                          std::string_view     pointer )
        {
            auto text = decode_string( value, context, pointer );
            if( !text.has_value() )
            {
                return std::unexpected( std::move( text.error() ) );
            }
            if( text->size() <= 2U || !text->starts_with( "0x" ) )
            {
                return decode_error( context,
                                     pointer,
                                     "must use the 0x-prefixed hexadecimal form" );
            }

            constexpr std::uint64_t maximumWindowId =
                std::numeric_limits<std::uint32_t>::max();
            std::uint64_t parsed{};
            for( const char character : std::string_view{ *text }.substr( 2U ) )
            {
                const auto digit = hexadecimal_digit( character );
                if( !digit.has_value() )
                {
                    return decode_error( context,
                                         pointer,
                                         "malformed hexadecimal window id" );
                }
                if( parsed >
                    ( maximumWindowId - *digit ) /
                    static_cast<std::uint64_t>( hexadecimalBase ) )
                {
                    return decode_error( context,
                                         pointer,
                                         "hexadecimal window id is out of range" );
                }
                parsed =
                    ( parsed * static_cast<std::uint64_t>( hexadecimalBase ) ) + *digit;
            }
            return static_cast<std::uint32_t>( parsed );
        }

        [[nodiscard]]
        grab::Result<TargetMatch>
        decode_target_match( const Json&          section,
                             const DecodeContext& context )
        {
            constexpr std::string_view base   = "/watch/target";
            auto                       object = require_object( section, context, base );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys =
                reject_unknown_keys( section,
                                     context,
                                     base,
                                     { "title", "wm_class", "pid", "window_id" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            if( section.size() != 1U )
            {
                return decode_error( context,
                                     base,
                                     "must contain exactly one match key" );
            }

            TargetMatch match;
            if( const Json* value = find_member( section, "title" ); value != nullptr )
            {
                auto text = decode_string( *value, context, "/watch/target/title" );
                if( !text.has_value() )
                {
                    return std::unexpected( std::move( text.error() ) );
                }
                match.kind = MatchKind::Title;
                match.text = std::move( *text );
                return match;
            }
            if( const Json* value = find_member( section, "wm_class" );
                value != nullptr )
            {
                auto text = decode_string( *value, context, "/watch/target/wm_class" );
                if( !text.has_value() )
                {
                    return std::unexpected( std::move( text.error() ) );
                }
                match.kind = MatchKind::WmClass;
                match.text = std::move( *text );
                return match;
            }
            if( const Json* value = find_member( section, "pid" ); value != nullptr )
            {
                auto pid = decode_unsigned( *value,
                                            context,
                                            "/watch/target/pid",
                                            1U,
                                            std::numeric_limits<std::uint32_t>::max() );
                if( !pid.has_value() )
                {
                    return std::unexpected( std::move( pid.error() ) );
                }
                match.kind = MatchKind::Pid;
                match.pid  = static_cast<std::uint32_t>( *pid );
                return match;
            }

            auto window_id = decode_window_id( *find_member( section, "window_id" ),
                                               context,
                                               "/watch/target/window_id" );
            if( !window_id.has_value() )
            {
                return std::unexpected( std::move( window_id.error() ) );
            }
            match.kind      = MatchKind::WindowId;
            match.window_id = *window_id;
            return match;
        }

        [[nodiscard]]
        grab::Result<WatchLimits>
        decode_watch_limits( const Json&          section,
                             const DecodeContext& context )
        {
            constexpr std::string_view base   = "/watch/limits";
            auto                       object = require_object( section, context, base );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys =
                reject_unknown_keys( section,
                                     context,
                                     base,
                                     { "max_files", "max_age_days", "max_disk_mib" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            WatchLimits limits;
            auto        max_files =
                decode_optional_unsigned( section,
                                          "max_files",
                                          context,
                                          base,
                                          0U,
                                          std::numeric_limits<std::uint64_t>::max(),
                                          limits.max_files );
            if( !max_files.has_value() )
            {
                return std::unexpected( std::move( max_files.error() ) );
            }
            auto max_age =
                decode_optional_unsigned( section,
                                          "max_age_days",
                                          context,
                                          base,
                                          0U,
                                          std::numeric_limits<std::uint32_t>::max(),
                                          limits.max_age_days );
            if( !max_age.has_value() )
            {
                return std::unexpected( std::move( max_age.error() ) );
            }
            auto max_disk =
                decode_optional_unsigned( section,
                                          "max_disk_mib",
                                          context,
                                          base,
                                          0U,
                                          std::numeric_limits<std::uint64_t>::max(),
                                          limits.max_disk_mib );
            if( !max_disk.has_value() )
            {
                return std::unexpected( std::move( max_disk.error() ) );
            }
            return limits;
        }

        [[nodiscard]]
        grab::Result<WatchSection>
        decode_watch( const Json&                  section,
                      const DecodeContext&         context,
                      const std::filesystem::path& output_root,
                      std::string_view             default_format )
        {
            constexpr std::string_view base   = "/watch";
            auto                       object = require_object( section, context, base );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys = reject_unknown_keys(
                section,
                context,
                base,
                { "interval_ms", "output", "filename", "format", "target", "limits" }
            );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            auto interval =
                required_unsigned( section,
                                   "interval_ms",
                                   context,
                                   base,
                                   minimumWatchIntervalMs,
                                   std::numeric_limits<std::uint32_t>::max() );
            if( !interval.has_value() )
            {
                return std::unexpected( std::move( interval.error() ) );
            }
            auto output_value = require_member( section, "output", context, base );
            if( !output_value.has_value() )
            {
                return std::unexpected( std::move( output_value.error() ) );
            }
            auto output =
                decode_path( **output_value, context, "/watch/output", output_root );
            if( !output.has_value() )
            {
                return std::unexpected( std::move( output.error() ) );
            }

            WatchSection watch;
            watch.interval_ms = static_cast<std::uint32_t>( *interval );
            watch.output      = std::move( *output );
            watch.filename    = defaultWatchFilename;
            watch.format      = default_format;
            if( const Json* value = find_member( section, "filename" );
                value != nullptr )
            {
                auto filename = decode_string( *value, context, "/watch/filename" );
                if( !filename.has_value() )
                {
                    return std::unexpected( std::move( filename.error() ) );
                }
                watch.filename = std::move( *filename );
            }
            if( const Json* value = find_member( section, "format" ); value != nullptr )
            {
                auto format = decode_string( *value, context, "/watch/format" );
                if( !format.has_value() )
                {
                    return std::unexpected( std::move( format.error() ) );
                }
                if( *format != supportedImageFormat )
                {
                    return decode_error( context,
                                         "/watch/format",
                                         "unsupported image format" );
                }
                watch.format = std::move( *format );
            }
            if( const Json* value = find_member( section, "target" ); value != nullptr )
            {
                auto target = decode_target_match( *value, context );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                watch.target = std::move( *target );
            }
            if( const Json* value = find_member( section, "limits" ); value != nullptr )
            {
                auto limits = decode_watch_limits( *value, context );
                if( !limits.has_value() )
                {
                    return std::unexpected( std::move( limits.error() ) );
                }
                watch.limits = *limits;
            }
            return watch;
        }

        [[nodiscard]]
        grab::Result<void>
        decode_coordinate( const Json&          object,
                           std::string_view     field,
                           const DecodeContext& context,
                           std::string_view     base,
                           std::int16_t&        destination )
        {
            auto value = require_member( object, field, context, base );
            if( !value.has_value() )
            {
                return std::unexpected( std::move( value.error() ) );
            }
            auto parsed = decode_signed( **value,
                                         context,
                                         field_pointer( base, field ),
                                         std::numeric_limits<std::int16_t>::min(),
                                         std::numeric_limits<std::int16_t>::max() );
            if( !parsed.has_value() )
            {
                return std::unexpected( std::move( parsed.error() ) );
            }
            destination = static_cast<std::int16_t>( *parsed );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        decode_button( const Json&          object,
                       const DecodeContext& context,
                       std::string_view     base,
                       ScriptStep&          step )
        {
            auto parsed = decode_optional_unsigned( object,
                                                    "button",
                                                    context,
                                                    base,
                                                    minimumButton,
                                                    maximumButton,
                                                    step.button );
            if( !parsed.has_value() )
            {
                return std::unexpected( std::move( parsed.error() ) );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        decode_coordinate_pair( const Json&          value,
                                const DecodeContext& context,
                                std::string_view     pointer,
                                std::int16_t&        x,
                                std::int16_t&        y )
        {
            constexpr std::size_t coordinateCount = 2U;
            if( !value.is_array() || value.size() != coordinateCount )
            {
                return decode_error( context, pointer, "must be a two-integer array" );
            }
            auto decoded_x = decode_signed( value.front(),
                                            context,
                                            index_pointer( pointer, 0U ),
                                            std::numeric_limits<std::int16_t>::min(),
                                            std::numeric_limits<std::int16_t>::max() );
            if( !decoded_x.has_value() )
            {
                return std::unexpected( std::move( decoded_x.error() ) );
            }
            auto decoded_y = decode_signed( value.back(),
                                            context,
                                            index_pointer( pointer, 1U ),
                                            std::numeric_limits<std::int16_t>::min(),
                                            std::numeric_limits<std::int16_t>::max() );
            if( !decoded_y.has_value() )
            {
                return std::unexpected( std::move( decoded_y.error() ) );
            }
            x = static_cast<std::int16_t>( *decoded_x );
            y = static_cast<std::int16_t>( *decoded_y );
            return {};
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_move_step( const Json&          object,
                          const DecodeContext& context,
                          std::string_view     base )
        {
            auto keys =
                reject_unknown_keys( object, context, base, { "action", "x", "y" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            ScriptStep step;
            step.action = StepAction::Move;
            auto x      = decode_coordinate( object, "x", context, base, step.x );
            if( !x.has_value() )
            {
                return std::unexpected( std::move( x.error() ) );
            }
            auto y = decode_coordinate( object, "y", context, base, step.y );
            if( !y.has_value() )
            {
                return std::unexpected( std::move( y.error() ) );
            }
            return step;
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_click_step( const Json&          object,
                           const DecodeContext& context,
                           std::string_view     base )
        {
            auto keys =
                reject_unknown_keys( object, context, base, { "action", "button" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            ScriptStep step;
            step.action = StepAction::Click;
            auto button = decode_button( object, context, base, step );
            if( !button.has_value() )
            {
                return std::unexpected( std::move( button.error() ) );
            }
            return step;
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_click_at_step( const Json&          object,
                              const DecodeContext& context,
                              std::string_view     base )
        {
            auto keys = reject_unknown_keys( object,
                                             context,
                                             base,
                                             { "action", "x", "y", "button" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            ScriptStep step;
            step.action = StepAction::ClickAt;
            auto x      = decode_coordinate( object, "x", context, base, step.x );
            if( !x.has_value() )
            {
                return std::unexpected( std::move( x.error() ) );
            }
            auto y = decode_coordinate( object, "y", context, base, step.y );
            if( !y.has_value() )
            {
                return std::unexpected( std::move( y.error() ) );
            }
            auto button = decode_button( object, context, base, step );
            if( !button.has_value() )
            {
                return std::unexpected( std::move( button.error() ) );
            }
            return step;
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_drag_step( const Json&          object,
                          const DecodeContext& context,
                          std::string_view     base )
        {
            auto keys =
                reject_unknown_keys( object, context, base, { "action", "from", "to" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            auto from = require_member( object, "from", context, base );
            if( !from.has_value() )
            {
                return std::unexpected( std::move( from.error() ) );
            }
            auto to = require_member( object, "to", context, base );
            if( !to.has_value() )
            {
                return std::unexpected( std::move( to.error() ) );
            }

            ScriptStep step;
            step.action    = StepAction::Drag;
            auto from_pair = decode_coordinate_pair( **from,
                                                     context,
                                                     field_pointer( base, "from" ),
                                                     step.x,
                                                     step.y );
            if( !from_pair.has_value() )
            {
                return std::unexpected( std::move( from_pair.error() ) );
            }
            auto to_pair = decode_coordinate_pair( **to,
                                                   context,
                                                   field_pointer( base, "to" ),
                                                   step.to_x,
                                                   step.to_y );
            if( !to_pair.has_value() )
            {
                return std::unexpected( std::move( to_pair.error() ) );
            }
            return step;
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_text_step( const Json&          object,
                          const DecodeContext& context,
                          std::string_view     base,
                          StepAction           action,
                          std::string_view     field )
        {
            auto keys =
                reject_unknown_keys( object, context, base, { "action", field } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            auto text = required_string( object, field, context, base );
            if( !text.has_value() )
            {
                return std::unexpected( std::move( text.error() ) );
            }
            ScriptStep step;
            step.action = action;
            step.text   = std::move( *text );
            return step;
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_delay_step( const Json&          object,
                           const DecodeContext& context,
                           std::string_view     base )
        {
            auto keys = reject_unknown_keys( object, context, base, { "action", "ms" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            auto delay = required_unsigned( object,
                                            "ms",
                                            context,
                                            base,
                                            1U,
                                            std::numeric_limits<std::uint32_t>::max() );
            if( !delay.has_value() )
            {
                return std::unexpected( std::move( delay.error() ) );
            }
            ScriptStep step;
            step.action   = StepAction::Delay;
            step.delay_ms = static_cast<std::uint32_t>( *delay );
            return step;
        }

        [[nodiscard]]
        grab::Result<ScriptStep>
        decode_script_step( const Json&          object,
                            const DecodeContext& context,
                            std::string_view     base )
        {
            auto is_object = require_object( object, context, base );
            if( !is_object.has_value() )
            {
                return std::unexpected( std::move( is_object.error() ) );
            }
            auto action = required_string( object, "action", context, base );
            if( !action.has_value() )
            {
                return std::unexpected( std::move( action.error() ) );
            }
            if( *action == "move" )
            {
                return decode_move_step( object, context, base );
            }
            if( *action == "click" )
            {
                return decode_click_step( object, context, base );
            }
            if( *action == "click_at" )
            {
                return decode_click_at_step( object, context, base );
            }
            if( *action == "drag" )
            {
                return decode_drag_step( object, context, base );
            }
            if( *action == "type" )
            {
                return decode_text_step( object,
                                         context,
                                         base,
                                         StepAction::Type,
                                         "text" );
            }
            if( *action == "key" )
            {
                return decode_text_step( object,
                                         context,
                                         base,
                                         StepAction::Key,
                                         "name" );
            }
            if( *action == "delay" )
            {
                return decode_delay_step( object, context, base );
            }
            return decode_error( context,
                                 field_pointer( base, "action" ),
                                 "unsupported script action" );
        }

        [[nodiscard]]
        grab::Result<ScriptSection>
        decode_script( const Json&          section,
                       const DecodeContext& context )
        {
            constexpr std::string_view base   = "/script";
            auto                       object = require_object( section, context, base );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys =
                reject_unknown_keys( section, context, base, { "loop", "steps" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            ScriptSection script;
            if( const Json* value = find_member( section, "loop" ); value != nullptr )
            {
                auto loop = decode_bool( *value, context, "/script/loop" );
                if( !loop.has_value() )
                {
                    return std::unexpected( std::move( loop.error() ) );
                }
                script.loop = *loop;
            }
            auto steps = require_member( section, "steps", context, base );
            if( !steps.has_value() )
            {
                return std::unexpected( std::move( steps.error() ) );
            }
            if( !( *steps )->is_array() || ( *steps )->empty() )
            {
                return decode_error( context,
                                     "/script/steps",
                                     "must be a non-empty array" );
            }

            std::uint64_t total_delay{};
            script.steps.reserve( ( *steps )->size() );
            for( std::size_t index = 0U; index < ( *steps )->size(); ++index )
            {
                auto step =
                    decode_script_step( ( *steps )->at( index ),
                                        context,
                                        index_pointer( "/script/steps", index ) );
                if( !step.has_value() )
                {
                    return std::unexpected( std::move( step.error() ) );
                }
                total_delay += step->delay_ms;
                script.steps.push_back( std::move( *step ) );
            }
            if( script.loop && total_delay < minimumLoopDelayMs )
            {
                return decode_error(
                    context,
                    "/script/steps",
                    "loop requires at least 100 ms of delay per cycle"
                );
            }
            return script;
        }

        [[nodiscard]]
        bool
        is_safe_target_name( std::string_view name ) noexcept
        {
            if( name.empty() )
            {
                return false;
            }
            return std::ranges::all_of(
                name,
                []( char character )
                {
                    return ( character >= 'A' && character <= 'Z' ) ||
                           ( character >= 'a' && character <= 'z' ) ||
                           ( character >= '0' && character <= '9' ) ||
                           character ==
                           '.' ||
                           character ==
                           '_' ||
                           character == '-';
                }
            );
        }

        [[nodiscard]]
        grab::Result<void>
        decode_target_argv( const Json&          object,
                            const DecodeContext& context,
                            std::string_view     base,
                            TargetSpec&          target )
        {
            auto value = require_member( object, "argv", context, base );
            if( !value.has_value() )
            {
                return std::unexpected( std::move( value.error() ) );
            }
            if( !( *value )->is_array() || ( *value )->empty() )
            {
                return decode_error( context,
                                     field_pointer( base, "argv" ),
                                     "must be a non-empty string array" );
            }

            target.argv.reserve( ( *value )->size() );
            for( std::size_t index = 0U; index < ( *value )->size(); ++index )
            {
                auto argument =
                    decode_string( ( *value )->at( index ),
                                   context,
                                   index_pointer( field_pointer( base, "argv" ),
                                                  index ) );
                if( !argument.has_value() )
                {
                    return std::unexpected( std::move( argument.error() ) );
                }
                target.argv.push_back( std::move( *argument ) );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        decode_target_env( const Json&          object,
                           const DecodeContext& context,
                           std::string_view     base,
                           DisplayBackend       backend,
                           TargetSpec&          target )
        {
            const Json* const environment = find_member( object, "env" );
            if( environment == nullptr )
            {
                return {};
            }
            const std::string env_pointer = field_pointer( base, "env" );
            auto env_object = require_object( *environment, context, env_pointer );
            if( !env_object.has_value() )
            {
                return std::unexpected( std::move( env_object.error() ) );
            }

            target.env.reserve( environment->size() );
            for( auto member = environment->begin(); member != environment->end();
                 ++member )
            {
                const std::string value_pointer =
                    field_pointer( env_pointer, member.key() );
                auto value = decode_string( member.value(), context, value_pointer );
                if( !value.has_value() )
                {
                    return std::unexpected( std::move( value.error() ) );
                }
                if( backend == DisplayBackend::Xvfb && member.key() == "DISPLAY" )
                {
                    return decode_error( context,
                                         value_pointer,
                                         "DISPLAY is owned by the xvfb backend" );
                }
                target.env.emplace_back( member.key(), std::move( *value ) );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        decode_target_match_mode( const Json&          object,
                                  const DecodeContext& context,
                                  std::string_view     base,
                                  TargetSpec&          target )
        {
            if( const Json* value = find_member( object, "match" ); value != nullptr )
            {
                auto match =
                    decode_string( *value, context, field_pointer( base, "match" ) );
                if( !match.has_value() )
                {
                    return std::unexpected( std::move( match.error() ) );
                }
                if( *match == "pid" )
                {
                    target.match = MatchKind::Pid;
                }
                else if( *match == "wm_class" )
                {
                    target.match = MatchKind::WmClass;
                }
                else if( *match == "title" )
                {
                    target.match = MatchKind::Title;
                }
                else
                {
                    return decode_error( context,
                                         field_pointer( base, "match" ),
                                         "unsupported target match mode" );
                }
            }

            const Json* const pattern_value = find_member( object, "pattern" );
            if( target.match == MatchKind::Pid )
            {
                if( pattern_value != nullptr )
                {
                    return decode_error( context,
                                         field_pointer( base, "pattern" ),
                                         "pattern is forbidden for pid matching" );
                }
                return {};
            }
            if( pattern_value == nullptr )
            {
                return decode_error( context,
                                     field_pointer( base, "pattern" ),
                                     "pattern is required for this match mode" );
            }
            auto pattern = decode_string( *pattern_value,
                                          context,
                                          field_pointer( base, "pattern" ) );
            if( !pattern.has_value() )
            {
                return std::unexpected( std::move( pattern.error() ) );
            }
            target.pattern = std::move( *pattern );
            return {};
        }

        [[nodiscard]]
        grab::Result<void>
        decode_target_numbers( const Json&          object,
                               const DecodeContext& context,
                               std::string_view     base,
                               TargetSpec&          target )
        {
            auto frames =
                decode_optional_unsigned( object,
                                          "frames",
                                          context,
                                          base,
                                          1U,
                                          std::numeric_limits<std::uint32_t>::max(),
                                          target.frames );
            if( !frames.has_value() )
            {
                return std::unexpected( std::move( frames.error() ) );
            }
            auto interval =
                decode_optional_unsigned( object,
                                          "interval_ms",
                                          context,
                                          base,
                                          minimumWatchIntervalMs,
                                          std::numeric_limits<std::uint32_t>::max(),
                                          target.interval_ms );
            if( !interval.has_value() )
            {
                return std::unexpected( std::move( interval.error() ) );
            }
            auto delay =
                decode_optional_unsigned( object,
                                          "delay_ms",
                                          context,
                                          base,
                                          0U,
                                          std::numeric_limits<std::uint32_t>::max(),
                                          target.delay_ms );
            if( !delay.has_value() )
            {
                return std::unexpected( std::move( delay.error() ) );
            }
            if( const Json* value = find_member( object, "timeout_s" );
                value != nullptr )
            {
                auto timeout = decode_number( *value,
                                              context,
                                              field_pointer( base, "timeout_s" ),
                                              0.0,
                                              false );
                if( !timeout.has_value() )
                {
                    return std::unexpected( std::move( timeout.error() ) );
                }
                target.timeout_s = *timeout;
            }
            if( const Json* value = find_member( object, "kill_after" );
                value != nullptr )
            {
                auto kill =
                    decode_bool( *value, context, field_pointer( base, "kill_after" ) );
                if( !kill.has_value() )
                {
                    return std::unexpected( std::move( kill.error() ) );
                }
                target.kill_after = *kill;
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<TargetSpec>
        decode_target( const Json&            object,
                       const DecodeContext&   context,
                       std::string_view       base,
                       const DefaultsSection& defaults,
                       DisplayBackend         backend )
        {
            auto is_object = require_object( object, context, base );
            if( !is_object.has_value() )
            {
                return std::unexpected( std::move( is_object.error() ) );
            }
            auto keys = reject_unknown_keys( object,
                                             context,
                                             base,
                                             {
                                                 "name",
                                                 "argv",
                                                 "env",
                                                 "match",
                                                 "pattern",
                                                 "frames",
                                                 "interval_ms",
                                                 "delay_ms",
                                                 "timeout_s",
                                                 "kill_after",
                                             } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            auto name = required_string( object, "name", context, base );
            if( !name.has_value() )
            {
                return std::unexpected( std::move( name.error() ) );
            }
            if( !is_safe_target_name( *name ) )
            {
                return decode_error( context,
                                     field_pointer( base, "name" ),
                                     "name is not filesystem-safe" );
            }

            TargetSpec target;
            target.name       = std::move( *name );
            target.timeout_s  = defaults.timeout_s;
            target.kill_after = defaults.kill_after;
            auto argv         = decode_target_argv( object, context, base, target );
            if( !argv.has_value() )
            {
                return std::unexpected( std::move( argv.error() ) );
            }
            auto env = decode_target_env( object, context, base, backend, target );
            if( !env.has_value() )
            {
                return std::unexpected( std::move( env.error() ) );
            }
            auto match = decode_target_match_mode( object, context, base, target );
            if( !match.has_value() )
            {
                return std::unexpected( std::move( match.error() ) );
            }
            auto numbers = decode_target_numbers( object, context, base, target );
            if( !numbers.has_value() )
            {
                return std::unexpected( std::move( numbers.error() ) );
            }
            return target;
        }

        [[nodiscard]]
        grab::Result<std::vector<TargetSpec>>
        decode_targets( const Json*            section,
                        const DecodeContext&   context,
                        const DefaultsSection& defaults,
                        DisplayBackend         backend )
        {
            std::vector<TargetSpec> targets;
            if( section == nullptr )
            {
                return targets;
            }
            if( !section->is_array() || section->empty() )
            {
                return decode_error( context, "/targets", "must be a non-empty array" );
            }

            std::unordered_set<std::string> names;
            targets.reserve( section->size() );
            for( std::size_t index = 0U; index < section->size(); ++index )
            {
                const std::string base   = index_pointer( "/targets", index );
                auto              target = decode_target( section->at( index ),
                                                          context,
                                                          base,
                                                          defaults,
                                                          backend );
                if( !target.has_value() )
                {
                    return std::unexpected( std::move( target.error() ) );
                }
                if( !names.insert( target->name ).second )
                {
                    return decode_error( context,
                                         field_pointer( base, "name" ),
                                         "target name must be unique" );
                }
                targets.push_back( std::move( *target ) );
            }
            return targets;
        }

        [[nodiscard]]
        grab::Result<BatchSection>
        decode_batch( const Json*                  section,
                      const DecodeContext&         context,
                      const std::filesystem::path& output_root )
        {
            BatchSection batch;
            batch.output_root = output_root / defaultBatchDirectory;
            if( section == nullptr )
            {
                return batch;
            }
            auto object = require_object( *section, context, "/batch" );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys =
                reject_unknown_keys( *section, context, "/batch", { "output_root" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }
            if( const Json* value = find_member( *section, "output_root" );
                value != nullptr )
            {
                auto root =
                    decode_path( *value, context, "/batch/output_root", output_root );
                if( !root.has_value() )
                {
                    return std::unexpected( std::move( root.error() ) );
                }
                batch.output_root = std::move( *root );
            }
            return batch;
        }

        [[nodiscard]]
        grab::Result<NotifyStrategy>
        decode_notify_strategy( const Json&          value,
                                const DecodeContext& context )
        {
            auto strategy = decode_string( value, context, "/notifications/strategy" );
            if( !strategy.has_value() )
            {
                return std::unexpected( std::move( strategy.error() ) );
            }
            if( *strategy == "os" )
            {
                return NotifyStrategy::Os;
            }
            if( *strategy == "none" )
            {
                return NotifyStrategy::None;
            }
            return decode_error( context,
                                 "/notifications/strategy",
                                 "unsupported notification strategy" );
        }

        [[nodiscard]]
        grab::Result<NotifySection>
        decode_notifications( const Json*          section,
                              const DecodeContext& context )
        {
            NotifySection notifications;
            if( section == nullptr )
            {
                return notifications;
            }
            auto object = require_object( *section, context, "/notifications" );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys =
                reject_unknown_keys( *section,
                                     context,
                                     "/notifications",
                                     { "enabled", "strategy", "popup_timeout_ms" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            if( const Json* value = find_member( *section, "enabled" );
                value != nullptr )
            {
                auto enabled = decode_bool( *value, context, "/notifications/enabled" );
                if( !enabled.has_value() )
                {
                    return std::unexpected( std::move( enabled.error() ) );
                }
                notifications.enabled = *enabled;
            }
            if( const Json* value = find_member( *section, "strategy" );
                value != nullptr )
            {
                auto strategy = decode_notify_strategy( *value, context );
                if( !strategy.has_value() )
                {
                    return std::unexpected( std::move( strategy.error() ) );
                }
                notifications.strategy = *strategy;
            }
            auto timeout =
                decode_optional_unsigned( *section,
                                          "popup_timeout_ms",
                                          context,
                                          "/notifications",
                                          0U,
                                          std::numeric_limits<std::uint32_t>::max(),
                                          notifications.popup_timeout_ms );
            if( !timeout.has_value() )
            {
                return std::unexpected( std::move( timeout.error() ) );
            }
            return notifications;
        }

        [[nodiscard]]
        grab::Result<CompareMode>
        decode_compare_mode( const Json&          value,
                             const DecodeContext& context )
        {
            auto mode = decode_string( value, context, "/compare/mode" );
            if( !mode.has_value() )
            {
                return std::unexpected( std::move( mode.error() ) );
            }
            if( *mode == "exact" )
            {
                return CompareMode::Exact;
            }
            if( *mode == "rmse" )
            {
                return CompareMode::Rmse;
            }
            return decode_error( context,
                                 "/compare/mode",
                                 "unsupported comparison mode" );
        }

        [[nodiscard]]
        grab::Result<CompareSection>
        decode_compare( const Json*          section,
                        const DecodeContext& context )
        {
            CompareSection compare;
            if( section == nullptr )
            {
                return compare;
            }
            auto object = require_object( *section, context, "/compare" );
            if( !object.has_value() )
            {
                return std::unexpected( std::move( object.error() ) );
            }
            auto keys = reject_unknown_keys( *section,
                                             context,
                                             "/compare",
                                             { "mode", "threshold", "ref" } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            if( const Json* value = find_member( *section, "mode" ); value != nullptr )
            {
                auto mode = decode_compare_mode( *value, context );
                if( !mode.has_value() )
                {
                    return std::unexpected( std::move( mode.error() ) );
                }
                compare.mode = *mode;
            }
            if( const Json* value = find_member( *section, "threshold" );
                value != nullptr )
            {
                auto threshold =
                    decode_number( *value, context, "/compare/threshold", 0.0, true );
                if( !threshold.has_value() )
                {
                    return std::unexpected( std::move( threshold.error() ) );
                }
                compare.threshold = *threshold;
            }
            if( const Json* value = find_member( *section, "ref" ); value != nullptr )
            {
                auto reference = decode_path( *value,
                                              context,
                                              "/compare/ref",
                                              context.config_directory );
                if( !reference.has_value() )
                {
                    return std::unexpected( std::move( reference.error() ) );
                }
                compare.ref = std::move( *reference );
            }
            return compare;
        }

        [[nodiscard]]
        grab::Result<Config>
        decode_document( const Json&          document,
                         const DecodeContext& context )
        {
            auto schema = decode_schema_version( document, context );
            if( !schema.has_value() )
            {
                return std::unexpected( std::move( schema.error() ) );
            }
            auto keys = reject_unknown_keys( document,
                                             context,
                                             "",
                                             {
                                                 "schema_version",
                                                 "defaults",
                                                 "display",
                                                 "watch",
                                                 "script",
                                                 "targets",
                                                 "batch",
                                                 "notifications",
                                                 "compare",
                                             } );
            if( !keys.has_value() )
            {
                return std::unexpected( std::move( keys.error() ) );
            }

            Config config;
            config.source = context.file;
            auto defaults =
                decode_defaults( find_member( document, "defaults" ), context );
            if( !defaults.has_value() )
            {
                return std::unexpected( std::move( defaults.error() ) );
            }
            config.defaults = std::move( *defaults );
            auto display = decode_display( find_member( document, "display" ), context );
            if( !display.has_value() )
            {
                return std::unexpected( std::move( display.error() ) );
            }
            config.display = *display;

            if( const Json* section = find_member( document, "watch" );
                section != nullptr )
            {
                auto watch = decode_watch( *section,
                                           context,
                                           config.defaults.output_root,
                                           config.defaults.format );
                if( !watch.has_value() )
                {
                    return std::unexpected( std::move( watch.error() ) );
                }
                config.watch = std::move( *watch );
            }
            if( const Json* section = find_member( document, "script" );
                section != nullptr )
            {
                auto script = decode_script( *section, context );
                if( !script.has_value() )
                {
                    return std::unexpected( std::move( script.error() ) );
                }
                config.script = std::move( *script );
            }
            auto targets = decode_targets( find_member( document, "targets" ),
                                           context,
                                           config.defaults,
                                           config.display.backend );
            if( !targets.has_value() )
            {
                return std::unexpected( std::move( targets.error() ) );
            }
            config.targets = std::move( *targets );
            auto batch     = decode_batch( find_member( document, "batch" ),
                                           context,
                                           config.defaults.output_root );
            if( !batch.has_value() )
            {
                return std::unexpected( std::move( batch.error() ) );
            }
            config.batch = std::move( *batch );
            auto notifications =
                decode_notifications( find_member( document, "notifications" ),
                                      context );
            if( !notifications.has_value() )
            {
                return std::unexpected( std::move( notifications.error() ) );
            }
            config.notifications = *notifications;
            auto compare = decode_compare( find_member( document, "compare" ), context );
            if( !compare.has_value() )
            {
                return std::unexpected( std::move( compare.error() ) );
            }
            config.compare = std::move( *compare );
            if( config.script.has_value() && !config.watch.has_value() )
            {
                return decode_error( context,
                                     "/script",
                                     "script requires a watch section" );
            }
            return config;
        }

    }    // namespace

    grab::Result<Config>
    load( const std::filesystem::path& path )
    {
        auto document = parse_document( path );
        if( !document.has_value() )
        {
            return std::unexpected( std::move( document.error() ) );
        }
        if( !document->is_object() )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               file_prefix( path ) + "/: root must be an object" );
        }

        std::error_code path_error;
        const auto      absolute_path = std::filesystem::absolute( path, path_error );
        if( path_error )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               file_prefix( path ) +
                                   "/: cannot resolve config directory" );
        }
        const DecodeContext context{
            .file             = path,
            .config_directory = absolute_path.parent_path(),
        };
        return decode_document( *document, context );
    }

    grab::Result<std::vector<Config>>
    resolve( std::span<const std::string_view> explicit_paths )
    {
        std::vector<Config> configs;
        if( !explicit_paths.empty() )
        {
            configs.reserve( explicit_paths.size() );
            for( const std::string_view explicit_path : explicit_paths )
            {
                auto config = load( std::filesystem::path{ explicit_path } );
                if( !config.has_value() )
                {
                    return std::unexpected( std::move( config.error() ) );
                }
                configs.push_back( std::move( *config ) );
            }
            return configs;
        }

        const char* const fallback = getEnvironment( "GRAB_CONFIG" );
        if( fallback == nullptr || std::string_view{ fallback }.empty() )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               "config:/: no config path provided; set GRAB_CONFIG" );
        }
        auto config = load( std::filesystem::path{ fallback } );
        if( !config.has_value() )
        {
            return std::unexpected( std::move( config.error() ) );
        }
        configs.push_back( std::move( *config ) );
        return configs;
    }

}    // namespace grab::config
