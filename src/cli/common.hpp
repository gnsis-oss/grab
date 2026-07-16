#pragma once

#include "grab/command_descriptor.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace grab::cli
{

    inline constexpr int successExitCode = 0;
    inline constexpr int runtimeExitCode = 1;
    inline constexpr int usageExitCode   = 2;

    inline void
    print_error( std::string_view message )
    {
        ( void )std::fputs( "grab: error: ", stderr );
        ( void )std::fwrite( message.data(), sizeof( char ), message.size(), stderr );
        ( void )std::fputc( '\n', stderr );
    }

    // The CLI verb spelling is derived from the canonical command-descriptor
    // name: the segment after the domain dot, with '_' spelled '-'. The one
    // domain-named verb is "session" (descriptor "session.open").
    [[nodiscard]]
    inline std::string
    command_verb( const grab::CommandDescriptor& descriptor )
    {
        if( descriptor.kind == grab::CommandKind::Session )
        {
            return "session";
        }
        const auto  dot = descriptor.name.find( '.' );
        std::string verb{ descriptor.name.substr( dot + 1U ) };
        std::ranges::replace( verb, '_', '-' );
        return verb;
    }

    [[nodiscard]]
    inline const grab::CommandDescriptor*
    find_command_by_verb( std::string_view verb )
    {
        for( const auto& descriptor : grab::list_commands() )
        {
            if( command_verb( descriptor ) == verb )
            {
                return &descriptor;
            }
        }
        return nullptr;
    }

}    // namespace grab::cli
