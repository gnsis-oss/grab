include_guard(GLOBAL)

include(FindPkgConfig)

function(_grab_pkg_config_target_name MODULE_NAME OUT_VAR)
    string(MAKE_C_IDENTIFIER "${MODULE_NAME}" _identifier)
    string(TOUPPER "${_identifier}" _upper_identifier)
    set("${OUT_VAR}" "GRAB_${_upper_identifier}" PARENT_SCOPE)
endfunction()

function(grab_find_xcb)
    find_package(PkgConfig REQUIRED)

    set(_required_modules
        xcb
        xcb-xtest
        xcb-composite
        xcb-render
        xcb-shape
        xcb-shm
        xcb-randr
        xcb-xfixes
        xcb-xinput
        xkbcommon-x11
        dbus-1
    )

    set(_missing_modules)
    foreach(_module IN LISTS _required_modules)
        _grab_pkg_config_target_name("${_module}" _target_name)

        if(TARGET "PkgConfig::${_target_name}")
            continue()
        endif()

        pkg_check_modules("${_target_name}" QUIET IMPORTED_TARGET GLOBAL "${_module}")
        if(NOT ${_target_name}_FOUND)
            list(APPEND _missing_modules "${_module}")
        endif()
    endforeach()

    if(_missing_modules)
        list(JOIN _missing_modules ", " _missing_text)
        list(JOIN _required_modules ", " _required_text)
        message(FATAL_ERROR
            "Missing required XCB/platform pkg-config package(s): ${_missing_text}.\n"
            "Required packages: ${_required_text}.\n"
            "Install the development packages and ensure PKG_CONFIG_PATH can find their .pc files."
        )
    endif()
endfunction()
