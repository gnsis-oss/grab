# ── Format ───────────────────────────────────────────────────
# Runs clang-format over every .c .h .cpp .hpp in the tree.
#
#   -DGRAB_FORMAT=ON    `format` joins ALL — a plain `ninja` rewrites sources
#   -DGRAB_FORMAT=OFF   `format` is on-demand only              (default)
#
# OFF by default because there is now more than one build directory, and an
# ALL target that rewrites the shared source tree means several ninja
# processes editing the same files at once. Only the `dev` preset turns it
# on. `format-check` is always available and never writes.
# ─────────────────────────────────────────────────────────────

option(GRAB_FORMAT "Run clang-format as part of the default build" OFF)

find_program(CLANG_FORMAT clang-format)
if(NOT CLANG_FORMAT)
    if(WIN32)
        set(_hint "Install LLVM: https://github.com/llvm/llvm-project/releases")
    else()
        set(_hint "Install it: apt install clang-format")
    endif()
    message(FATAL_ERROR "clang-format not found.\n${_hint}")
endif()

file(GLOB_RECURSE FORMAT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/*.c
)

if(FORMAT_SOURCES)
    if(GRAB_FORMAT)
        set(_format_all ALL)
        message(STATUS "Format: ${CLANG_FORMAT} (rewrites sources on every build)")
    else()
        set(_format_all "")
        message(STATUS "Format: ${CLANG_FORMAT} (on demand — `ninja format`)")
    endif()

    add_custom_target(format ${_format_all}
        COMMAND ${CLANG_FORMAT} -style=file:${CMAKE_CURRENT_SOURCE_DIR}/.clang-format -i ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "clang-format: formatting ${CMAKE_PROJECT_NAME} sources"
        VERBATIM
    )

    # Never writes. Fails on the first file that would change, so a review or
    # CI check can assert formatting without mutating the tree.
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} -style=file:${CMAKE_CURRENT_SOURCE_DIR}/.clang-format
                --dry-run -Werror ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "clang-format: checking ${CMAKE_PROJECT_NAME} sources"
        VERBATIM
    )
endif()
