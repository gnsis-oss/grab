# ── Cache ────────────────────────────────────────────────────
# Uses ccache to speed up rebuilds. Enabled automatically when
# ccache is found. No cmake option needed — zero cost if absent.
# ─────────────────────────────────────────────────────────────

find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE})
    message(STATUS "Cache: ccache enabled (${CCACHE})")
else()
    message(STATUS "Cache: ccache not found (builds will be slower)")
endif()
