# ── Tidy ─────────────────────────────────────────────────────
# Runs clang-tidy on every compiled source via CMAKE_CXX_CLANG_TIDY.
# Hard abort if clang-tidy is not installed.
# Requires CMAKE_EXPORT_COMPILE_COMMANDS=ON in the root CMakeLists.
# ─────────────────────────────────────────────────────────────

find_program(CLANG_TIDY clang-tidy)
if(NOT CLANG_TIDY)
    if(WIN32)
        set(_hint "Install LLVM: https://github.com/llvm/llvm-project/releases")
    else()
        set(_hint "Install it: apt install clang-tidy")
    endif()
    message(FATAL_ERROR "clang-tidy not found.\n${_hint}")
endif()
message(STATUS "Tidy: ${CLANG_TIDY}")

# Walk the source tree for .clang-tidy so per-directory carve-outs
# under examples/, tests/ are honoured. The root config is still
# picked up by the walk; subdirectory configs use
# `InheritParentConfig: true`. Hardcoding `--config-file=` here
# would defeat the carve-outs, which is why we let clang-tidy
# auto-discover.
set(CMAKE_CXX_CLANG_TIDY
    ${CLANG_TIDY}
    "--header-filter=${CMAKE_CURRENT_SOURCE_DIR}/include/.*"
)
