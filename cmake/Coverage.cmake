# ── Coverage ─────────────────────────────────────────────────
# Always enabled. Generates gcov-compatible coverage data.
# ─────────────────────────────────────────────────────────────

if(MEMORY_SANITIZE)
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
