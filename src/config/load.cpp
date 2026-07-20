#include "grab/config.hpp"
#include "grab/result.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <nlohmann/json_fwd.hpp>
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

        using Json                                    = nlohmann::json;

        constexpr int          maximumNestingDepth    = 64;
        constexpr std::int64_t supportedSchemaVersion = 1;

        struct ParseState
        {
                bool                                         duplicate_key{};
                std::string                                  duplicate_name;
                bool                                         depth_exceeded{};
                std::vector<std::unordered_set<std::string>> object_keys;
        };

        [[nodiscard]]
        std::string
        file_prefix( const std::filesystem::path& path )
        {
            return path.string() + ":";
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
        bool
        has_supported_schema_version( const Json& document )
        {
            const auto member = document.find( "schema_version" );
            if( member == document.end() || !member->is_number_integer() )
            {
                return false;
            }
            if( member->is_number_unsigned() )
            {
                return member->get<std::uint64_t>() ==
                       static_cast<std::uint64_t>( supportedSchemaVersion );
            }
            return member->get<std::int64_t>() == supportedSchemaVersion;
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
        if( !has_supported_schema_version( *document ) )
        {
            return grab::fail( ErrorCode::InvalidArgument,
                               file_prefix( path ) +
                                   "/schema_version: must be integer 1" );
        }

        Config config{};
        config.source               = path;
        config.defaults.output_root = path.parent_path();
        config.batch.output_root    = config.defaults.output_root / "sessions";
        return config;
    }

    grab::Result<std::vector<Config>>
    resolve( std::span<const std::string_view> explicit_paths )
    {
        static_cast<void>( explicit_paths );
        return grab::fail( ErrorCode::InvalidArgument,
                           "config resolution is not implemented" );
    }

}    // namespace grab::config
