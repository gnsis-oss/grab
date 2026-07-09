include_guard(GLOBAL)

include(FindPkgConfig)

function(grab_find_zlib)
    find_package(PkgConfig REQUIRED)

    if(NOT TARGET PkgConfig::GRAB_ZLIB)
        pkg_check_modules(GRAB_ZLIB QUIET IMPORTED_TARGET GLOBAL zlib)
    endif()

    if(NOT TARGET PkgConfig::GRAB_ZLIB AND NOT GRAB_ZLIB_FOUND)
        message(FATAL_ERROR
            "Missing required zlib pkg-config package.\n"
            "Install zlib development headers and ensure PKG_CONFIG_PATH can find zlib.pc."
        )
    endif()

    set(GRAB_ZLIB_TARGET PkgConfig::GRAB_ZLIB CACHE INTERNAL "grab zlib library target")
    message(STATUS "zlib: using pkg-config")
endfunction()

function(_grab_codec_pkg_config_target_name MODULE_NAME OUT_VAR)
    string(MAKE_C_IDENTIFIER "${MODULE_NAME}" _identifier)
    string(TOUPPER "${_identifier}" _upper_identifier)
    set("${OUT_VAR}" "GRAB_${_upper_identifier}" PARENT_SCOPE)
endfunction()

function(_grab_mark_pkg_config_includes_system TARGET_NAME)
    get_target_property(_include_dirs "${TARGET_NAME}" INTERFACE_INCLUDE_DIRECTORIES)
    if(_include_dirs)
        set_target_properties("${TARGET_NAME}" PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_include_dirs}"
        )
    endif()
endfunction()

function(grab_find_libav)
    find_package(PkgConfig REQUIRED)

    set(_required_modules
        libavcodec
        libavformat
        libavutil
        libswscale
    )

    set(_missing_modules)
    set(_targets)
    foreach(_module IN LISTS _required_modules)
        _grab_codec_pkg_config_target_name("${_module}" _target_name)

        if(NOT TARGET "PkgConfig::${_target_name}")
            pkg_check_modules("${_target_name}" QUIET IMPORTED_TARGET GLOBAL "${_module}")
        endif()

        if(NOT TARGET "PkgConfig::${_target_name}" AND NOT ${_target_name}_FOUND)
            list(APPEND _missing_modules "${_module}")
        else()
            list(APPEND _targets "PkgConfig::${_target_name}")
            _grab_mark_pkg_config_includes_system("PkgConfig::${_target_name}")
        endif()
    endforeach()

    if(_missing_modules)
        list(JOIN _missing_modules ", " _missing_text)
        list(JOIN _required_modules ", " _required_text)
        message(FATAL_ERROR
            "Missing required libav pkg-config package(s): ${_missing_text}.\n"
            "Required packages: ${_required_text}.\n"
            "Install FFmpeg/libav development headers and ensure PKG_CONFIG_PATH can find their .pc files."
        )
    endif()

    set(GRAB_LIBAV_TARGETS ${_targets} CACHE INTERNAL "grab libav library targets")
    message(STATUS "libav: using pkg-config")
endfunction()
