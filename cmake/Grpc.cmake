include_guard(GLOBAL)

include(FindPkgConfig)

function(grab_find_grpc)
    find_package(Protobuf CONFIG QUIET)
    find_package(gRPC CONFIG QUIET)

    if(TARGET protobuf::libprotobuf AND TARGET gRPC::grpc++)
        set(_protobuf_target protobuf::libprotobuf)
        set(_grpcxx_target gRPC::grpc++)
        set(_discovery_source "CMake package config")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GRAB_PROTOBUF QUIET IMPORTED_TARGET GLOBAL protobuf)
        pkg_check_modules(GRAB_GRPCXX QUIET IMPORTED_TARGET GLOBAL grpc++)

        set(_missing_modules)
        if(NOT GRAB_PROTOBUF_FOUND)
            list(APPEND _missing_modules "protobuf")
        endif()
        if(NOT GRAB_GRPCXX_FOUND)
            list(APPEND _missing_modules "grpc++")
        endif()

        if(_missing_modules)
            list(JOIN _missing_modules ", " _missing_text)
            message(FATAL_ERROR
                "Missing required gRPC/protobuf pkg-config package(s): ${_missing_text}.\n"
                "Install protobuf and gRPC development packages and ensure PKG_CONFIG_PATH can find "
                "protobuf.pc and grpc++.pc."
            )
        endif()

        set(_protobuf_target PkgConfig::GRAB_PROTOBUF)
        set(_grpcxx_target PkgConfig::GRAB_GRPCXX)
        set(_discovery_source "pkg-config")
    endif()

    if(TARGET protobuf::protoc)
        set(_protoc "$<TARGET_FILE:protobuf::protoc>")
    elseif(Protobuf_PROTOC_EXECUTABLE)
        set(_protoc "${Protobuf_PROTOC_EXECUTABLE}")
    else()
        find_program(_protoc protoc)
    endif()

    if(TARGET gRPC::grpc_cpp_plugin)
        set(_grpc_cpp_plugin "$<TARGET_FILE:gRPC::grpc_cpp_plugin>")
    else()
        find_program(_grpc_cpp_plugin grpc_cpp_plugin)
    endif()

    if(NOT _protoc)
        message(FATAL_ERROR
            "Missing required protoc executable. Install protobuf-compiler or put protoc on PATH."
        )
    endif()

    if(NOT _grpc_cpp_plugin)
        message(FATAL_ERROR
            "Missing required grpc_cpp_plugin executable. Install gRPC C++ plugins or put "
            "grpc_cpp_plugin on PATH."
        )
    endif()

    set(GRAB_PROTOBUF_TARGET "${_protobuf_target}" CACHE INTERNAL "grab protobuf library target")
    set(GRAB_GRPCXX_TARGET "${_grpcxx_target}" CACHE INTERNAL "grab gRPC C++ library target")
    set(GRAB_PROTOC_EXECUTABLE "${_protoc}" CACHE INTERNAL "grab protoc executable")
    set(GRAB_GRPC_CPP_PLUGIN "${_grpc_cpp_plugin}" CACHE INTERNAL "grab grpc_cpp_plugin executable")

    message(STATUS "gRPC/protobuf: using ${_discovery_source}")
endfunction()

function(grab_proto_library TARGET_NAME)
    if(NOT ARGN)
        message(FATAL_ERROR "grab_proto_library(${TARGET_NAME}) requires at least one proto file.")
    endif()

    grab_find_grpc()

    set(_proto_import_root "${CMAKE_CURRENT_SOURCE_DIR}/proto")
    set(_generated_include_dir "${CMAKE_BINARY_DIR}/generated")

    if(NOT IS_DIRECTORY "${_proto_import_root}")
        message(FATAL_ERROR "Proto import root does not exist: ${_proto_import_root}")
    endif()

    set(_proto_abs_files)
    foreach(_proto IN LISTS ARGN)
        get_filename_component(_proto_abs "${_proto}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        if(NOT EXISTS "${_proto_abs}")
            message(FATAL_ERROR "Proto file does not exist: ${_proto}")
        endif()
        list(APPEND _proto_abs_files "${_proto_abs}")
    endforeach()

    set(_generated_sources)
    set(_generated_headers)
    foreach(_proto_abs IN LISTS _proto_abs_files)
        file(RELATIVE_PATH _proto_rel "${_proto_import_root}" "${_proto_abs}")
        if(_proto_rel MATCHES "^\\.\\.")
            message(FATAL_ERROR "Proto file must live under ${_proto_import_root}: ${_proto_abs}")
        endif()

        get_filename_component(_proto_dir "${_proto_rel}" DIRECTORY)
        get_filename_component(_proto_name "${_proto_rel}" NAME_WE)
        if(_proto_dir STREQUAL "")
            set(_output_dir "${_generated_include_dir}")
        else()
            set(_output_dir "${_generated_include_dir}/${_proto_dir}")
        endif()

        set(_pb_h "${_output_dir}/${_proto_name}.pb.h")
        set(_pb_cc "${_output_dir}/${_proto_name}.pb.cc")
        set(_grpc_pb_h "${_output_dir}/${_proto_name}.grpc.pb.h")
        set(_grpc_pb_cc "${_output_dir}/${_proto_name}.grpc.pb.cc")

        add_custom_command(
            OUTPUT
                "${_pb_h}"
                "${_pb_cc}"
                "${_grpc_pb_h}"
                "${_grpc_pb_cc}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
            COMMAND ${GRAB_PROTOC_EXECUTABLE}
                "--proto_path=${_proto_import_root}"
                "--cpp_out=${_generated_include_dir}"
                "--grpc_out=${_generated_include_dir}"
                "--plugin=protoc-gen-grpc=${GRAB_GRPC_CPP_PLUGIN}"
                "${_proto_rel}"
            DEPENDS ${_proto_abs_files}
            WORKING_DIRECTORY "${_proto_import_root}"
            COMMENT "Generating protobuf/gRPC sources for ${_proto_rel}"
            VERBATIM
        )

        list(APPEND _generated_sources "${_pb_cc}" "${_grpc_pb_cc}")
        list(APPEND _generated_headers "${_pb_h}" "${_grpc_pb_h}")
    endforeach()

    set_source_files_properties(
        ${_generated_sources}
        ${_generated_headers}
        PROPERTIES GENERATED TRUE
    )

    add_library(${TARGET_NAME} STATIC ${_generated_sources} ${_generated_headers})
    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_CLANG_TIDY ""
        C_CLANG_TIDY ""
        CXX_INCLUDE_WHAT_YOU_USE ""
        C_INCLUDE_WHAT_YOU_USE ""
    )
    target_compile_features(${TARGET_NAME} PUBLIC cxx_std_23)
    target_include_directories(${TARGET_NAME} SYSTEM PUBLIC "${_generated_include_dir}")
    target_link_libraries(${TARGET_NAME} PUBLIC "${GRAB_PROTOBUF_TARGET}" "${GRAB_GRPCXX_TARGET}")

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${TARGET_NAME} PRIVATE -w)
    elseif(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /w)
    endif()
endfunction()
