# ── Include What You Use ─────────────────────────────────────
# Opt-in: -DWITH_IWYU=ON
# If not found on PATH, clones and builds from source at
# configure time (clang_22 branch, linked against system LLVM).
#
# Requires: libclang-dev matching the compiler
#   sudo apt install libclang-23-dev
#
# IWYU and clang-tidy's misc-include-cleaner may overlap.
# When they disagree, prefer IWYU and silence clang-tidy with
# // IWYU pragma: keep
# ─────────────────────────────────────────────────────────────

option(WITH_IWYU "Run include-what-you-use on every compiled source" OFF)

if(WITH_IWYU)
    find_program(IWYU_BIN include-what-you-use)

    if(NOT IWYU_BIN)
        message(STATUS "IWYU: not found on PATH — building from source (clang_22 branch)")

        # Detect LLVM major version from the compiler
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} --version
            OUTPUT_VARIABLE _clang_ver OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[0-9]+" _llvm_major "${_clang_ver}")

        # Pre-check: IWYU links against clang static libs
        set(_llvm_prefix "/usr/lib/llvm-${_llvm_major}")
        if(NOT EXISTS "${_llvm_prefix}/lib/libclangBasic.a")
            message(FATAL_ERROR
                "Cannot build IWYU: Clang static libraries not found.\n"
                "Install: sudo apt install libclang-${_llvm_major}-dev")
        endif()

        set(_iwyu_src "${CMAKE_CURRENT_BINARY_DIR}/_iwyu_src")
        set(_iwyu_build "${CMAKE_CURRENT_BINARY_DIR}/_iwyu_build")
        set(_iwyu_install "${CMAKE_CURRENT_BINARY_DIR}/_iwyu_install")

        # Clone (only if not already cloned)
        if(NOT EXISTS "${_iwyu_src}/CMakeLists.txt")
            message(STATUS "IWYU: cloning clang_22 branch...")
            execute_process(
                COMMAND git clone --depth 1 -b clang_22
                    https://github.com/include-what-you-use/include-what-you-use.git
                    ${_iwyu_src}
                RESULT_VARIABLE _clone_result)
            if(NOT _clone_result EQUAL 0)
                message(FATAL_ERROR "IWYU: git clone failed")
            endif()
        endif()

        # Configure (only if not already configured)
        if(NOT EXISTS "${_iwyu_build}/CMakeCache.txt")
            message(STATUS "IWYU: configuring...")
            execute_process(
                COMMAND ${CMAKE_COMMAND}
                    -S ${_iwyu_src}
                    -B ${_iwyu_build}
                    -G Ninja
                    -DCMAKE_PREFIX_PATH=${_llvm_prefix}
                    -DCMAKE_INSTALL_PREFIX=${_iwyu_install}
                    -DCMAKE_BUILD_TYPE=Release
                    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                RESULT_VARIABLE _cfg_result)
            if(NOT _cfg_result EQUAL 0)
                message(FATAL_ERROR "IWYU: cmake configure failed")
            endif()
        endif()

        # Build + install (only if binary doesn't exist yet)
        if(NOT EXISTS "${_iwyu_install}/bin/include-what-you-use")
            message(STATUS "IWYU: building...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${_iwyu_build} -j
                RESULT_VARIABLE _build_result)
            if(NOT _build_result EQUAL 0)
                message(FATAL_ERROR "IWYU: build failed")
            endif()
            execute_process(
                COMMAND ${CMAKE_COMMAND} --install ${_iwyu_build}
                RESULT_VARIABLE _install_result)
            if(NOT _install_result EQUAL 0)
                message(FATAL_ERROR "IWYU: install failed")
            endif()
        endif()

        set(IWYU_BIN "${_iwyu_install}/bin/include-what-you-use")
    endif()

    message(STATUS "IWYU: ${IWYU_BIN}")

    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
        ${IWYU_BIN}
        -Xiwyu --no_fwd_decls
        -Xiwyu --cxx17ns
        -Xiwyu --mapping_file=${CMAKE_CURRENT_LIST_DIR}/iwyu-gtest.imp
    )
endif()
