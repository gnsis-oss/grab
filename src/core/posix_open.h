#ifndef CORE_POSIX_OPEN_H
#define CORE_POSIX_OPEN_H

#ifdef __cplusplus
extern "C"
{
#endif

    int
    grab_open_read_probe(
        const char* path
    ); /* open(path, O_RDONLY|O_NONBLOCK|O_CLOEXEC); close; 1 ok, 0 fail */
    int
    grab_open_write_probe(
        const char* path
    ); /* open(path, O_WRONLY|O_NONBLOCK|O_CLOEXEC); close; 1 ok, 0 fail */
    int
    grab_fsync_dir(
        const char* path
    ); /* open(path, O_RDONLY|O_DIRECTORY|O_CLOEXEC); fsync; close; 1 ok, 0 fail */

#ifdef __cplusplus
}
#endif

#endif /* CORE_POSIX_OPEN_H */
