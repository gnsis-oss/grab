add_executable(grab_inventory_tests
    driver_test.cpp
    environment_merge_test.cpp
    launch_env_test.cpp
    surface_registry_test.cpp
)
target_link_libraries(grab_inventory_tests PRIVATE grab_inventory grab_input GTest::gtest_main)
target_include_directories(grab_inventory_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(grab_inventory_tests DISCOVERY_MODE PRE_TEST)
