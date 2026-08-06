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

    // The eight payload-carrying overlay steps, as opposed to the four
    // overlay.* CLI verbs (trail, shape, feedback, sketch) which are whole
    // interactive tools.
    [[nodiscard]]
    inline constexpr bool
    is_overlay_step( grab::CommandKind kind ) noexcept
    {
        switch( kind )
        {
            case grab::CommandKind::OverlayAdd :
            case grab::CommandKind::OverlayUpdate :
            case grab::CommandKind::OverlayRemove :
            case grab::CommandKind::OverlayClear :
            case grab::CommandKind::OverlayGrab :
            case grab::CommandKind::OverlayRelease :
            case grab::CommandKind::OverlayAttach :
            case grab::CommandKind::OverlayDetach :
                return true;
            default :
                return false;
        }
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
        if( descriptor.kind == grab::CommandKind::OverlayShape )
        {
            return "overlay";
        }
        // The overlay STEPS keep their domain in the verb. Without it
        // "overlay.release" would derive the same verb as "input.release" and
        // find_command_by_verb would silently answer with whichever row came
        // first — the exact ambiguity the table's uniqueness test guards.
        // Neither is a real verb; both exist so `grab overlay-release` can say
        // so rather than printing generic usage.
        if( is_overlay_step( descriptor.kind ) )
        {
            const auto  step = descriptor.name.find( '.' );
            std::string prefixed{ "overlay-" };
            prefixed.append( descriptor.name.substr( step + 1U ) );
            return prefixed;
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
