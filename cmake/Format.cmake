# ── Format ───────────────────────────────────────────────────
# Runs clang-format on every .c .h .cpp .hpp on every build.
# Hard abort if clang-format is not installed.
# ─────────────────────────────────────────────────────────────

find_program(CLANG_FORMAT clang-format)
if(NOT CLANG_FORMAT)
    if(WIN32)
        set(_hint "Install LLVM: https://github.com/llvm/llvm-project/releases")
    else()
        set(_hint "Install it: apt install clang-format")
    endif()
    message(FATAL_ERROR "clang-format not found.\n${_hint}")
endif()
message(STATUS "Format: ${CLANG_FORMAT}")

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
list(FILTER FORMAT_SOURCES EXCLUDE REGEX "/src/vendor/")

if(FORMAT_SOURCES)
    add_custom_target(format ALL
        COMMAND ${CLANG_FORMAT} -style=file:${CMAKE_CURRENT_SOURCE_DIR}/.clang-format -i ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "clang-format: formatting ${CMAKE_PROJECT_NAME} sources"
        VERBATIM
    )
endif()
