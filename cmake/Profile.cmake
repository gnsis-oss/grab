# ── Profile ──────────────────────────────────────────────────
# perf, valgrind, heaptrack — OFF by default. Linux only.
# Enable with -DENABLE_PERF=ON, -DENABLE_VALGRIND=ON, -DENABLE_HEAPTRACK=ON.
# Hard abort if enabled but tool not found.
#
# Usage (after build):
#   ninja perf_record PROFILE_TARGET=my_binary
#   ninja perf_flamegraph
#   ninja valgrind_memcheck PROFILE_TARGET=my_binary
#   ninja valgrind_callgrind PROFILE_TARGET=my_binary
#   ninja heaptrack_run PROFILE_TARGET=my_binary
# ─────────────────────────────────────────────────────────────

option(ENABLE_PERF "Enable perf profiling targets (Linux only)" OFF)
option(ENABLE_VALGRIND "Enable valgrind targets (Linux only)" OFF)
option(ENABLE_HEAPTRACK "Enable heaptrack targets (Linux only)" OFF)

set(PROFILE_TARGET "" CACHE STRING "Binary to profile (path or target name)")

# ── perf ──

if(ENABLE_PERF)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ENABLE_PERF is ON but perf is Linux only.")
    endif()

    find_program(PERF perf)
    if(NOT PERF)
        message(FATAL_ERROR
            "ENABLE_PERF is ON but perf not found.\n"
            "Install it: apt install linux-tools-$(uname -r)")
    endif()

    find_program(STACKCOLLAPSE stackcollapse-perf.pl)
    find_program(FLAMEGRAPH flamegraph.pl)
    if(NOT STACKCOLLAPSE OR NOT FLAMEGRAPH)
        message(WARNING
            "FlameGraph scripts not found in PATH.\n"
            "perf_flamegraph target will not work.\n"
            "Get them: git clone https://github.com/brendangregg/FlameGraph")
    endif()

    message(STATUS "Profile: perf enabled (${PERF})")

    add_custom_target(perf_record
        COMMAND ${PERF} record -g --call-graph dwarf -o perf.data ${PROFILE_TARGET}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "perf: recording ${PROFILE_TARGET}"
        VERBATIM
    )

    # flamegraph uses pipes and redirects — must go through shell
    add_custom_target(perf_flamegraph
        COMMAND sh -c "${PERF} script -i perf.data | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "perf: generating flamegraph.svg"
    )
endif()

# ── valgrind ──

if(ENABLE_VALGRIND)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ENABLE_VALGRIND is ON but valgrind is Linux only.")
    endif()

    find_program(VALGRIND valgrind)
    if(NOT VALGRIND)
        message(FATAL_ERROR
            "ENABLE_VALGRIND is ON but valgrind not found.\n"
            "Install it: apt install valgrind")
    endif()
    message(STATUS "Profile: valgrind enabled (${VALGRIND})")

    add_custom_target(valgrind_memcheck
        COMMAND ${VALGRIND}
            --tool=memcheck
            --leak-check=full
            --show-leak-kinds=all
            --track-origins=yes
            ${PROFILE_TARGET}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "valgrind: memcheck ${PROFILE_TARGET}"
        VERBATIM
    )

    add_custom_target(valgrind_callgrind
        COMMAND ${VALGRIND}
            --tool=callgrind
            --callgrind-out-file=callgrind.out
            ${PROFILE_TARGET}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "valgrind: callgrind ${PROFILE_TARGET} -> callgrind.out"
        VERBATIM
    )
endif()

# ── heaptrack ──

if(ENABLE_HEAPTRACK)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "ENABLE_HEAPTRACK is ON but heaptrack is Linux only.")
    endif()

    find_program(HEAPTRACK heaptrack)
    if(NOT HEAPTRACK)
        message(FATAL_ERROR
            "ENABLE_HEAPTRACK is ON but heaptrack not found.\n"
            "Install it: apt install heaptrack")
    endif()
    message(STATUS "Profile: heaptrack enabled (${HEAPTRACK})")

    add_custom_target(heaptrack_run
        COMMAND ${HEAPTRACK} ${PROFILE_TARGET}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "heaptrack: profiling ${PROFILE_TARGET}"
        VERBATIM
    )
endif()
