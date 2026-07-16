add_test(NAME grab_invariant_checks
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_invariants.sh)

add_test(NAME grab_legacy_ratchet
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_legacy_ratchet.sh)

add_executable(grab_core_tests
    ${CMAKE_SOURCE_DIR}/src/cli/input_command.cpp
    cli/input_command_test.cpp
    cli/command_table_test.cpp
    core/test_capability.cpp
    core/test_command_descriptor.cpp
    core/test_capture.cpp
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
    core/test_phase0_gate.cpp
    core/test_result.cpp
    core/test_result_taxonomy.cpp
    core/test_session.cpp
    core/test_space_graph.cpp
    core/test_trace_enums.cpp
    core/test_vendor_l0.cpp
    core/test_version.cpp
    codec/test_image.cpp
    codec/test_png.cpp
    screen/test_screen.cpp
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
    transport/test_service_hardening.cpp
    event/test_browser_classifier.cpp
)
target_link_libraries(grab_core_tests PRIVATE grab_core grab_vendor_l0 grab_codec grab_screen grab_image grab_event grab_input grab_notify grab_proto grab_transport grab_storage grab_service ${GRAB_LIBAV_TARGETS} GTest::gtest_main)
target_include_directories(grab_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(grab_core_tests PRIVATE GRAB_CODEC_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/codec/pngs")
include(GoogleTest)

add_executable(grab_client_tests
    client/test_client.cpp
)
target_link_libraries(grab_client_tests PRIVATE grab_client grab_kernel grab_screen grab_transport GTest::gtest_main)
target_include_directories(grab_client_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

add_executable(grab_kernel_tests
    kernel/action/test_transaction.cpp
    kernel/action/test_polling_event_source.cpp
    kernel/action/test_wait_engine.cpp
    kernel/capture/test_damage_hint_auditor.cpp
    kernel/capture/test_inject_gate.cpp
    kernel/capture/test_pacing_governor.cpp
    kernel/capture/test_tile_differ.cpp
    kernel/lifecycle/test_session_core.cpp
    kernel/lifecycle/test_session_verbs.cpp
    kernel/test_ui.cpp
    kernel/test_tree_store.cpp
    kernel/test_target_registry.cpp
    kernel/test_fake_runtime.cpp
    query/test_query.cpp
)
target_link_libraries(grab_kernel_tests PRIVATE grab_kernel GTest::gtest_main)
target_include_directories(grab_kernel_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
gtest_discover_tests(grab_kernel_tests)

add_executable(grab_driver_x11_tests
    drivers/x11/test_coordinate_authority.cpp
    drivers/x11/test_enumerate.cpp
    drivers/x11/test_record.cpp
    drivers/x11/test_virtual_display.cpp
    drivers/x11/test_workflow.cpp
    drivers/x11/test_x11_capture.cpp
    drivers/x11/test_x11_capture_route.cpp
    drivers/x11/test_x11_event_source.cpp
    drivers/x11/test_x11_input_correctness.cpp
    drivers/x11/test_x11_runtime.cpp
    drivers/x11/test_x11_tree_source.cpp
)
target_link_libraries(grab_driver_x11_tests PRIVATE
    grab_driver_x11
    grab_vendor_l0
    grab_notify
    ${GRAB_LIBAV_TARGETS}
    GTest::gtest_main
)
target_include_directories(grab_driver_x11_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

add_executable(grab_integration_tests
    integration/test_exit_gate.cpp
)
target_link_libraries(grab_integration_tests PRIVATE
    grab_client
    grab_kernel
    grab_driver_x11
    grab_transport
    GTest::gtest_main
)
target_include_directories(grab_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

add_executable(grab_driver_atspi_tests
    drivers/atspi/test_atspi_mapping.cpp
    drivers/atspi/test_atspi_alias.cpp
    drivers/atspi/test_atspi_runtime.cpp
)
target_link_libraries(grab_driver_atspi_tests PRIVATE
    grab_driver_atspi
    grab_driver_x11
    grab_vendor_l0
    GTest::gtest_main
)
target_include_directories(grab_driver_atspi_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
gtest_discover_tests(grab_driver_atspi_tests DISCOVERY_MODE PRE_TEST)

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
    gtest_discover_tests(grab_integration_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES FIXTURES_REQUIRED grab_xvfb ENVIRONMENT "DISPLAY=:87")
    gtest_discover_tests(grab_driver_x11_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES FIXTURES_REQUIRED grab_xvfb ENVIRONMENT "DISPLAY=:87")
    gtest_discover_tests(grab_client_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES FIXTURES_REQUIRED grab_xvfb ENVIRONMENT "DISPLAY=:87")
else()
    gtest_discover_tests(grab_core_tests)
    gtest_discover_tests(grab_integration_tests DISCOVERY_MODE PRE_TEST)
    gtest_discover_tests(grab_driver_x11_tests DISCOVERY_MODE PRE_TEST)
    gtest_discover_tests(grab_client_tests DISCOVERY_MODE PRE_TEST)
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
