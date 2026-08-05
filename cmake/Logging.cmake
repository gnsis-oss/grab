# ── Logging ──────────────────────────────────────────────────
# Compile-time log ceiling, expressed as a generated C++ constant rather
# than a preprocessor define.
#
#   -DGRAB_LOG_LEVEL=off|nominal|verbose|debug     (default: debug)
#
# The default is the FULL ceiling, in every preset including release, and that
# is deliberate. A diagnostic facility that has to be compiled in before it can
# be used is a facility nobody has when they need it: the first symptom of a
# problem arrives on a machine running whatever binary is already there, and
# "rebuild with logging on and reproduce it" is not an answer. Levels above the
# ceiling cost nothing at runtime, but they also cannot be turned on at
# runtime, which is the wrong trade for everything except a measured hot loop.
#
# `off` remains selectable for a consumer who wants the code gone entirely.
# Nothing in grab's own presets selects it.
#
# The level reaches the code as `grab::log::compileLevel`, an
# `inline constexpr int` in a per-build-directory generated header. Callers
# gate on it with `if constexpr`; nothing in grab's sources expands a
# function-like macro to log.
#
# This is only the CEILING. What actually emits at runtime is a separate,
# independently settable level (GRAB_LOG / --log-level), defaulting to off.
# ─────────────────────────────────────────────────────────────

set(GRAB_LOG_LEVEL "debug" CACHE STRING
    "Compile-time log ceiling: off | nominal | verbose | debug")
set_property(CACHE GRAB_LOG_LEVEL PROPERTY STRINGS off nominal verbose debug)

if(GRAB_LOG_LEVEL STREQUAL "off")
    set(GRAB_LOG_COMPILE_LEVEL 0)
elseif(GRAB_LOG_LEVEL STREQUAL "nominal")
    set(GRAB_LOG_COMPILE_LEVEL 1)
elseif(GRAB_LOG_LEVEL STREQUAL "verbose")
    set(GRAB_LOG_COMPILE_LEVEL 2)
elseif(GRAB_LOG_LEVEL STREQUAL "debug")
    set(GRAB_LOG_COMPILE_LEVEL 3)
else()
    message(FATAL_ERROR
        "GRAB_LOG_LEVEL is '${GRAB_LOG_LEVEL}'.\n"
        "Legal values: off nominal verbose debug")
endif()

set(GRAB_LOG_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/grab_log_config.hpp.in
    ${GRAB_LOG_CONFIG_DIR}/kernel/support/log_config.hpp
    @ONLY
)

# Carried to every target through grab_core, which every other grab target
# links PUBLIC (directly or transitively).
add_library(grab_log_config INTERFACE)
target_include_directories(grab_log_config SYSTEM INTERFACE ${GRAB_LOG_CONFIG_DIR})

message(STATUS "Logging: compile ceiling ${GRAB_LOG_LEVEL} (${GRAB_LOG_COMPILE_LEVEL})")
