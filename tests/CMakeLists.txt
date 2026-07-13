add_test(NAME grab_invariant_checks
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_invariants.sh)

add_executable(grab_core_tests
    ${CMAKE_SOURCE_DIR}/src/cli/input_command.cpp
    cli/input_command_test.cpp
    core/test_capability.cpp
    core/test_context.cpp
    core/test_enum_table.cpp
    core/test_log.cpp
    core/test_cli_doctor.cpp
    geometry/test_point.cpp
    geometry/test_size.cpp
    geometry/test_rectangle.cpp
    geometry/test_curve.cpp
    core/test_doctor.cpp
    core/test_event.cpp
    core/test_event_bus.cpp
    core/test_ids.cpp
    core/test_prober.cpp
    core/test_permission.cpp
    core/test_process_ref.cpp
    core/test_monitor.cpp
    core/test_registry.cpp
    core/test_reactor.cpp
    core/test_resolver.cpp
    core/test_result.cpp
    core/test_result_taxonomy.cpp
    core/test_session.cpp
    core/test_space_graph.cpp
    core/test_trace_enums.cpp
    core/test_vendor_l0.cpp
    core/test_version.cpp
    codec/test_image.cpp
    codec/test_png.cpp
    screen/test_enumerate.cpp
    screen/test_screen.cpp
    screen/test_virtual_display.cpp
    screen/test_workflow.cpp
    screen/test_x11_capture.cpp
    screen/test_record.cpp
    image/test_compare.cpp
    input/test_keymap.cpp
    input/test_gestures.cpp
    input/test_input.cpp
    event/test_browser_bridge.cpp
    event/test_source_registry.cpp
    event/test_monitor_source.cpp
    event/test_platform_factory.cpp
    event/test_event_descriptor.cpp
    event/test_payload_fields.cpp
    event/test_atspi.cpp
    event/test_state.cpp
    event/test_state_source.cpp
    event/test_evdev.cpp
    event/test_xinput2.cpp
    event/test_window_x11.cpp
    notify/test_notifier.cpp
    input/test_locator.cpp
    input/test_seat.cpp
    transport/test_codec.cpp
    storage/test_jsonl_sink.cpp
    service/test_daemon.cpp
    transport/test_service.cpp
    event/test_browser_classifier.cpp
)
target_link_libraries(grab_core_tests PRIVATE grab_core grab_vendor_l0 grab_codec grab_screen grab_image grab_event grab_input grab_notify grab_proto grab_transport grab_storage grab_service ${GRAB_LIBAV_TARGETS} GTest::gtest_main)
target_include_directories(grab_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(grab_core_tests PRIVATE GRAB_CODEC_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/codec/pngs")
include(GoogleTest)

# A7: self-provision the Xvfb displays the X-backed suites use so `ctest` is
# self-contained (no manually-started display). Opt-out with -DGRAB_TEST_PROVISION_XVFB=OFF.
option(GRAB_TEST_PROVISION_XVFB
    "Auto-provision Xvfb displays for X-backed tests via a CTest fixture" ON)

if(GRAB_TEST_PROVISION_XVFB)
    gtest_discover_tests(grab_core_tests TEST_FILTER "Compare.*:Diff.*:Workflow.CompareFilesDetectsDifference")
    gtest_discover_tests(grab_core_tests
        TEST_FILTER "-Compare.*:Diff.*:Workflow.CompareFilesDetectsDifference"
        PROPERTIES FIXTURES_REQUIRED grab_xvfb ENVIRONMENT "DISPLAY=:87")
    add_test(NAME grab_xvfb_setup
        COMMAND ${CMAKE_COMMAND} -E env XVFB_PIDFILE=${CMAKE_BINARY_DIR}/grab-xvfb.pids
                sh ${CMAKE_CURRENT_SOURCE_DIR}/xvfb_fixture.sh start)
    set_tests_properties(grab_xvfb_setup PROPERTIES FIXTURES_SETUP grab_xvfb)
    add_test(NAME grab_xvfb_cleanup
        COMMAND ${CMAKE_COMMAND} -E env XVFB_PIDFILE=${CMAKE_BINARY_DIR}/grab-xvfb.pids
                sh ${CMAKE_CURRENT_SOURCE_DIR}/xvfb_fixture.sh stop)
    set_tests_properties(grab_xvfb_cleanup PROPERTIES FIXTURES_CLEANUP grab_xvfb)
else()
    gtest_discover_tests(grab_core_tests)
endif()

add_executable(grab_session_tests
    session/fake_session_provider_test.cpp
    session/manager_test.cpp
    session/registry_test.cpp
    session/record_test.cpp
    session/selection_test.cpp
    session/state_machine_test.cpp
    session/test_workspace_alias.cpp
    session/types_test.cpp
)
target_link_libraries(grab_session_tests PRIVATE grab_session GTest::gtest_main)
target_include_directories(grab_session_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/session)
gtest_discover_tests(grab_session_tests)

add_executable(grab_x11_tests
    x11/xcb_reply_test.cpp
    x11/xkb_keymap_test.cpp
    x11/xi_seat_test.cpp
    session/x11_seat_provider_test.cpp
    session/builtin_session_providers_test.cpp
)
target_link_libraries(grab_x11_tests PRIVATE grab_core grab_platform_x11 grab_input_x11 grab_session GTest::gtest_main)
target_include_directories(grab_x11_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR})

if(GRAB_TEST_PROVISION_XVFB)
    gtest_discover_tests(grab_x11_tests
        PROPERTIES FIXTURES_REQUIRED grab_xvfb ENVIRONMENT "DISPLAY=:87")
else()
    gtest_discover_tests(grab_x11_tests)
endif()
