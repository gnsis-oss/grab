#include "grab/keymap.hpp"
#include "grab/window.hpp"
#include "input/input_sink.hpp"
#include "platform/x11/xcb_atom.hpp"
#include "platform/x11/xcb_connection.hpp"
#include "platform/x11/xcb_reply.hpp"
#include "platform/x11/xtest_input.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>
#include <xkbcommon/xkbcommon.h>

namespace grab::platform::x11
{

    namespace
    {

        constexpr std::uint8_t     motion_detail             = 0U;
        constexpr std::uint8_t     default_device            = 0U;
        constexpr std::int16_t     button_root_x             = 0;
        constexpr std::int16_t     button_root_y             = 0;
        constexpr std::uint8_t     do_not_propagate          = 0U;
        constexpr std::uint8_t     client_message_format     = 32U;
        constexpr std::uint32_t    activate_source_normal    = 1U;
        constexpr std::uint32_t    activate_no_current_event = 0U;
        constexpr std::string_view active_window_atom_name   = "_NET_ACTIVE_WINDOW";

        constexpr std::uint32_t    activate_event_mask =
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY ) |
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT );

        [[nodiscard]]
        std::int16_t
        clamp_i16( std::int32_t value ) noexcept
        {
            constexpr auto int16_min =
                static_cast<std::int32_t>( std::numeric_limits<std::int16_t>::min() );
            constexpr auto int16_max =
                static_cast<std::int32_t>( std::numeric_limits<std::int16_t>::max() );
            return static_cast<std::int16_t>(
                std::clamp( value, int16_min, int16_max )
            );
        }

        [[nodiscard]]
        std::uint8_t
        event_detail( xkb_keycode_t keycode ) noexcept
        {
            constexpr auto u_int8_max =
                static_cast<xkb_keycode_t>( std::numeric_limits<std::uint8_t>::max() );
            if( keycode > u_int8_max )
            {
                return std::numeric_limits<std::uint8_t>::max();
            }
            return static_cast<std::uint8_t>( keycode );
        }

        [[nodiscard]]
        xcb_client_message_event_t
        active_window_event( xcb_window_t target,
                             xcb_atom_t   atom ) noexcept
        {
            return xcb_client_message_event_t{
                .response_type = XCB_CLIENT_MESSAGE,
                .format        = client_message_format,
                .sequence      = 0U,
                .window        = target,
                .type          = atom,
                .data          = xcb_client_message_data_t{
                                                           .data32 = {
                        activate_source_normal,
                        XCB_CURRENT_TIME,
                        activate_no_current_event,
                    }, },
            };
        }

    }    // namespace

    XtestInputSink::XtestInputSink( const XcbConnection& conn,
                                    const grab::Keymap&  keymap,
                                    grab::WindowRef      target ) noexcept :
        conn( conn ),
        keymap( keymap ),
        target( target )
    {
    }

    void
    XtestInputSink::move( grab::input::Point p )
    {
        ( void )xcb_test_fake_input( conn.get(),
                                     XCB_MOTION_NOTIFY,
                                     motion_detail,
                                     XCB_CURRENT_TIME,
                                     conn.root(),
                                     clamp_i16( p.x ),
                                     clamp_i16( p.y ),
                                     default_device );
        flush();
    }

    void
    XtestInputSink::button( std::uint8_t code,
                            bool         press,
                            bool         clear_modifiers )
    {
        if( clear_modifiers )
        {
            release_held_modifiers();
        }
        emit_button( code, press );
        flush();
    }

    void
    XtestInputSink::sync()
    {
        const xcb_get_input_focus_cookie_t cookie = xcb_get_input_focus( conn.get() );
        auto                               reply =
            make_xcb_reply( xcb_get_input_focus_reply( conn.get(), cookie, nullptr ) );
        ( void )reply;
    }

    void
    XtestInputSink::wait( std::uint32_t millis )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds{ millis } );
    }

    void
    XtestInputSink::type_text( std::string_view utf8 )
    {
        auto strokes = keymap.text_to_keystrokes( utf8 );
        if( !strokes.has_value() )
        {
            return;
        }

        for( const auto& stroke : *strokes )
        {
            press_stroke( stroke );
            flush();
        }
    }

    void
    XtestInputSink::key( std::string_view keysym )
    {
        auto stroke = keymap.keystroke_for_key( keysym );
        if( !stroke.has_value() )
        {
            return;
        }

        press_stroke( *stroke );
        flush();
    }

    void
    XtestInputSink::activate()
    {
        if( !target.valid )
        {
            return;
        }

        auto atom =
            intern_atom( conn, active_window_atom_name, XcbAtomMode::CreateIfMissing );
        if( !atom.has_value() )
        {
            return;
        }

        const xcb_client_message_event_t event =
            active_window_event( static_cast<xcb_window_t>( target.id ), *atom );
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto* const raw_event = reinterpret_cast<const char*>( &event );
        ( void )xcb_send_event( conn.get(),
                                do_not_propagate,
                                conn.root(),
                                activate_event_mask,
                                raw_event );
        constexpr std::uint32_t stack_above = XCB_STACK_MODE_ABOVE;
        ( void )xcb_configure_window( conn.get(),
                                      static_cast<xcb_window_t>( target.id ),
                                      XCB_CONFIG_WINDOW_STACK_MODE,
                                      &stack_above );
        flush();
        sync();
    }

    void
    XtestInputSink::emit_key( std::uint8_t  type,
                              xkb_keycode_t keycode )
    {
        ( void )xcb_test_fake_input( conn.get(),
                                     type,
                                     event_detail( keycode ),
                                     XCB_CURRENT_TIME,
                                     conn.root(),
                                     button_root_x,
                                     button_root_y,
                                     default_device );
    }

    void
    XtestInputSink::emit_button( std::uint8_t code,
                                 bool         press )
    {
        ( void )xcb_test_fake_input( conn.get(),
                                     press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                                     code,
                                     XCB_CURRENT_TIME,
                                     conn.root(),
                                     button_root_x,
                                     button_root_y,
                                     default_device );
    }

    void
    XtestInputSink::press_modifier( xkb_keycode_t keycode )
    {
        emit_key( XCB_KEY_PRESS, keycode );
        if( std::ranges::find( held_modifiers, keycode ) == held_modifiers.end() )
        {
            held_modifiers.push_back( keycode );
        }
    }

    void
    XtestInputSink::release_modifier( xkb_keycode_t keycode )
    {
        emit_key( XCB_KEY_RELEASE, keycode );
        std::erase( held_modifiers, keycode );
    }

    void
    XtestInputSink::release_held_modifiers()
    {
        while( !held_modifiers.empty() )
        {
            const xkb_keycode_t keycode = held_modifiers.back();
            held_modifiers.pop_back();
            emit_key( XCB_KEY_RELEASE, keycode );
        }
    }

    void
    XtestInputSink::press_stroke( const grab::Keystroke& stroke )
    {
        if( stroke.shift )
        {
            press_modifier( static_cast<xkb_keycode_t>( keymap.shift_keycode() ) );
        }
        if( stroke.altgr )
        {
            press_modifier( static_cast<xkb_keycode_t>( keymap.altgr_keycode() ) );
        }

        emit_key( XCB_KEY_PRESS, static_cast<xkb_keycode_t>( stroke.keycode ) );
        emit_key( XCB_KEY_RELEASE, static_cast<xkb_keycode_t>( stroke.keycode ) );

        if( stroke.altgr )
        {
            release_modifier( static_cast<xkb_keycode_t>( keymap.altgr_keycode() ) );
        }
        if( stroke.shift )
        {
            release_modifier( static_cast<xkb_keycode_t>( keymap.shift_keycode() ) );
        }
    }

    void
    XtestInputSink::flush()
    {
        ( void )xcb_flush( conn.get() );
    }

}    // namespace grab::platform::x11
