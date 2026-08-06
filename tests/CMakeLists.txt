add_test(NAME grab_invariant_checks
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_invariants.sh)

add_test(NAME grab_public_header_allowlist
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_public_headers.sh)

target_sources(grab_kernel PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel/presentation/overlay_animation.cpp)

add_executable(grab_core_tests
    ${CMAKE_SOURCE_DIR}/src/frontends/cli/feedback_command.cpp
    ${CMAKE_SOURCE_DIR}/src/frontends/cli/input_command.cpp
    ${CMAKE_SOURCE_DIR}/src/frontends/cli/overlay_command.cpp
    ${CMAKE_SOURCE_DIR}/src/frontends/cli/sketch_command.cpp
    ${CMAKE_SOURCE_DIR}/src/frontends/cli/watch_daemon.cpp
    ${CMAKE_SOURCE_DIR}/src/frontends/cli/windows_command.cpp
    cli/feedback_command_test.cpp
    cli/input_command_test.cpp
    cli/overlay_command_test.cpp
    cli/sketch_command_test.cpp
    cli/watch_daemon_test.cpp
    cli/windows_command_test.cpp
    cli/command_table_test.cpp
    config/test_batch_manifest.cpp
    config/test_environment.cpp
    config/test_load.cpp
    config/test_pattern.cpp
    config/test_profiles.cpp
    config/test_rotation.cpp
    config/test_schedule.cpp
    core/test_capability.cpp
    core/test_command_descriptor.cpp
    core/test_capture.cpp
    core/test_context.cpp
    core/test_enum_table.cpp
    core/test_diag.cpp
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
    core/test_process_wait.cpp
    core/test_monitor.cpp
    core/test_overlay_types.cpp
    core/test_registry.cpp
    core/test_reactor.cpp
    core/test_resolver.cpp
    core/test_phase0_gate.cpp
    core/test_result.cpp
    core/test_result_taxonomy.cpp
    core/test_session.cpp
    core/test_space_graph.cpp
    core/test_adjacency_graph.cpp
    core/test_trace_enums.cpp
    core/test_version.cpp
    codec/test_image.cpp
    codec/test_png.cpp
    screen/test_screen.cpp
    screen/test_enumerate_fallback.cpp
    screen/test_capture_window_id.cpp
    image/test_compare.cpp
    image/test_compare_dirs.cpp
    input/test_keymap.cpp
    input/test_input.cpp
    event/test_event_descriptor.cpp
    event/test_payload_fields.cpp
    notify/test_notifier.cpp
    transport/test_codec.cpp
    storage/test_jsonl_sink.cpp
    service/test_daemon.cpp
    transport/test_service.cpp
    transport/test_service_hardening.cpp
)
target_link_libraries(grab_core_tests PRIVATE grab_core grab_kernel grab_codec grab_screen grab_image grab_input grab_notify grab_proto grab_transport grab_storage grab_service ${GRAB_LIBAV_TARGETS} GTest::gtest_main)
target_include_directories(grab_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(grab_core_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/compat/x11_relocation_shim)
target_compile_definitions(grab_core_tests PRIVATE
    GRAB_CODEC_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/codec/pngs"
    GRAB_PROFILES_DIR="${CMAKE_SOURCE_DIR}/profiles")
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
    kernel/scheduling/test_pacing_governor.cpp
    kernel/scheduling/test_reactor.cpp
    kernel/capture/test_tile_differ.cpp
    kernel/lifecycle/test_session_core.cpp
    kernel/lifecycle/test_observation_pump.cpp
    kernel/lifecycle/test_session_verbs.cpp
    kernel/graph/test_state_manager.cpp
    kernel/graph/test_state_snapshot_provider.cpp
    kernel/test_gesture_classifier.cpp
    kernel/test_cursor_feedback.cpp
    kernel/test_ui.cpp
    kernel/test_overlay_animation.cpp
    kernel/test_overlay_delegate_contract.cpp
    kernel/test_overlay_draw.cpp
    kernel/test_overlay_edit.cpp
    kernel/test_overlay_edit_session.cpp
    kernel/test_overlay_raster.cpp
    kernel/test_overlay_scene.cpp
    kernel/test_overlay_service.cpp
    kernel/test_trail_animator.cpp
    kernel/test_tree_store.cpp
    kernel/test_target_registry.cpp
    kernel/test_fake_runtime.cpp
    query/test_query.cpp
)
target_link_libraries(grab_kernel_tests PRIVATE
    grab_kernel
    grab_codec
    GTest::gtest_main)
target_include_directories(grab_kernel_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(grab_kernel_tests PRIVATE
    GRAB_OVERLAY_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/kernel/golden/overlay")
gtest_discover_tests(grab_kernel_tests)

add_executable(grab_driver_x11_tests
    drivers/x11/test_coordinate_authority.cpp
    drivers/x11/test_enumerate.cpp
    drivers/x11/test_overlay_delegate.cpp
    drivers/x11/test_record.cpp
    drivers/x11/test_virtual_display.cpp
    drivers/x11/test_workflow.cpp
    drivers/x11/test_x11_capture.cpp
    drivers/x11/test_x11_capture_route.cpp
    drivers/x11/test_x11_event_source.cpp
    drivers/x11/test_x11_input_correctness.cpp
    drivers/x11/test_x11_runtime.cpp
    drivers/x11/test_x11_tree_source.cpp
    drivers/x11/test_x11_xtest_seat.cpp
    drivers/x11/test_x11_drag_recipe.cpp
    drivers/x11/test_window_tracker.cpp
)
target_link_libraries(grab_driver_x11_tests PRIVATE
    grab_driver_x11
    grab_notify
    ${GRAB_LIBAV_TARGETS}
    GTest::gtest_main
)
target_include_directories(grab_driver_x11_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

add_executable(grab_config_batch_window fake/config_batch_window.cpp)
target_link_libraries(grab_config_batch_window PRIVATE PkgConfig::GRAB_XCB)
target_compile_features(grab_config_batch_window PRIVATE cxx_std_23)

add_executable(grab_integration_tests
    integration/test_config_batch.cpp
    integration/test_config_watch.cpp
    integration/test_exit_gate.cpp
    integration/test_overlay_ring3.cpp
)
add_dependencies(grab_integration_tests grab_config_batch_window)
target_compile_definitions(grab_integration_tests PRIVATE
    GRAB_CONFIG_BATCH_WINDOW_PATH="$<TARGET_FILE:grab_config_batch_window>")
target_link_libraries(grab_integration_tests PRIVATE
    grab_client
    grab_kernel
    grab_driver_x11
    grab_screen
    grab_input
    grab_input_x11
    grab_session
    grab_transport
    grab_compat_eventgrab_v1
    PkgConfig::GRAB_XCB_RANDR
    PkgConfig::GRAB_XCB_SHAPE
    GTest::gtest_main
)
target_include_directories(grab_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

add_executable(grab_driver_atspi_tests
    drivers/atspi/test_atspi.cpp
    drivers/atspi/test_atspi_mapping.cpp
    drivers/atspi/test_atspi_alias.cpp
    drivers/atspi/test_atspi_runtime.cpp
)
target_link_libraries(grab_driver_atspi_tests PRIVATE
    grab_driver_atspi
    grab_driver_x11
    GTest::gtest_main
)
target_include_directories(grab_driver_atspi_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
gtest_discover_tests(grab_driver_atspi_tests DISCOVERY_MODE PRE_TEST)

add_executable(grab_driver_evdev_tests
    drivers/device/evdev/test_evdev_source.cpp
)
target_link_libraries(grab_driver_evdev_tests PRIVATE
    grab_driver_evdev
    GTest::gtest_main
)
target_include_directories(grab_driver_evdev_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
gtest_discover_tests(grab_driver_evdev_tests)

add_executable(grab_compat_tests
    compat/eventgrab_v1/test_browser_evidence.cpp
    compat/eventgrab_v1/test_browser_projection.cpp
)
target_link_libraries(grab_compat_tests PRIVATE
    grab_compat_eventgrab_v1
    grab_core
    grab_proto
    GTest::gtest_main
)
target_include_directories(grab_compat_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
gtest_discover_tests(grab_compat_tests)

add_executable(grab_driver_webextension_tests
    drivers/semantic/webextension/test_browser_bridge.cpp
)
target_link_libraries(grab_driver_webextension_tests PRIVATE
    grab_driver_webextension
    grab_core
    GTest::gtest_main
)
target_include_directories(grab_driver_webextension_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)
gtest_discover_tests(grab_driver_webextension_tests)

# A7: self-provision the Xvfb displays the X-backed suites use so `ctest` is
# self-contained (no manually-started display). Opt-out with -DGRAB_TEST_PROVISION_XVFB=OFF.
option(GRAB_TEST_PROVISION_XVFB
    "Auto-provision Xvfb displays for X-backed tests via a CTest fixture" ON)

if(GRAB_TEST_PROVISION_XVFB)
    gtest_discover_tests(grab_core_tests TEST_FILTER "Compare.*:Diff.*:Workflow.CompareFilesDetectsDifference:Config*.*")
    gtest_discover_tests(grab_core_tests
        TEST_FILTER "-Compare.*:Diff.*:Workflow.CompareFilesDetectsDifference:Config*.*"
        PROPERTIES FIXTURES_REQUIRED grab_xvfb ENVIRONMENT "DISPLAY=:87")
    add_test(NAME grab_xvfb_setup
        COMMAND ${CMAKE_COMMAND} -E env XVFB_PIDFILE=${CMAKE_BINARY_DIR}/grab-xvfb.pids
                sh ${CMAKE_CURRENT_SOURCE_DIR}/xvfb_fixture.sh start)
    set_tests_properties(grab_xvfb_setup PROPERTIES FIXTURES_SETUP grab_xvfb)
    add_test(NAME grab_xvfb_cleanup
        COMMAND ${CMAKE_COMMAND} -E env XVFB_PIDFILE=${CMAKE_BINARY_DIR}/grab-xvfb.pids
                sh ${CMAKE_CURRENT_SOURCE_DIR}/xvfb_fixture.sh stop)
    add_test(NAME grab_config_watch_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/config_watch_smoke.sh
                ${CMAKE_BINARY_DIR})
    set_tests_properties(grab_config_watch_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME grab_config_batch_smoke
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/config_batch_smoke.sh
                ${CMAKE_BINARY_DIR} $<TARGET_FILE:grab_config_batch_window>)
    set_tests_properties(grab_config_batch_smoke PROPERTIES TIMEOUT 120)
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
    drivers/x11/xcb_reply_test.cpp
    drivers/x11/xkb_keymap_test.cpp
    drivers/x11/xi_seat_test.cpp
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
