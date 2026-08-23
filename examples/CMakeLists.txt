add_executable(mouse_snake_trail mouse_snake_trail.cpp)
target_link_libraries(mouse_snake_trail PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_compile_features(mouse_snake_trail PRIVATE cxx_std_23)

# osu_stress used to be a second executable here: the VISUAL half of
# examples/sequences/osu_stress.json, parsing the same document so the circles
# it drew matched the pointer the sequence moved. It is gone, and nothing
# replaced it -- the eight overlay.* steps put the visuals INSIDE the document,
# so the whole example is now one command:
#
#     grab play examples/sequences/osu_stress.json
#
# There is no second copy of the coordinates to drift, and no second process to
# start, order or tear down.

add_executable(event_logger event_logger.cpp)
target_link_libraries(event_logger PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_driver_x11
    grab_driver_webextension
    grab_storage)
target_compile_features(event_logger PRIVATE cxx_std_23)

# Rung 4 of the visual capability ladder, ported from the spider work
# (l0/.worktrees/spider-visual): one authored page, one button, a human
# trajectory with a live trail, and a click proved three independent ways
# (a11y name flip, pixel flip, device-read press point). Brings up its own
# X session + WM + compositor + a11y bus + Firefox via support/host.hpp,
# or drives the current display with --session.
add_executable(stage_button stage_button.cpp)
target_link_libraries(stage_button PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_include_directories(stage_button PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stage_button PRIVATE cxx_std_23)

# Rung 8: press-drag-release — the macOS-install gesture against a real
# page. Grabs an authored icon, carries it with the button held on a human
# trajectory, drops it in a zone, and verifies on a11y, pixel and device
# channels. Same host and support set as stage_button.
add_executable(stage_drag stage_drag.cpp)
target_link_libraries(stage_drag PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_include_directories(stage_drag PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stage_drag PRIVATE cxx_std_23)

# Rung 9: wheel then click — a four-screenful page whose button starts below
# the fold, is wheeled into view in measured bursts (pixels-per-notch is a
# browser setting nothing asserts), then approached and clicked.
add_executable(stage_scroll stage_scroll.cpp)
target_link_libraries(stage_scroll PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_include_directories(stage_scroll PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stage_scroll PRIVATE cxx_std_23)

# Rung 9, long form: a variable-speed scroll journey — down slowly to a
# bottom button, back up at walking pace to a midway one, down again on a
# ramp to a third, clicking each stop. Each leg's necessity is asserted
# before it runs, and the event stream must carry wheel events in both
# directions.
add_executable(stage_scroll_tour stage_scroll_tour.cpp)
target_link_libraries(stage_scroll_tour PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_include_directories(stage_scroll_tour PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stage_scroll_tour PRIVATE cxx_std_23)

# Rung 6: the keyboard — focus a labelled entry with a real click, type a
# paragraph letter by letter at a human rhythm (word breaths, comma and
# full-stop beats, occasional thinks), then press SEND. Verified on a11y
# (value equals the paragraph exactly, receipt carries what the page's
# script received), pixel and device (key-downs counted) channels.
add_executable(stage_type stage_type.cpp)
target_link_libraries(stage_type PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_include_directories(stage_type PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stage_type PRIVATE cxx_std_23)

# Rung 7: the controls — a form carrying every HTML input type (all 22)
# plus a combo box; text, checkbox, radio, combo (by keyboard), range
# (click-the-track), number, date, password and a real type=submit are
# driven; values verified through page-side mirrors read over a11y, the
# APPLY summary, and a JS-enumerated catalog of the DOM's input types.
add_executable(stage_form stage_form.cpp)
target_link_libraries(stage_form PRIVATE
    grab_core
    grab_kernel
    grab_session
    grab_platform_x11
    grab_input
    grab_input_x11
    grab_screen
    grab_image
    grab_codec)
target_include_directories(stage_form PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_features(stage_form PRIVATE cxx_std_23)

# The consumer side of display provisioning, whole: one call for a display
# grab can honestly drive, and a report of which preconditions are live.
add_executable(provision_display provision_display.cpp)
target_link_libraries(provision_display PRIVATE
    grab_core
    grab_session)
target_compile_features(provision_display PRIVATE cxx_std_23)
