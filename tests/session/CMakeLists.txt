add_executable(grab_session_tests
    fake_session_provider_test.cpp
    manager_test.cpp
    registry_test.cpp
    record_test.cpp
    selection_test.cpp
    state_machine_test.cpp
    types_test.cpp
)
target_link_libraries(grab_session_tests PRIVATE grab_session GTest::gtest_main)
target_include_directories(grab_session_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(grab_session_tests DISCOVERY_MODE PRE_TEST)
