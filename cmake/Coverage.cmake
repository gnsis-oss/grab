# ── Coverage ─────────────────────────────────────────────────
# OFF by default, and independent of CMAKE_BUILD_TYPE.
#
#   -DGRAB_COVERAGE=ON   gcov-compatible arc instrumentation
#
# Coverage used to be on in every non-Release build. It is very expensive:
# the arc counters dominate per-pixel loops — the overlay raster goes from
# ~1 ms to >100 ms per 3200x2000 frame — which is fatal against a 16.7 ms
# frame budget. Measure coverage in the `coverage` preset; build everything
# else without it.
# ─────────────────────────────────────────────────────────────

option(GRAB_COVERAGE "Build with gcov arc instrumentation (--coverage)" OFF)

if(NOT GRAB_COVERAGE)
    message(STATUS "Coverage: off")
    return()
endif()

# libgcov writes .gcda files with uninitialised bytes that MSan flags as
# fwrite errors. This used to be a silent skip, which reported success
# while producing no coverage data at all.
if(GRAB_SANITIZER STREQUAL "memory")
    message(FATAL_ERROR
        "GRAB_COVERAGE=ON is incompatible with GRAB_SANITIZER=memory.\n"
        "libgcov writes uninitialised bytes that MSan reports as errors.\n"
        "Use the `coverage` preset (no sanitizer) or the `msan` preset (no coverage).")
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR
        "GRAB_COVERAGE=ON but ${CMAKE_CXX_COMPILER_ID} has no --coverage support.")
endif()

add_compile_options(--coverage)
add_link_options(--coverage)
message(STATUS "Coverage: enabled (--coverage)")
