add_executable(mouse_snake_trail mouse_snake_trail.cpp)
target_link_libraries(mouse_snake_trail PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_compile_features(mouse_snake_trail PRIVATE cxx_std_23)

# The visual half of examples/sequences/osu_stress.json. It parses that same
# document (nlohmann/json arrives transitively as a PUBLIC dependency of
# grab_core) so no coordinate is duplicated between the two halves.
add_executable(osu_stress osu_stress.cpp)
target_link_libraries(osu_stress PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_compile_features(osu_stress PRIVATE cxx_std_23)
target_compile_definitions(osu_stress PRIVATE
    OSU_STRESS_DEFAULT_DOCUMENT="${CMAKE_CURRENT_SOURCE_DIR}/sequences/osu_stress.json")

add_executable(event_logger event_logger.cpp)
target_link_libraries(event_logger PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_driver_x11
    grab_driver_webextension
    grab_storage)
target_compile_features(event_logger PRIVATE cxx_std_23)
