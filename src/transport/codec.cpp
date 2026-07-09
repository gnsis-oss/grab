#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/result.hpp"
#include "transport/codec.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace grab::transport
{
    namespace
    {

        constexpr std::uint64_t    kNoSequence     = 0U;

        constexpr std::string_view kKeyCode        = "key_code";
        constexpr std::string_view kKeyName        = "key_name";
        constexpr std::string_view kText           = "text";
        constexpr std::string_view kButton         = "button";
        constexpr std::string_view kButtonName     = "button_name";
        constexpr std::string_view kAxis           = "axis";
        constexpr std::string_view kDelta          = "delta";
        constexpr std::string_view kIdleS          = "idle_s";
        constexpr std::string_view kApp            = "app";
        constexpr std::string_view kPid            = "pid";
        constexpr std::string_view kTitle          = "title";
        constexpr std::string_view kPrevTitle      = "prev_title";
        constexpr std::string_view kDurationS      = "duration_s";
        constexpr std::string_view kRole           = "role";
        constexpr std::string_view kName           = "name";
        constexpr std::string_view kDetail         = "detail";
        constexpr std::string_view kState          = "state";
        constexpr std::string_view kJson           = "json";
        constexpr std::string_view kTabTitle       = "tab_title";
        constexpr std::string_view kPrevTabTitle   = "prev_tab_title";
        constexpr std::string_view kProtocolPrefix = "malformed event: ";

        [[nodiscard]]
        std::unexpected<grab::Error>
        protocol_error( std::string message )
        {
            return grab::fail( grab::ErrorCode::protocol_error,
                               std::string{ kProtocolPrefix } + std::move( message ) );
        }

        [[nodiscard]]
        eventgrab::v1::EventKind
        to_wire_kind( grab::EventKind kind )
        {
            switch( kind )
            {
                case grab::EventKind::key_down :
                    return eventgrab::v1::INPUT_KEY_DOWN;
                case grab::EventKind::key_up :
                    return eventgrab::v1::INPUT_KEY_UP;
                case grab::EventKind::key_combo :
                    return eventgrab::v1::INPUT_KEY_COMBO;
                case grab::EventKind::mouse_click :
                    return eventgrab::v1::INPUT_MOUSE_CLICK;
                case grab::EventKind::mouse_move :
                    return eventgrab::v1::INPUT_MOUSE_MOVE;
                case grab::EventKind::idle_start :
                    return eventgrab::v1::INPUT_IDLE_START;
                case grab::EventKind::idle_end :
                    return eventgrab::v1::INPUT_IDLE_END;
                case grab::EventKind::window_focus_changed :
                    return eventgrab::v1::WINDOW_FOCUS_CHANGED;
                case grab::EventKind::window_title_changed :
                    return eventgrab::v1::WINDOW_TITLE_CHANGED;
                case grab::EventKind::window_created :
                    return eventgrab::v1::WINDOW_CREATED;
                case grab::EventKind::window_closed :
                    return eventgrab::v1::WINDOW_CLOSED;
                case grab::EventKind::a11y_button_clicked :
                    return eventgrab::v1::A11Y_BUTTON_CLICKED;
                case grab::EventKind::a11y_menu_opened :
                    return eventgrab::v1::A11Y_MENU_OPENED;
                case grab::EventKind::a11y_menu_closed :
                    return eventgrab::v1::A11Y_MENU_CLOSED;
                case grab::EventKind::a11y_focus_changed :
                    return eventgrab::v1::A11Y_FOCUS_CHANGED;
                case grab::EventKind::a11y_text_changed :
                    return eventgrab::v1::A11Y_TEXT_CHANGED;
                case grab::EventKind::a11y_state_changed :
                    return eventgrab::v1::A11Y_STATE_CHANGED;
                case grab::EventKind::app_tab_changed :
                    return eventgrab::v1::APP_TAB_CHANGED;
                case grab::EventKind::app_context_update :
                    return eventgrab::v1::APP_CONTEXT_UPDATE;
                case grab::EventKind::browser_tab_switched :
                    return eventgrab::v1::BROWSER_TAB_SWITCHED;
                case grab::EventKind::state_snapshot :
                    return eventgrab::v1::STATE_SNAPSHOT;
                case grab::EventKind::unspecified :
                    return eventgrab::v1::EVENT_KIND_UNSPECIFIED;
            }

            return eventgrab::v1::EVENT_KIND_UNSPECIFIED;
        }

        [[nodiscard]]
        grab::Result<grab::EventKind>
        from_wire_kind( eventgrab::v1::EventKind kind )
        {
            switch( kind )
            {
                case eventgrab::v1::INPUT_KEY_DOWN :
                    return grab::EventKind::key_down;
                case eventgrab::v1::INPUT_KEY_UP :
                    return grab::EventKind::key_up;
                case eventgrab::v1::INPUT_KEY_COMBO :
                    return grab::EventKind::key_combo;
                case eventgrab::v1::INPUT_MOUSE_CLICK :
                    return grab::EventKind::mouse_click;
                case eventgrab::v1::INPUT_MOUSE_MOVE :
                    return grab::EventKind::mouse_move;
                case eventgrab::v1::INPUT_IDLE_START :
                    return grab::EventKind::idle_start;
                case eventgrab::v1::INPUT_IDLE_END :
                    return grab::EventKind::idle_end;
                case eventgrab::v1::WINDOW_FOCUS_CHANGED :
                    return grab::EventKind::window_focus_changed;
                case eventgrab::v1::WINDOW_TITLE_CHANGED :
                    return grab::EventKind::window_title_changed;
                case eventgrab::v1::WINDOW_CREATED :
                    return grab::EventKind::window_created;
                case eventgrab::v1::WINDOW_CLOSED :
                    return grab::EventKind::window_closed;
                case eventgrab::v1::A11Y_BUTTON_CLICKED :
                    return grab::EventKind::a11y_button_clicked;
                case eventgrab::v1::A11Y_MENU_OPENED :
                    return grab::EventKind::a11y_menu_opened;
                case eventgrab::v1::A11Y_MENU_CLOSED :
                    return grab::EventKind::a11y_menu_closed;
                case eventgrab::v1::A11Y_FOCUS_CHANGED :
                    return grab::EventKind::a11y_focus_changed;
                case eventgrab::v1::A11Y_TEXT_CHANGED :
                    return grab::EventKind::a11y_text_changed;
                case eventgrab::v1::A11Y_STATE_CHANGED :
                    return grab::EventKind::a11y_state_changed;
                case eventgrab::v1::APP_TAB_CHANGED :
                    return grab::EventKind::app_tab_changed;
                case eventgrab::v1::APP_CONTEXT_UPDATE :
                    return grab::EventKind::app_context_update;
                case eventgrab::v1::BROWSER_TAB_SWITCHED :
                    return grab::EventKind::browser_tab_switched;
                case eventgrab::v1::STATE_SNAPSHOT :
                    return grab::EventKind::state_snapshot;
                case eventgrab::v1::EVENT_KIND_UNSPECIFIED :
                default :
                    return protocol_error( "unknown event kind" );
            }
        }

        [[nodiscard]]
        eventgrab::v1::EventCategory
        to_wire_category( grab::EventCategory category )
        {
            switch( category )
            {
                case grab::EventCategory::input :
                    return eventgrab::v1::EVENT_CATEGORY_INPUT;
                case grab::EventCategory::window :
                    return eventgrab::v1::EVENT_CATEGORY_WINDOW;
                case grab::EventCategory::accessibility :
                    return eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY;
                case grab::EventCategory::integration :
                    return eventgrab::v1::EVENT_CATEGORY_INTEGRATION;
                case grab::EventCategory::browser :
                    return eventgrab::v1::EVENT_CATEGORY_BROWSER;
                case grab::EventCategory::state :
                    return eventgrab::v1::EVENT_CATEGORY_STATE;
                case grab::EventCategory::unspecified :
                    return eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED;
            }

            return eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED;
        }

        [[nodiscard]]
        grab::Result<grab::EventCategory>
        from_wire_category( eventgrab::v1::EventCategory category )
        {
            switch( category )
            {
                case eventgrab::v1::EVENT_CATEGORY_INPUT :
                    return grab::EventCategory::input;
                case eventgrab::v1::EVENT_CATEGORY_WINDOW :
                    return grab::EventCategory::window;
                case eventgrab::v1::EVENT_CATEGORY_ACCESSIBILITY :
                    return grab::EventCategory::accessibility;
                case eventgrab::v1::EVENT_CATEGORY_INTEGRATION :
                    return grab::EventCategory::integration;
                case eventgrab::v1::EVENT_CATEGORY_BROWSER :
                    return grab::EventCategory::browser;
                case eventgrab::v1::EVENT_CATEGORY_STATE :
                    return grab::EventCategory::state;
                case eventgrab::v1::EVENT_CATEGORY_UNSPECIFIED :
                    return grab::EventCategory::unspecified;
                default :
                    return protocol_error( "unknown event category" );
            }
        }

        [[nodiscard]]
        std::string
        uint_to_string( std::uint32_t value )
        {
            return std::to_string( value );
        }

        [[nodiscard]]
        std::string
        double_to_string( double value )
        {
            std::ostringstream stream;
            stream << std::setprecision( std::numeric_limits<double>::max_digits10 )
                   << value;
            return stream.str();
        }

        [[nodiscard]]
        bool
        parse_uint32( std::string_view text,
                      std::uint32_t&   value ) noexcept
        {
            if( text.empty() )
            {
                return false;
            }

            std::uint32_t parsed     = 0U;
            const auto*   begin      = text.data();
            const auto    distance   = static_cast<std::ptrdiff_t>( text.size() );
            const auto*   end        = std::next( begin, distance );
            const auto    result     = std::from_chars( begin, end, parsed );
            const bool    all_parsed = result.ptr == end;
            if( result.ec != std::errc{} || !all_parsed )
            {
                return false;
            }

            value = parsed;
            return true;
        }

        [[nodiscard]]
        bool
        parse_double( std::string_view text,
                      double&          value ) noexcept
        {
            if( text.empty() )
            {
                return false;
            }

            double      parsed     = 0.0;
            const auto* begin      = text.data();
            const auto  distance   = static_cast<std::ptrdiff_t>( text.size() );
            const auto* end        = std::next( begin, distance );
            const auto  result     = std::from_chars( begin, end, parsed );
            const bool  all_parsed = result.ptr == end;
            if( result.ec != std::errc{} || !all_parsed )
            {
                return false;
            }

            value = parsed;
            return true;
        }

        [[nodiscard]]
        grab::Result<std::string>
        required_string( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            const auto& data = wire.data();
            const auto  iter = data.find( std::string{ key } );
            if( iter == data.end() )
            {
                return protocol_error( "missing data key " + std::string{ key } );
            }
            return iter->second;
        }

        [[nodiscard]]
        std::string
        optional_string( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            const auto& data = wire.data();
            const auto  iter = data.find( std::string{ key } );
            if( iter == data.end() )
            {
                return {};
            }
            return iter->second;
        }

        [[nodiscard]]
        grab::Result<std::uint32_t>
        required_uint32( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            auto text = required_string( wire, key );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            std::uint32_t value = 0U;
            if( !parse_uint32( *text, value ) )
            {
                return protocol_error( "unparseable unsigned integer key " +
                                       std::string{ key } );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<double>
        required_double( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            auto text = required_string( wire, key );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            double value = 0.0;
            if( !parse_double( *text, value ) )
            {
                return protocol_error( "unparseable floating point key " +
                                       std::string{ key } );
            }
            return value;
        }

        [[nodiscard]]
        grab::Result<double>
        optional_double( const eventgrab::v1::Event& wire,
                         std::string_view            key )
        {
            const auto& data = wire.data();
            const auto  iter = data.find( std::string{ key } );
            if( iter == data.end() )
            {
                return 0.0;
            }

            double value = 0.0;
            if( !parse_double( iter->second, value ) )
            {
                return protocol_error( "unparseable floating point key " +
                                       std::string{ key } );
            }
            return value;
        }

        void
        set_data( eventgrab::v1::Event& wire,
                  std::string_view      key,
                  std::string           value )
        {
            auto* const data = wire.mutable_data();
            auto [iter, inserted] =
                data->try_emplace( std::string{ key }, std::move( value ) );
            if( !inserted )
            {
                iter->second = std::move( value );
            }
        }

        void
        encode_input_key( eventgrab::v1::Event& wire,
                          const grab::InputKey& payload )
        {
            set_data( wire, kKeyCode, uint_to_string( payload.code ) );
            set_data( wire, kKeyName, payload.name );
        }

        void
        encode_key_combo( eventgrab::v1::Event& wire,
                          const grab::KeyCombo& payload )
        {
            set_data( wire, kText, payload.text );
        }

        void
        encode_mouse_click( eventgrab::v1::Event&   wire,
                            const grab::MouseClick& payload )
        {
            set_data( wire, kButton, uint_to_string( payload.button ) );
            set_data( wire, kButtonName, payload.name );
        }

        void
        encode_mouse_move( eventgrab::v1::Event&  wire,
                           const grab::MouseMove& payload )
        {
            set_data( wire, kAxis, payload.axis );
            set_data( wire, kDelta, double_to_string( payload.delta ) );
        }

        void
        encode_idle( eventgrab::v1::Event& wire,
                     const grab::Idle&     payload )
        {
            set_data( wire, kIdleS, double_to_string( payload.idle_s ) );
        }

        void
        encode_window_change( eventgrab::v1::Event&     wire,
                              const grab::WindowChange& payload )
        {
            set_data( wire, kApp, payload.app );
            set_data( wire, kPid, payload.pid );
            set_data( wire, kTitle, payload.title );
            set_data( wire, kPrevTitle, payload.prev_title );
            set_data( wire, kDurationS, double_to_string( payload.duration_s ) );
        }

        void
        encode_a11y_event( eventgrab::v1::Event&  wire,
                           grab::EventKind        kind,
                           const grab::A11yEvent& payload )
        {
            set_data( wire, kApp, payload.app );
            set_data( wire, kRole, payload.role );
            set_data( wire, kName, payload.name );
            if( kind == grab::EventKind::a11y_state_changed )
            {
                set_data( wire, kState, payload.detail );
                return;
            }
            set_data( wire, kDetail, payload.detail );
        }

        void
        encode_integration_event( eventgrab::v1::Event&         wire,
                                  const grab::IntegrationEvent& payload )
        {
            set_data( wire, kApp, payload.app );
            set_data( wire, kTitle, payload.title );
            set_data( wire, kDetail, payload.detail );
            set_data( wire, kJson, payload.json );
        }

        void
        encode_browser_tab( eventgrab::v1::Event&   wire,
                            const grab::BrowserTab& payload )
        {
            set_data( wire, kApp, payload.app );
            set_data( wire, kPid, payload.pid );
            set_data( wire, kTabTitle, payload.tab_title );
            set_data( wire, kPrevTabTitle, payload.prev_tab_title );
        }

        void
        encode_state_snapshot( eventgrab::v1::Event&      wire,
                               const grab::StateSnapshot& payload )
        {
            set_data( wire, kJson, payload.json );
        }

        [[nodiscard]]
        grab::Result<void>
        validate_size( const eventgrab::v1::Event& wire )
        {
            if( wire.data_size() > kMaxDataEntries )
            {
                return protocol_error( "too many data entries" );
            }

            for( const auto& [key, value] : wire.data() )
            {
                if( key.size() >
                    static_cast<std::size_t>( kMaxValueBytes ) ||
                    value.size() > static_cast<std::size_t>( kMaxValueBytes ) )
                {
                    return protocol_error( "data entry exceeds maximum byte size" );
                }
            }

            return {};
        }

        [[nodiscard]]
        grab::Result<grab::InputKey>
        decode_input_key( const eventgrab::v1::Event& wire )
        {
            auto code = required_uint32( wire, kKeyCode );
            if( !code.has_value() )
            {
                return std::unexpected( code.error() );
            }

            return grab::InputKey{
                .code = *code,
                .name = optional_string( wire, kKeyName ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::KeyCombo>
        decode_key_combo( const eventgrab::v1::Event& wire )
        {
            auto text = required_string( wire, kText );
            if( !text.has_value() )
            {
                return std::unexpected( text.error() );
            }

            return grab::KeyCombo{ .text = *text };
        }

        [[nodiscard]]
        grab::Result<grab::MouseClick>
        decode_mouse_click( const eventgrab::v1::Event& wire )
        {
            auto button = required_uint32( wire, kButton );
            if( !button.has_value() )
            {
                return std::unexpected( button.error() );
            }

            return grab::MouseClick{
                .button = *button,
                .name   = optional_string( wire, kButtonName ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::MouseMove>
        decode_mouse_move( const eventgrab::v1::Event& wire )
        {
            auto axis = required_string( wire, kAxis );
            if( !axis.has_value() )
            {
                return std::unexpected( axis.error() );
            }

            auto delta = required_double( wire, kDelta );
            if( !delta.has_value() )
            {
                return std::unexpected( delta.error() );
            }

            return grab::MouseMove{ .axis = *axis, .delta = *delta };
        }

        [[nodiscard]]
        grab::Result<grab::Idle>
        decode_idle( const eventgrab::v1::Event& wire,
                     grab::EventKind             kind )
        {
            auto idle_s = kind == grab::EventKind::idle_start
                            ? required_double( wire, kIdleS )
                            : optional_double( wire, kIdleS );
            if( !idle_s.has_value() )
            {
                return std::unexpected( idle_s.error() );
            }

            return grab::Idle{ .idle_s = *idle_s };
        }

        [[nodiscard]]
        grab::Result<grab::WindowChange>
        decode_window_change( const eventgrab::v1::Event& wire,
                              grab::EventKind             kind )
        {
            auto app = required_string( wire, kApp );
            if( !app.has_value() )
            {
                return std::unexpected( app.error() );
            }
            auto pid = required_string( wire, kPid );
            if( !pid.has_value() )
            {
                return std::unexpected( pid.error() );
            }
            auto title = required_string( wire, kTitle );
            if( !title.has_value() )
            {
                return std::unexpected( title.error() );
            }
            const auto prev_title = optional_string( wire, kPrevTitle );
            auto       duration_s = kind == grab::EventKind::window_closed
                                      ? required_double( wire, kDurationS )
                                      : optional_double( wire, kDurationS );
            if( !duration_s.has_value() )
            {
                return std::unexpected( duration_s.error() );
            }

            return grab::WindowChange{
                .app        = *app,
                .pid        = *pid,
                .title      = *title,
                .prev_title = prev_title,
                .duration_s = *duration_s,
            };
        }

        [[nodiscard]]
        grab::Result<grab::A11yEvent>
        decode_a11y_event( const eventgrab::v1::Event& wire,
                           grab::EventKind             kind )
        {
            auto name = required_string( wire, kName );
            if( !name.has_value() )
            {
                return std::unexpected( name.error() );
            }

            if( kind == grab::EventKind::a11y_focus_changed )
            {
                auto app = required_string( wire, kApp );
                if( !app.has_value() )
                {
                    return std::unexpected( app.error() );
                }
                auto role = required_string( wire, kRole );
                if( !role.has_value() )
                {
                    return std::unexpected( role.error() );
                }

                return grab::A11yEvent{
                    .app    = *app,
                    .role   = *role,
                    .name   = *name,
                    .detail = optional_string( wire, kDetail ),
                };
            }

            if( kind == grab::EventKind::a11y_state_changed )
            {
                auto detail = required_string( wire, kState );
                if( !detail.has_value() )
                {
                    return std::unexpected( detail.error() );
                }

                return grab::A11yEvent{
                    .app    = optional_string( wire, kApp ),
                    .role   = optional_string( wire, kRole ),
                    .name   = *name,
                    .detail = *detail,
                };
            }

            return grab::A11yEvent{
                .app    = optional_string( wire, kApp ),
                .role   = optional_string( wire, kRole ),
                .name   = *name,
                .detail = optional_string( wire, kDetail ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::IntegrationEvent>
        decode_integration_event( const eventgrab::v1::Event& wire )
        {
            auto app = required_string( wire, kApp );
            if( !app.has_value() )
            {
                return std::unexpected( app.error() );
            }
            auto title = required_string( wire, kTitle );
            if( !title.has_value() )
            {
                return std::unexpected( title.error() );
            }
            auto detail = required_string( wire, kDetail );
            if( !detail.has_value() )
            {
                return std::unexpected( detail.error() );
            }
            auto json = required_string( wire, kJson );
            if( !json.has_value() )
            {
                return std::unexpected( json.error() );
            }

            return grab::IntegrationEvent{
                .app    = *app,
                .title  = *title,
                .detail = *detail,
                .json   = *json,
            };
        }

        [[nodiscard]]
        grab::Result<grab::BrowserTab>
        decode_browser_tab( const eventgrab::v1::Event& wire )
        {
            auto app = required_string( wire, kApp );
            if( !app.has_value() )
            {
                return std::unexpected( app.error() );
            }
            auto pid = required_string( wire, kPid );
            if( !pid.has_value() )
            {
                return std::unexpected( pid.error() );
            }
            auto tab_title = required_string( wire, kTabTitle );
            if( !tab_title.has_value() )
            {
                return std::unexpected( tab_title.error() );
            }
            return grab::BrowserTab{
                .app            = *app,
                .pid            = *pid,
                .tab_title      = *tab_title,
                .prev_tab_title = optional_string( wire, kPrevTabTitle ),
            };
        }

        [[nodiscard]]
        grab::Result<grab::StateSnapshot>
        decode_state_snapshot( const eventgrab::v1::Event& wire )
        {
            auto json = required_string( wire, kJson );
            if( !json.has_value() )
            {
                return std::unexpected( json.error() );
            }

            return grab::StateSnapshot{ .json = *json };
        }

        [[nodiscard]]
        grab::Result<grab::Payload>
        decode_payload( const eventgrab::v1::Event& wire,
                        grab::EventKind             kind )
        {
            switch( kind )
            {
                case grab::EventKind::key_down :
                case grab::EventKind::key_up :
                    return decode_input_key( wire );
                case grab::EventKind::key_combo :
                    return decode_key_combo( wire );
                case grab::EventKind::mouse_click :
                    return decode_mouse_click( wire );
                case grab::EventKind::mouse_move :
                    return decode_mouse_move( wire );
                case grab::EventKind::idle_start :
                case grab::EventKind::idle_end :
                    return decode_idle( wire, kind );
                case grab::EventKind::window_focus_changed :
                case grab::EventKind::window_title_changed :
                case grab::EventKind::window_created :
                case grab::EventKind::window_closed :
                    return decode_window_change( wire, kind );
                case grab::EventKind::a11y_button_clicked :
                case grab::EventKind::a11y_menu_opened :
                case grab::EventKind::a11y_menu_closed :
                case grab::EventKind::a11y_focus_changed :
                case grab::EventKind::a11y_text_changed :
                case grab::EventKind::a11y_state_changed :
                    return decode_a11y_event( wire, kind );
                case grab::EventKind::app_tab_changed :
                case grab::EventKind::app_context_update :
                    return decode_integration_event( wire );
                case grab::EventKind::browser_tab_switched :
                    return decode_browser_tab( wire );
                case grab::EventKind::state_snapshot :
                    return decode_state_snapshot( wire );
                case grab::EventKind::unspecified :
                    return protocol_error( "unknown event kind" );
            }

            return protocol_error( "unknown event kind" );
        }

    }    // namespace

    grab::Result<eventgrab::v1::Event>
    to_wire( const grab::Event& event )
    {
        if( event.kind == grab::EventKind::unspecified )
        {
            return protocol_error( "unknown event kind" );
        }

        eventgrab::v1::Event wire;
        wire.set_kind( to_wire_kind( event.kind ) );
        wire.set_category( to_wire_category( event.category ) );
        wire.set_timestamp( event.timestamp );

        switch( event.kind )
        {
            case grab::EventKind::key_down :
            case grab::EventKind::key_up :
                encode_input_key( wire, std::get<grab::InputKey>( event.payload ) );
                break;
            case grab::EventKind::key_combo :
                encode_key_combo( wire, std::get<grab::KeyCombo>( event.payload ) );
                break;
            case grab::EventKind::mouse_click :
                encode_mouse_click( wire, std::get<grab::MouseClick>( event.payload ) );
                break;
            case grab::EventKind::mouse_move :
                encode_mouse_move( wire, std::get<grab::MouseMove>( event.payload ) );
                break;
            case grab::EventKind::idle_start :
            case grab::EventKind::idle_end :
                encode_idle( wire, std::get<grab::Idle>( event.payload ) );
                break;
            case grab::EventKind::window_focus_changed :
            case grab::EventKind::window_title_changed :
            case grab::EventKind::window_created :
            case grab::EventKind::window_closed :
                encode_window_change( wire,
                                      std::get<grab::WindowChange>( event.payload ) );
                break;
            case grab::EventKind::a11y_button_clicked :
            case grab::EventKind::a11y_menu_opened :
            case grab::EventKind::a11y_menu_closed :
            case grab::EventKind::a11y_focus_changed :
            case grab::EventKind::a11y_text_changed :
            case grab::EventKind::a11y_state_changed :
                encode_a11y_event( wire,
                                   event.kind,
                                   std::get<grab::A11yEvent>( event.payload ) );
                break;
            case grab::EventKind::app_tab_changed :
            case grab::EventKind::app_context_update :
                encode_integration_event(
                    wire,
                    std::get<grab::IntegrationEvent>( event.payload )
                );
                break;
            case grab::EventKind::browser_tab_switched :
                encode_browser_tab( wire, std::get<grab::BrowserTab>( event.payload ) );
                break;
            case grab::EventKind::state_snapshot :
                encode_state_snapshot( wire,
                                       std::get<grab::StateSnapshot>( event.payload ) );
                break;
            case grab::EventKind::unspecified :
                return protocol_error( "unknown event kind" );
        }

        return wire;
    }

    grab::Result<grab::Event>
    from_wire( const eventgrab::v1::Event& wire )
    {
        auto size_status = validate_size( wire );
        if( !size_status.has_value() )
        {
            return std::unexpected( size_status.error() );
        }

        auto kind = from_wire_kind( wire.kind() );
        if( !kind.has_value() )
        {
            return std::unexpected( kind.error() );
        }

        auto category = from_wire_category( wire.category() );
        if( !category.has_value() )
        {
            return std::unexpected( category.error() );
        }

        auto payload = decode_payload( wire, *kind );
        if( !payload.has_value() )
        {
            return std::unexpected( payload.error() );
        }

        return grab::Event{
            .timestamp = wire.timestamp(),
            .sequence  = kNoSequence,
            .kind      = *kind,
            .category  = *category,
            .payload   = std::move( *payload ),
        };
    }

}    // namespace grab::transport
