#ifndef GRAB_VERSION_HPP
#define GRAB_VERSION_HPP

#include <string_view>

// The semantic version is owned by CMake: `project(grab VERSION x.y.z)` in the
// top-level CMakeLists.txt injects the components below via
// target_compile_definitions. The fallbacks apply only to translation units
// built without those definitions, keeping this header self-contained.
#ifndef GRAB_VERSION_MAJOR
    #define GRAB_VERSION_MAJOR 0
#endif
#ifndef GRAB_VERSION_MINOR
    #define GRAB_VERSION_MINOR 0
#endif
#ifndef GRAB_VERSION_PATCH
    #define GRAB_VERSION_PATCH 0
#endif

#define GRAB_VERSION_STRINGIFY_IMPL( value ) #value
#define GRAB_VERSION_STRINGIFY( value )      GRAB_VERSION_STRINGIFY_IMPL( value )
#define GRAB_VERSION_STRING                                                      \
    GRAB_VERSION_STRINGIFY( GRAB_VERSION_MAJOR )                                 \
    "." GRAB_VERSION_STRINGIFY( GRAB_VERSION_MINOR ) "." GRAB_VERSION_STRINGIFY( \
        GRAB_VERSION_PATCH                                                       \
    )

namespace grab
{

    // Numeric components (named to avoid the glibc <sys/sysmacros.h>
    // `major`/`minor` function-like macros).
    inline constexpr int              version_major = GRAB_VERSION_MAJOR;
    inline constexpr int              version_minor = GRAB_VERSION_MINOR;
    inline constexpr int              version_patch = GRAB_VERSION_PATCH;

    // Single source-of-truth semantic version, e.g. "0.0.1", derived from
    // CMake's project version.
    inline constexpr std::string_view version = GRAB_VERSION_STRING;

}    // namespace grab

#endif
