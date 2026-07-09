#ifndef CORE_POSIX_MKSTEMP_HPP
#define CORE_POSIX_MKSTEMP_HPP

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

#endif    // CORE_POSIX_MKSTEMP_HPP
