# ── Warnings ─────────────────────────────────────────────────
# Hard-enforced compiler warnings. Applied globally.
# ─────────────────────────────────────────────────────────────

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wconversion
        -Wshadow
        -Wpedantic
        -Werror
    )
elseif(MSVC)
    add_compile_options(/W4 /WX)
endif()
