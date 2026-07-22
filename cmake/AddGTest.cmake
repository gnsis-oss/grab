# ── GTest ────────────────────────────────────────────────────
# FetchContent GoogleTest v1.14.0.
# Temporarily disables -Werror and clang-tidy so GTest compiles
# cleanly under strict project settings.
# ─────────────────────────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# ── Suspend project strictness for GTest ──
set(_saved_tidy "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_CLANG_TIDY "")
set(_saved_iwyu "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "")

get_directory_property(_saved_opts COMPILE_OPTIONS)
set(_clean_opts "${_saved_opts}")
list(FILTER _clean_opts EXCLUDE REGEX "-Werror|-Wpedantic")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_clean_opts}")

FetchContent_MakeAvailable(googletest)

# ── Silence GTest's own warnings (e.g. char8_t→char32_t in gtest-printers.h) ──
foreach(_gt gtest gtest_main gmock gmock_main)
    if(TARGET ${_gt})
        target_compile_options(${_gt} PRIVATE -w)
    endif()
endforeach()

# ── Restore project strictness ──
set(CMAKE_CXX_CLANG_TIDY "${_saved_tidy}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${_saved_iwyu}")
set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_saved_opts}")
