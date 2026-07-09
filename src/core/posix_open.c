#include "core/posix_open.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* open(2)'s prototype is variadic, so the exact-flag helpers live in a C
 * translation unit with fixed-prototype exports. Use O_WRONLY, not O_RDWR, so
 * write-only paths answer correctly; never use O_CREAT or O_TRUNC.
 */

static int
grab_open_probe( const char* path,
                 int         flags )
{
    const int fd = open( path, flags );
    if( fd < 0 )
    {
        return 0;
    }

    close( fd );
    return 1;
}

int
grab_open_read_probe( const char* path )
{
    return grab_open_probe( path, O_RDONLY | O_NONBLOCK | O_CLOEXEC );
}

int
grab_open_write_probe( const char* path )
{
    return grab_open_probe( path, O_WRONLY | O_NONBLOCK | O_CLOEXEC );
}

int
grab_fsync_dir( const char* path )
{
    const int fd = open( path, O_RDONLY | O_DIRECTORY | O_CLOEXEC );
    if( fd < 0 )
    {
        return 0;
    }

    if( fsync( fd ) < 0 )
    {
        const int error_number = errno;
        close( fd );
        errno = error_number;
        return 0;
    }

    if( close( fd ) < 0 )
    {
        return 0;
    }

    return 1;
}
