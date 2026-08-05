#include "frontends/cli/log_options.hpp"
#include "grab/result.hpp"
#include "kernel/support/log.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::cli
{
    namespace
    {

        constexpr std::string_view levelFlag    = "--log-level";
        constexpr std::string_view tagsFlag     = "--log-tags";
        constexpr std::string_view fileFlag     = "--log-file";
        constexpr char             tagSeparator = ',';

        constexpr std::string_view usage =
            "  --log-level off|nominal|verbose|debug   what to emit (default: off)\n"
            "  --log-tags  TAG[,TAG...]                restrict to these subsystems\n"
            "  --log-file  PATH                        write there instead of stderr\n";

        [[nodiscard]]
        bool
        parse_level( std::string_view text,
                     log::Level&      out ) noexcept
        {
            if( text == "off" )
            {
                out = log::Level::Off;
                return true;
            }
            if( text == "nominal" )
            {
                out = log::Level::Nominal;
                return true;
            }
            if( text == "verbose" )
            {
                out = log::Level::Verbose;
                return true;
            }
            if( text == "debug" )
            {
                out = log::Level::Debug;
                return true;
            }
            return false;
        }

    }    // namespace

    std::string_view
    log_options_usage() noexcept
    {
        return usage;
    }

    Result<LogOptions>
    apply_log_options( std::span<char* const> args )
    {
        // The environment is the baseline; a flag overrides it. Calling this
        // before parsing means GRAB_LOG_FILE still applies when only
        // --log-level is given.
        log::configure_from_environment();

        LogOptions               options;
        std::vector<std::string> tags;
        options.remaining.reserve( args.size() );

        for( std::size_t index = 0; index < args.size(); ++index )
        {
            const std::string_view argument{ args[index] };
            const bool             is_log_flag =
                argument == levelFlag || argument == tagsFlag || argument == fileFlag;
            if( !is_log_flag )
            {
                options.remaining.push_back( args[index] );
                continue;
            }

            if( index + 1 >= args.size() )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ argument } + " requires a value" );
            }
            const std::string_view value{ args[index + 1] };
            ++index;

            if( argument == levelFlag )
            {
                auto level = log::Level::Off;
                if( !parse_level( value, level ) )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "unknown log level '" +
                                     std::string{ value } +
                                     "' (off|nominal|verbose|debug)" );
                }
                log::set_runtime_level( level );
            }
            else if( argument == fileFlag )
            {
                if( !log::sink_to_file( value ) )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot open log file '" + std::string{ value } + "'" );
                }
            }
            else
            {
                std::string_view remaining = value;
                while( !remaining.empty() )
                {
                    const auto comma = remaining.find( tagSeparator );
                    if( comma == std::string_view::npos )
                    {
                        tags.emplace_back( remaining );
                        break;
                    }
                    if( comma > 0 )
                    {
                        tags.emplace_back( remaining.substr( 0, comma ) );
                    }
                    remaining.remove_prefix( comma + 1 );
                }
            }
        }

        if( !tags.empty() )
        {
            std::vector<std::string_view> views;
            views.reserve( tags.size() );
            for( const auto& entry : tags )
            {
                views.emplace_back( entry );
            }
            log::set_tag_filter( views );
        }

        return options;
    }

}    // namespace grab::cli
