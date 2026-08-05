# ── Sanitizers ───────────────────────────────────────────────
# Instrumentation is an EXPLICIT choice, independent of CMAKE_BUILD_TYPE.
# A sanitizer used to be inferred from the build type, which made
# "optimized + ASan" (the useful crash-hunting build) and "debug without
# sanitizers" (the useful stepping build) both inexpressible.
#
#   -DGRAB_SANITIZER=none      no instrumentation           (default)
#   -DGRAB_SANITIZER=address   ASan + UBSan
#   -DGRAB_SANITIZER=thread    TSan + UBSan
#   -DGRAB_SANITIZER=memory    MSan + UBSan
#
# UBSan rides along with every non-none value. ASan/TSan/MSan are mutually
# exclusive by construction — the enum cannot express two at once.
#
# MSVC: ASan only (/fsanitize=address). No UBSan/TSan/MSan support.
# ─────────────────────────────────────────────────────────────

set(GRAB_SANITIZER "none" CACHE STRING
    "Sanitizer to build with: none | address | thread | memory")
set_property(CACHE GRAB_SANITIZER PROPERTY STRINGS none address thread memory)

# ── Deprecated boolean spellings ────────────────────────────
# Accepted for one release so existing invocations keep working.
option(THREAD_SANITIZE "Deprecated: use -DGRAB_SANITIZER=thread" OFF)
option(MEMORY_SANITIZE "Deprecated: use -DGRAB_SANITIZER=memory" OFF)

if(THREAD_SANITIZE AND MEMORY_SANITIZE)
    message(FATAL_ERROR
        "THREAD_SANITIZE and MEMORY_SANITIZE cannot both be ON.\n"
        "TSan and MSan use incompatible shadow memory layouts.\n"
        "Both are deprecated: use -DGRAB_SANITIZER=thread or =memory.")
endif()

if(THREAD_SANITIZE AND GRAB_SANITIZER STREQUAL "none")
    message(DEPRECATION "THREAD_SANITIZE is deprecated; use -DGRAB_SANITIZER=thread")
    set(GRAB_SANITIZER "thread")
endif()
if(MEMORY_SANITIZE AND GRAB_SANITIZER STREQUAL "none")
    message(DEPRECATION "MEMORY_SANITIZE is deprecated; use -DGRAB_SANITIZER=memory")
    set(GRAB_SANITIZER "memory")
endif()

# ── Validate ────────────────────────────────────────────────

set(_grab_sanitizer_values none address thread memory)
if(NOT GRAB_SANITIZER IN_LIST _grab_sanitizer_values)
    message(FATAL_ERROR
        "GRAB_SANITIZER is '${GRAB_SANITIZER}'.\n"
        "Legal values: ${_grab_sanitizer_values}")
endif()

# ── Frame pointers ──────────────────────────────────────────
# Useful on their own for perf; mandatory under a sanitizer, whose stack
# traces are unusable without them.

option(GRAB_FRAME_POINTERS "Build with -fno-omit-frame-pointer" OFF)
if(NOT GRAB_SANITIZER STREQUAL "none")
    set(GRAB_FRAME_POINTERS ON)
endif()

if(GRAB_FRAME_POINTERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fno-omit-frame-pointer)
endif()

# ── Apply ───────────────────────────────────────────────────

if(GRAB_SANITIZER STREQUAL "none")
    message(STATUS "Sanitizers: none")
    return()
endif()

if(MSVC)
    if(NOT GRAB_SANITIZER STREQUAL "address")
        message(WARNING "MSVC supports ASan only. Ignoring GRAB_SANITIZER=${GRAB_SANITIZER}.")
    endif()
    add_compile_options(/fsanitize=address)
    message(STATUS "Sanitizers: ASan (MSVC)")
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
        "GRAB_SANITIZER=${GRAB_SANITIZER} but ${CMAKE_CXX_COMPILER_ID} is unsupported.\n"
        "Configure with -DGRAB_SANITIZER=none.")
endif()

# UBSan is always on — compatible with all three.
set(SANITIZE_FLAGS "-fsanitize=undefined,${GRAB_SANITIZER}")
add_compile_options(${SANITIZE_FLAGS})
add_link_options(${SANITIZE_FLAGS})
message(STATUS "Sanitizers: ${GRAB_SANITIZER} + UBSan")

# ── MSan-instrumented libc++ ────────────────────────────────────────
# MSan reports thousands of false positives against an uninstrumented
# standard library. Point at the MSan-instrumented libc++ built into the
# CI image at /opt/llvm-msan.
# See .gitlab/ci-image.Dockerfile (Phase 3) for the libc++ build.
if(GRAB_SANITIZER STREQUAL "memory" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-fsanitize-memory-track-origins
                        -stdlib=libc++ -nostdinc++ -Wno-unused-command-line-argument
                        -isystem /opt/llvm-msan/include/c++/v1)
    add_link_options(-stdlib=libc++
                     -L/opt/llvm-msan/lib
                     -Wl,-rpath,/opt/llvm-msan/lib
                     -lc++ -lc++abi -lunwind)
endif()
