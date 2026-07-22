# ── Coverage ─────────────────────────────────────────────────
# Always enabled in dev builds. Generates gcov-compatible coverage data.
# Release/MinSizeRel builds skip it (same gate as Sanitizers.cmake): the
# arc counters dominate per-pixel loops — the overlay raster drops from
# ~1 ms to >100 ms per 3200x2000 frame when instrumented.
# ─────────────────────────────────────────────────────────────

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    message(STATUS "Coverage: skipped (${CMAKE_BUILD_TYPE} build)")
elseif(MEMORY_SANITIZE)
    # libgcov writes .gcda files with uninitialised bytes that MSan
    # flags as fwrite errors; disable coverage under MSan instrumentation.
    message(STATUS "Coverage: skipped (MEMORY_SANITIZE=ON)")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(--coverage)
    add_link_options(--coverage)
    message(STATUS "Coverage: enabled (--coverage)")
else()
    message(STATUS "Coverage: skipped (MSVC)")
endif()
