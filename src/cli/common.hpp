#pragma once

#include <cstdio>
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

}    // namespace grab::cli
