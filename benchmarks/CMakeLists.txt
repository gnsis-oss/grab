# Benchmarks are not tests: they assert nothing and gate nothing. They exist so
# a performance claim about the overlay can be reproduced with one command
# instead of re-derived from a live session that nobody else can observe.
add_executable(grab_overlay_raster_bench overlay_raster_bench.cpp)
target_link_libraries(grab_overlay_raster_bench PRIVATE grab_kernel grab_core)
target_include_directories(grab_overlay_raster_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(grab_overlay_raster_bench PRIVATE cxx_std_23)
