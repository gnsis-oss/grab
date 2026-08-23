cmake_minimum_required(VERSION 3.24)
# Single source of truth for the project's semantic version.
project(grab VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

# An unset CMAKE_BUILD_TYPE on a single-config generator means no optimization
# flags AND no -g: every binary is -O0 and stripped of line information, with
# no diagnostic. That was grab's effective default for its whole life. Presets
# all set one explicitly; this catches a bare `cmake -B ...` that does not.
get_property(_grab_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND NOT _grab_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Build type: Debug | Release | RelWithDebInfo | MinSizeRel" FORCE)
    message(STATUS "Build type: RelWithDebInfo (defaulted — none was requested)")
elseif(PROJECT_IS_TOP_LEVEL AND NOT _grab_multi_config)
    message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
endif()

# Dev tooling is for grab's OWN top-level builds only. As an add_subdirectory
# dependency the `format ALL` target collides with the consumer's, so guard the
# whole group on PROJECT_IS_TOP_LEVEL.
#
# Order matters: Coverage reads GRAB_SANITIZER, Logging is read by every
# target below.
if(PROJECT_IS_TOP_LEVEL)
    include(Cache)
    include(Warnings)
    include(Sanitizers)
    include(Coverage)
    include(Format)
    include(Tidy)
    include(IWYU)
    include(Profile)
endif()
include(Logging)

# Build directories moved under build/<config>, where clangd does not look.
# The `dev` preset — and only it, so several configurations never fight over
# the link — points the editor at its compile database.
option(GRAB_COMPILE_COMMANDS_LINK
       "Symlink compile_commands.json into the source root for clangd" OFF)
if(PROJECT_IS_TOP_LEVEL AND GRAB_COMPILE_COMMANDS_LINK)
    file(CREATE_LINK
        ${CMAKE_BINARY_DIR}/compile_commands.json
        ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
        SYMBOLIC)
endif()

# Build-essential modules define find/helper functions grab_core's build calls.
include(Xcb)
include(Grpc)
include(Codec)
include(AddJson)

grab_find_xcb()
grab_find_zlib()
grab_find_libav()
find_package(PkgConfig REQUIRED)
if(NOT TARGET PkgConfig::GRAB_XKBCOMMON)
    pkg_check_modules(GRAB_XKBCOMMON REQUIRED IMPORTED_TARGET GLOBAL xkbcommon)
endif()
grab_proto_library(grab_proto
    proto/eventgrab/v1/events.proto
    proto/eventgrab/v1/service.proto
)

add_library(grab_core STATIC
    src/config/batch_manifest.cpp
    src/config/environment.cpp
    src/config/load.cpp
    src/config/pattern.cpp
    src/config/rotation.cpp
    src/config/schedule.cpp
    src/spi/monitor.cpp
    src/kernel/lifecycle/context.cpp
    src/kernel/routing/doctor.cpp
    src/kernel/support/log.cpp
    src/kernel/support/diag.cpp
    src/kernel/events/event_bus.cpp
    src/kernel/identity/id_factory.cpp
    src/kernel/support/posix_open.c
    src/spi/permission.cpp
    src/kernel/lifecycle/process_ref.cpp
    src/kernel/routing/prober.cpp
    src/kernel/scheduling/reactor.cpp
    src/kernel/routing/registry.cpp
    src/kernel/routing/resolver.cpp
    src/kernel/lifecycle/session.cpp
    src/kernel/presentation/space_graph.cpp
    src/kernel/lifecycle/session_impl.cpp
    src/kernel/lifecycle/observation_pump.cpp
)
target_link_libraries(grab_core PUBLIC nlohmann_json::nlohmann_json)
# The generated log ceiling must reach every TU that includes
# kernel/support/log.hpp, which is any of them. PUBLIC on grab_core carries it
# to every other grab target and to the tests.
target_link_libraries(grab_core PUBLIC grab_log_config)
# Composition root: SessionCore (grab_core) composes the X11 and AT-SPI driver stacks.
# The static-library cycles through grab_kernel and both drivers are intentional; CMake supports cycles among STATIC libraries.
target_link_libraries(grab_core PRIVATE grab_driver_x11)
target_link_libraries(grab_core PRIVATE grab_driver_atspi)
# The core->driver_atspi->kernel->core component needs three repetitions because atspi_monitor.cpp.o is only pulled in the second pass and still needs Reactor from grab_core.
set_target_properties(grab_core PROPERTIES LINK_INTERFACE_MULTIPLICITY 3)
target_include_directories(grab_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(grab_core PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_core PUBLIC cxx_std_23)
# Feed the project's semantic version (source of truth: project(... VERSION))
# into the code so grab/version.hpp reflects it with no duplicated constant.
target_compile_definitions(grab_core PUBLIC
    GRAB_VERSION_MAJOR=${grab_VERSION_MAJOR}
    GRAB_VERSION_MINOR=${grab_VERSION_MINOR}
    GRAB_VERSION_PATCH=${grab_VERSION_PATCH})

add_library(grab_kernel STATIC
    src/kernel/action/transaction.cpp
    src/kernel/action/polling_event_source.cpp
    src/kernel/action/wait_engine.cpp
    src/kernel/capture/damage_hint_auditor.cpp
    src/kernel/capture/inject_gate.cpp
    src/kernel/scheduling/pacing_governor.cpp
    src/kernel/scheduling/timer_thread.cpp
    src/kernel/sequence/sequence.cpp
    src/kernel/sequence/sequence_ops.cpp
    src/kernel/sequence/interpreter.cpp
    src/kernel/sequence/execute.cpp
    src/kernel/sequence/player.cpp
    src/kernel/sequence/drive.cpp
    src/kernel/capture/tile_differ.cpp
    src/kernel/graph/tree_store.cpp
    src/kernel/graph/target_registry.cpp
    src/kernel/graph/state_manager.cpp
    src/kernel/graph/state_snapshot_provider.cpp
    src/kernel/input/gesture_classifier.cpp
    src/kernel/presentation/cursor_feedback.cpp
    src/kernel/presentation/overlay_draw.cpp
    src/kernel/presentation/overlay_animation.cpp
    src/kernel/presentation/overlay_edit.cpp
    src/kernel/presentation/overlay_edit_session.cpp
    src/kernel/presentation/overlay_raster.cpp
    src/kernel/presentation/overlay_scene.cpp
    src/kernel/presentation/overlay_service.cpp
    src/kernel/presentation/trail_animator.cpp
    src/kernel/query/evaluator.cpp
    src/kernel/query/locator.cpp
    src/kernel/query/snapshot_tree_nav.cpp
)
target_link_libraries(grab_kernel PUBLIC grab_core)
target_include_directories(grab_kernel PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_kernel PUBLIC cxx_std_23)

# The public sequence facade (include/grab/sequence.hpp): load a document,
# play it against a Session. Its seat needs the display-bound facades
# (grab::Input, grab::Screen, PNG encoding), which is exactly why it is not
# part of grab_kernel — the kernel stays display-free and testable without X.
add_library(grab_sequence STATIC
    src/sequence/api.cpp
)
target_link_libraries(grab_sequence PUBLIC grab_core grab_kernel
    PRIVATE grab_input grab_screen grab_codec)
target_include_directories(grab_sequence PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_sequence PUBLIC cxx_std_23)

add_library(grab_codec STATIC
    src/codec/png.cpp
)
target_link_libraries(grab_codec PUBLIC grab_core PRIVATE "${GRAB_ZLIB_TARGET}")
target_include_directories(grab_codec PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(grab_codec PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_codec PUBLIC cxx_std_23)

add_library(grab_screen STATIC
    src/drivers/desktop/x11/screen_facade.cpp
)
target_link_libraries(grab_screen
    PUBLIC
        grab_core
        grab_codec
        PkgConfig::GRAB_XCB
        PkgConfig::GRAB_XCB_COMPOSITE
        PkgConfig::GRAB_XCB_SHM
        PkgConfig::GRAB_XCB_RANDR
)
target_include_directories(grab_screen PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_screen PUBLIC cxx_std_23)

add_library(grab_image STATIC
    src/image/compare.cpp
    src/image/compare_dirs.cpp
)
target_link_libraries(grab_image PUBLIC grab_core PRIVATE grab_codec)
target_include_directories(grab_image PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(grab_image PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(grab_image PUBLIC cxx_std_23)

add_library(grab_input STATIC
    src/drivers/desktop/x11/input_facade.cpp
)
target_link_libraries(grab_input PUBLIC grab_core grab_driver_x11 grab_input_x11 PkgConfig::GRAB_XCB PkgConfig::GRAB_XCB_XTEST)
target_include_directories(grab_input PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_input PUBLIC cxx_std_23)

# eventgrab.v1 compat: the browser vocabulary left the core and is re-derived as a
# v1-wire projection here (src/compat/eventgrab_v1/).
add_library(grab_compat_eventgrab_v1 STATIC
    src/compat/eventgrab_v1/browser_projection.cpp
    src/compat/eventgrab_v1/v1_payload_codec.cpp
)
target_link_libraries(grab_compat_eventgrab_v1 PUBLIC grab_core grab_proto "${GRAB_PROTOBUF_TARGET}")
target_include_directories(grab_compat_eventgrab_v1 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_compat_eventgrab_v1 PUBLIC cxx_std_23)

# WebExtension native-messaging bridge (contributes integration app.* events).
add_library(grab_driver_webextension STATIC
    src/drivers/semantic/webextension/browser_bridge.cpp
)
target_link_libraries(grab_driver_webextension PUBLIC grab_core)
target_include_directories(grab_driver_webextension PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_driver_webextension PUBLIC cxx_std_23)

add_library(grab_notify STATIC
    src/notify/notifier.cpp
)
target_link_libraries(grab_notify PUBLIC grab_core PkgConfig::GRAB_DBUS_1)
target_include_directories(grab_notify PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_notify PUBLIC cxx_std_23)

target_link_libraries(grab_screen PUBLIC grab_image grab_notify)

add_library(grab_session STATIC
    src/session/display_probe.cpp
    src/session/manager.cpp
    src/session/poll_wait.cpp
    src/session/provisioning.cpp
    src/session/record.cpp
    src/session/registry.cpp
    src/session/selection.cpp
    src/session/state_machine.cpp
    src/session/virtual_display.cpp
)
target_link_libraries(grab_session PUBLIC grab_core PkgConfig::GRAB_XCB)
target_include_directories(grab_session PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(grab_session PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_session PUBLIC cxx_std_23)

add_library(grab_platform_x11 STATIC
    src/drivers/desktop/x11/xcb_connection.cpp
    src/drivers/desktop/x11/xi_seat.cpp
    src/session/x11_seat_provider.cpp
    src/session/builtin_session_providers.cpp
)
target_link_libraries(grab_platform_x11 PUBLIC grab_core grab_session
    PkgConfig::GRAB_XCB PkgConfig::GRAB_XCB_XINPUT)
target_include_directories(grab_platform_x11 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(grab_platform_x11 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_platform_x11 PUBLIC cxx_std_23)

add_library(grab_driver_x11 STATIC
    src/drivers/desktop/x11/config_batch.cpp
    src/drivers/desktop/x11/config_watch.cpp
    src/drivers/desktop/x11/coordinate_authority.cpp
    src/drivers/desktop/x11/enumerate.cpp
    src/drivers/desktop/x11/overlay_delegate.cpp
    src/drivers/desktop/x11/record.cpp
    src/drivers/desktop/x11/workflow.cpp
    src/drivers/desktop/x11/x11_capture.cpp
    src/drivers/desktop/x11/x11_capture_route.cpp
    src/drivers/desktop/x11/x11_event_source.cpp
    src/drivers/desktop/x11/x11_input_correctness.cpp
    src/drivers/desktop/x11/x11_routes.cpp
    src/drivers/desktop/x11/x11_runtime.cpp
    src/drivers/desktop/x11/x11_topology_source.cpp
    src/drivers/desktop/x11/x11_tree_source.cpp
    src/drivers/desktop/x11/x11_xtest_seat.cpp
    src/drivers/desktop/x11/window_tracker.cpp
)
target_link_libraries(grab_driver_x11
    PUBLIC
        grab_kernel
        grab_platform_x11
    PRIVATE
        grab_screen
        grab_input_x11
        ${GRAB_LIBAV_TARGETS}
        PkgConfig::GRAB_XCB_RENDER
        PkgConfig::GRAB_XCB_SHAPE
        PkgConfig::GRAB_XCB_SHM
        PkgConfig::GRAB_XCB_XTEST
        PkgConfig::GRAB_XCB_XFIXES
)
target_include_directories(grab_driver_x11 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_driver_x11 PUBLIC cxx_std_23)

add_library(grab_driver_evdev STATIC
    src/drivers/device/evdev/evdev_source.cpp
)
target_link_libraries(grab_driver_evdev PUBLIC grab_core)
target_include_directories(grab_driver_evdev PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_driver_evdev PUBLIC cxx_std_23)

add_library(grab_driver_atspi STATIC
    src/drivers/semantic/atspi/atspi_enumerator.cpp
    src/drivers/semantic/atspi/atspi_monitor.cpp
    src/drivers/semantic/atspi/atspi_runtime.cpp
    src/drivers/semantic/atspi/atspi_tree_source.cpp
)
target_link_libraries(grab_driver_atspi
    PUBLIC
        grab_kernel
        # atspi_monitor.hpp exposes <dbus/dbus.h> types, so consumers of the
        # driver header need the D-Bus include path.
        PkgConfig::GRAB_DBUS_1
)
target_include_directories(grab_driver_atspi PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_driver_atspi PUBLIC cxx_std_23)

add_library(grab_input_x11 STATIC
    src/drivers/desktop/x11/xkb_keymap.cpp
)
target_link_libraries(grab_input_x11 PUBLIC grab_platform_x11
    PkgConfig::GRAB_XKBCOMMON PkgConfig::GRAB_XKBCOMMON_X11)
target_include_directories(grab_input_x11 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_input_x11 PUBLIC cxx_std_23)

add_library(grab_transport STATIC
    src/frontends/grpc/codec.cpp
    src/frontends/grpc/service.cpp
    src/frontends/grpc/server.cpp
)
target_link_libraries(grab_transport PUBLIC grab_core grab_proto "${GRAB_GRPCXX_TARGET}" "${GRAB_PROTOBUF_TARGET}")
target_link_libraries(grab_transport PRIVATE grab_kernel grab_codec grab_compat_eventgrab_v1)
target_include_directories(grab_transport PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_transport PUBLIC cxx_std_23)

add_library(grab_client STATIC
    src/client/client.cpp
    src/client/loopback_transport.cpp
    src/client/unix_socket_transport.cpp
)
target_link_libraries(grab_client PUBLIC grab_core PRIVATE grab_transport grab_kernel grab_codec)
target_include_directories(grab_client PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_client PUBLIC cxx_std_23)

add_library(grab_storage STATIC
    src/storage/jsonl_sink.cpp
)
target_link_libraries(grab_storage PUBLIC grab_core)
target_include_directories(grab_storage PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_storage PUBLIC cxx_std_23)

add_library(grab_service STATIC
    src/frontends/grpc/daemon.cpp
)
target_link_libraries(grab_service PUBLIC grab_core grab_transport grab_storage)
target_include_directories(grab_service PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_service PUBLIC cxx_std_23)

add_executable(grab_cli
    src/frontends/cli/main.cpp
    src/frontends/cli/feedback_command.cpp
    src/frontends/cli/input_command.cpp
    src/frontends/cli/log_options.cpp
    src/frontends/cli/overlay_command.cpp
    src/frontends/cli/play_command.cpp
    src/frontends/cli/session_command.cpp
    src/frontends/cli/sketch_command.cpp
    src/frontends/cli/watch_daemon.cpp
    src/frontends/cli/windows_command.cpp
)
set_target_properties(grab_cli PROPERTIES OUTPUT_NAME grab)
target_link_libraries(grab_cli PRIVATE grab_core grab_codec grab_screen grab_image grab_notify grab_input grab_sequence grab_service
    grab_client grab_driver_x11 grab_input_x11 grab_kernel grab_platform_x11 grab_session)
target_include_directories(grab_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_cli PRIVATE cxx_std_23)

# Until now there was no install rule of any kind, so the only way to run the
# CLI was by path into a build directory -- and the obvious such path held a
# months-old binary that predated half the verbs. Installing is what makes
# `grab` a command rather than an artifact you have to go find.
#
#   cmake --install build/dev --prefix ~/.local
#
# The headers go with it so an add_subdirectory consumer is not the only way to
# build against the library. The static archives are deliberately NOT exported:
# that needs an export set covering every internal target and a config package
# to go with it, and shipping half of one would be worse than shipping none.
if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    install(TARGETS grab_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include/grab
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")
endif()

# grab's own examples and tests build only for grab's top-level builds. As an
# add_subdirectory dependency (spider's SPIDER_ENABLE_GRAB path) a consumer only
# wants grab_core, and BUILD_TESTING is the consumer's flag for ITS tests.
option(GRAB_BUILD_EXAMPLES "Build example programs" ON)
if(PROJECT_IS_TOP_LEVEL AND GRAB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

option(GRAB_BUILD_BENCHMARKS "Build benchmark programs" ON)
if(PROJECT_IS_TOP_LEVEL AND GRAB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    include(AddGTest)
    enable_testing()
    add_subdirectory(tests)
endif()
