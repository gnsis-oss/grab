cmake_minimum_required(VERSION 3.24)
# Single source of truth for the project's semantic version.
project(grab VERSION 0.0.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

include(Cache)
include(Warnings)
include(Sanitizers)
include(Coverage)
include(Format)
include(Tidy)
include(IWYU)
include(Profile)
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

add_library(grab_vendor_l0 INTERFACE)
target_include_directories(grab_vendor_l0 SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/l0)

add_library(grab_core STATIC
    src/core/monitor.cpp
    src/core/context.cpp
    src/core/doctor.cpp
    src/core/event_bus.cpp
    src/core/id_factory.cpp
    src/core/posix_open.c
    src/core/permission.cpp
    src/core/process_ref.cpp
    src/core/prober.cpp
    src/core/reactor.cpp
    src/core/registry.cpp
    src/core/resolver.cpp
    src/core/session.cpp
    src/core/space_graph.cpp
    src/kernel/lifecycle/session_impl.cpp
)
target_link_libraries(grab_core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(grab_core PRIVATE grab_vendor_l0)
# Composition root: SessionCore (grab_core) composes the X11 and AT-SPI driver stacks.
# The static-library cycles through grab_kernel and both drivers are intentional; CMake supports cycles among STATIC libraries.
target_link_libraries(grab_core PRIVATE grab_driver_x11)
target_link_libraries(grab_core PRIVATE grab_driver_atspi)
# The core->driver_atspi->event->core component needs three repetitions because atspi.cpp.o is only pulled in the second pass and still needs Reactor from grab_core.
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
    src/kernel/capture/pacing_governor.cpp
    src/kernel/capture/tile_differ.cpp
    src/kernel/graph/tree_store.cpp
    src/kernel/graph/target_registry.cpp
    src/kernel/query/evaluator.cpp
    src/kernel/query/locator.cpp
    src/kernel/query/snapshot_tree_nav.cpp
)
target_link_libraries(grab_kernel PUBLIC grab_core PRIVATE grab_vendor_l0)
target_include_directories(grab_kernel PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_kernel PUBLIC cxx_std_23)

add_library(grab_codec STATIC
    src/codec/png.cpp
)
target_link_libraries(grab_codec PUBLIC grab_core PRIVATE "${GRAB_ZLIB_TARGET}")
target_include_directories(grab_codec PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_include_directories(grab_codec PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_codec PUBLIC cxx_std_23)

add_library(grab_screen STATIC
    src/screen/screen.cpp
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
)
target_link_libraries(grab_image PUBLIC grab_core)
target_include_directories(grab_image PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(grab_image PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(grab_image PUBLIC cxx_std_23)

add_library(grab_input STATIC
    src/input/input.cpp
    src/input/locator.cpp
)
target_link_libraries(grab_input PUBLIC grab_core grab_driver_x11 grab_input_x11 PkgConfig::GRAB_XCB PkgConfig::GRAB_XCB_XTEST)
target_include_directories(grab_input PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(grab_input PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/compat/x11_relocation_shim)
target_compile_features(grab_input PUBLIC cxx_std_23)

add_library(grab_event STATIC
    src/event/atspi.cpp
    src/event/window_x11.cpp
    src/event/browser_bridge.cpp
    src/event/source_registry.cpp
    src/event/platform_factory.cpp
    src/event/state.cpp
    src/event/state_source.cpp
    src/event/evdev.cpp
    src/event/xinput2.cpp
)
target_link_libraries(grab_event PUBLIC grab_core PkgConfig::GRAB_XCB PkgConfig::GRAB_XCB_XINPUT PkgConfig::GRAB_DBUS_1)
target_include_directories(grab_event PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_include_directories(grab_event PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/compat/x11_relocation_shim)
target_compile_features(grab_event PUBLIC cxx_std_23)

add_library(grab_notify STATIC
    src/notify/notifier.cpp
)
target_link_libraries(grab_notify PUBLIC grab_core PkgConfig::GRAB_DBUS_1)
target_include_directories(grab_notify PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_notify PUBLIC cxx_std_23)

target_link_libraries(grab_screen PUBLIC grab_event grab_image grab_notify)

add_library(grab_session STATIC
    src/session/manager.cpp
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
    src/drivers/desktop/x11/coordinate_authority.cpp
    src/drivers/desktop/x11/enumerate.cpp
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
)
target_link_libraries(grab_driver_x11
    PUBLIC
        grab_kernel
        grab_platform_x11
    PRIVATE
        grab_vendor_l0
        grab_screen
        grab_input_x11
        ${GRAB_LIBAV_TARGETS}
        PkgConfig::GRAB_XCB_XTEST
)
target_include_directories(grab_driver_x11 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_driver_x11 PUBLIC cxx_std_23)

add_library(grab_driver_atspi STATIC
    src/drivers/semantic/atspi/atspi_runtime.cpp
    src/drivers/semantic/atspi/atspi_tree_source.cpp
)
target_link_libraries(grab_driver_atspi
    PUBLIC
        grab_kernel
        grab_event
    PRIVATE
        grab_vendor_l0
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
    src/transport/codec.cpp
    src/transport/service.cpp
    src/transport/server.cpp
)
target_link_libraries(grab_transport PUBLIC grab_core grab_proto "${GRAB_GRPCXX_TARGET}" "${GRAB_PROTOBUF_TARGET}")
target_link_libraries(grab_transport PRIVATE grab_kernel grab_codec)
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
    src/service/daemon.cpp
)
target_link_libraries(grab_service PUBLIC grab_core grab_transport grab_storage grab_event)
target_include_directories(grab_service PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_service PUBLIC cxx_std_23)

add_executable(grab_cli
    src/cli/main.cpp
    src/cli/input_command.cpp
    src/cli/session_command.cpp
)
set_target_properties(grab_cli PROPERTIES OUTPUT_NAME grab)
target_link_libraries(grab_cli PRIVATE grab_core grab_codec grab_screen grab_image grab_notify grab_input grab_service
    grab_client grab_input_x11 grab_platform_x11 grab_session)
target_include_directories(grab_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
target_compile_features(grab_cli PRIVATE cxx_std_23)

if(BUILD_TESTING)
    include(AddGTest)
    enable_testing()
    add_subdirectory(tests)
endif()
