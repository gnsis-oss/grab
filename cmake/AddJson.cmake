# ── nlohmann/json ────────────────────────────────────────────
# FetchContent nlohmann/json v3.11.3 (header-only). Provides the
# nlohmann_json::nlohmann_json interface target used for all JSON
# serialization and parsing in grab.
# ─────────────────────────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

# Keep the dependency lean and out of the project's strict lanes.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")

set(_saved_tidy "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_CLANG_TIDY "")
set(_saved_iwyu "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "")

FetchContent_MakeAvailable(nlohmann_json)

set(CMAKE_CXX_CLANG_TIDY "${_saved_tidy}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${_saved_iwyu}")
