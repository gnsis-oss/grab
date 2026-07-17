#pragma once

#include <stdlib.h>

namespace grab::core::posix
{

    [[nodiscard]]
    inline int
    mkstemp( char* file_template )
    {
        return ::mkstemp( file_template );
    }

}    // namespace grab::core::posix
