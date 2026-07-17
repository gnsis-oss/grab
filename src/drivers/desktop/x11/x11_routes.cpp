#include "drivers/desktop/x11/x11_drag_recipe.hpp"
#include "drivers/desktop/x11/x11_routes.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/query.hpp"
#include "grab/ui.hpp"
#include "kernel/action/wait_engine.hpp"
#include "kernel/query/snapshot_tree_nav.hpp"
#include "platform/x11/protocol.hpp"
#include "spi/tree_source.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

namespace grab::drivers::desktop::x11
{
    namespace
    {

        constexpr std::uint8_t  activationFormat32Bits    = 32U;
        constexpr std::uint8_t  activationNoPropagate     = 0U;
        constexpr std::uint32_t activationSourceNormal    = 1U;
        constexpr std::uint32_t activationNoCurrentWindow = 0U;
        constexpr std::uint32_t activationStackAbove      = XCB_STACK_MODE_ABOVE;
        constexpr std::uint32_t activationEventMask =
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY ) |
            static_cast<std::uint32_t>( XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT );

        template<typename Type>
        using XcbOwned = std::unique_ptr<Type, decltype( &std::free )>;

        template<typename Type>
        [[nodiscard]]
        XcbOwned<Type>
        take_xcb_owned( Type* pointer ) noexcept
        {
            return XcbOwned<Type>{ pointer, &std::free };
        }

        [[nodiscard]]
        grab::Result<void>
        check_request( xcb_connection_t* connection,
                       xcb_void_cookie_t cookie,
                       std::string_view  operation )
        {
            const auto error = take_xcb_owned( xcb_request_check( connection, cookie ) );
            if( error != nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   std::string{ operation } + " failed" );
            }
            return {};
        }

        [[nodiscard]]
        grab::Result<xcb_atom_t>
        intern_atom( xcb_connection_t* connection,
                     std::string_view  name,
                     bool              only_if_exists )
        {
            xcb_generic_error_t* raw_error = nullptr;
            const auto           reply     = take_xcb_owned( xcb_intern_atom_reply(
                connection,
                xcb_intern_atom( connection,
                                 only_if_exists ? 1U : 0U,
                                 static_cast<std::uint16_t>( name.size() ),
                                 name.data() ),
                &raw_error
            ) );
            const auto           error     = take_xcb_owned( raw_error );
            if( error != nullptr || reply == nullptr )
            {
                return grab::fail( grab::ErrorCode::ProtocolError,
                                   "X11 activation atom lookup failed for " +
                                       std::string{ name } );
            }
            return reply->atom;
        }

        template<typename Value>
        struct IsVariant : std::false_type
        {
        };

        template<typename... Values>
        struct IsVariant<std::variant<Values...>> : std::true_type
        {
        };

        template<typename Value>
        [[nodiscard]]
        std::optional<grab::WidgetRef>
        widget_ref_from( const Value& value )
        {
            using Type = std::remove_cvref_t<Value>;

            if constexpr( std::is_same_v<Type, grab::WidgetRef> )
            {
                return value;
            }
            else if constexpr( IsVariant<Type>::value )
            {
                return std::visit(
                    []( const auto& alternative )
                    {
                        return widget_ref_from( alternative );
                    },
                    value
                );
            }
            else if constexpr( requires { value.widget; } )
            {
                return widget_ref_from( value.widget );
            }
            else if constexpr( requires { value.ref; } )
            {
                return widget_ref_from( value.ref );
            }
            else if constexpr( requires { value.widget_ref; } )
            {
                return widget_ref_from( value.widget_ref );
            }
            else if constexpr( requires { value.reference; } )
            {
                return widget_ref_from( value.reference );
            }
            else if constexpr( requires {
                                   value.has_value();
                                   *value;
                               } )
            {
                if( value.has_value() )
                {
                    return widget_ref_from( *value );
                }
                return std::nullopt;
            }
            else
            {
                return std::nullopt;
            }
        }

        template<typename Value>
        [[nodiscard]]
        grab::Result<Value>
        failure( grab::ErrorCode code,
                 std::string     message )
        {
            return std::unexpected{
                grab::Error{
                            .code       = code,
                            .message    = std::move( message ),
                            .capability = {},
                            .target     = {},
                            .attempts   = {},
                            }
            };
        }

        [[nodiscard,
          maybe_unused]]
        grab::Result<std::vector<std::uint8_t>>
        modifier_keycodes( xcb_connection_t* connection )
        {
            if( connection == nullptr )
            {
                return failure<std::vector<std::uint8_t>>(
                    grab::ErrorCode::CapabilityUnavailable,
                    "X11 connection is unavailable"
                );
            }

            xcb_generic_error_t* error = nullptr;
            auto*                reply =
                xcb_get_modifier_mapping_reply( connection,
                                                xcb_get_modifier_mapping( connection ),
                                                &error );
            if( error != nullptr || reply == nullptr )
            {
                std::free( error );
                std::free( reply );
                return failure<std::vector<std::uint8_t>>(
                    grab::ErrorCode::ProtocolError,
                    "Could not query the X11 modifier map"
                );
            }

            std::array<bool, 256U>    seen{};
            std::vector<std::uint8_t> result;
            const auto  count    = xcb_get_modifier_mapping_keycodes_length( reply );
            const auto* keycodes = xcb_get_modifier_mapping_keycodes( reply );
            result.reserve( static_cast<std::size_t>( count ) );
            for( int index = 0; index < count; ++index )
            {
                const auto keycode = keycodes[index];
                if( keycode != 0U && !seen[keycode] )
                {
                    seen[keycode] = true;
                    result.push_back( keycode );
                }
            }
            std::free( reply );
            return result;
        }

        [[nodiscard,
          maybe_unused]]
        grab::Result<std::vector<char32_t>>
        decode_utf8( std::string_view text )
        {
            std::vector<char32_t> result;
            result.reserve( text.size() );

            for( std::size_t offset = 0U; offset < text.size(); )
            {
                const auto    lead      = static_cast<std::uint8_t>( text[offset] );
                std::size_t   length    = 0U;
                std::uint32_t codepoint = 0U;
                std::uint32_t minimum   = 0U;

                if( lead <= 0X7FU )
                {
                    length    = 1U;
                    codepoint = lead;
                }
                else if( lead >= 0XC2U && lead <= 0XDFU )
                {
                    length    = 2U;
                    codepoint = lead & 0X1FU;
                    minimum   = 0X80U;
                }
                else if( lead >= 0XE0U && lead <= 0XEFU )
                {
                    length    = 3U;
                    codepoint = lead & 0X0FU;
                    minimum   = 0X8'00U;
                }
                else if( lead >= 0XF0U && lead <= 0XF4U )
                {
                    length    = 4U;
                    codepoint = lead & 0X07U;
                    minimum   = 0X1'00'00U;
                }
                else
                {
                    return failure<std::vector<char32_t>>(
                        grab::ErrorCode::InvalidArgument,
                        "Text contains invalid UTF-8"
                    );
                }

                if( offset + length > text.size() )
                {
                    return failure<std::vector<char32_t>>(
                        grab::ErrorCode::InvalidArgument,
                        "Text contains truncated UTF-8"
                    );
                }
                for( std::size_t index = 1U; index < length; ++index )
                {
                    const auto continuation =
                        static_cast<std::uint8_t>( text[offset + index] );
                    if( ( continuation & 0XC0U ) != 0X80U )
                    {
                        return failure<std::vector<char32_t>>(
                            grab::ErrorCode::InvalidArgument,
                            "Text contains invalid UTF-8"
                        );
                    }
                    codepoint = ( codepoint << 6U ) | ( continuation & 0X3FU );
                }

                if( codepoint <
                    minimum ||
                    codepoint >
                    0X10'FF'FFU ||
                    ( codepoint >= 0XD8'00U && codepoint <= 0XDF'FFU ) )
                {
                    return failure<std::vector<char32_t>>(
                        grab::ErrorCode::InvalidArgument,
                        "Text contains an invalid Unicode scalar value"
                    );
                }

                result.push_back( static_cast<char32_t>( codepoint ) );
                offset += length;
            }
            return result;
        }

        [[nodiscard,
          maybe_unused]]
        constexpr std::uint32_t
        unicode_keysym( char32_t codepoint ) noexcept
        {
            const auto value = static_cast<std::uint32_t>( codepoint );
            if( ( value >= 0X20U && value <= 0X7EU ) ||
                ( value >= 0XA0U && value <= 0XFFU ) )
            {
                return value;
            }
            return 0X01'00'00'00U | value;
        }

        template<typename ResultType>
        [[nodiscard]]
        grab::Result<void>
        forward_error( ResultType&& result )
        {
            return std::unexpected{ std::forward<ResultType>( result ).error() };
        }

        [[nodiscard]]
        grab::Result<void>
        possibly_committed()
        {
            return failure<void>(
                grab::ErrorCode::PossiblyCommitted,
                "X11 input failed after crossing the input commit boundary"
            );
        }

        class ReservationBase : public grab::spi::RouteReservation
        {
            public:

                [[nodiscard]]
                std::span<const std::string_view>
                barriers() const noexcept final
                {
                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                arm_barrier( std::string_view,
                             const grab::OperationContext& ) final
                {
                    return failure<void>( grab::ErrorCode::InvalidArgument,
                                          "X11 route does not advertise barriers" );
                }

                [[nodiscard]]
                grab::Result<std::vector<grab::BarrierOutcome>>
                settle( const grab::OperationContext& ) final
                {
                    return std::vector<grab::BarrierOutcome>{};
                }

                [[nodiscard]]
                grab::Result<void>
                verify( const grab::OperationContext& ) override
                {
                    return {};
                }
        };

        class PointerReservation final : public ReservationBase
        {
            public:

                PointerReservation( X11TreeSource&         source,
                                    xcb_connection_t*      connection,
                                    xcb_window_t           root,
                                    X11InputSeat&          seat,
                                    const grab::WidgetRef& target ) :
                    source_( &source ),
                    connection_( connection ),
                    root_( root ),
                    seat_( &seat ),
                    target_( target )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                commit( const grab::OperationContext& ) final
                {
                    const auto xid = source_->resolve_xid( target_ );
                    if( !xid )
                    {
                        return forward_error( xid );
                    }

                    if( connection_ == nullptr || root_ == XCB_WINDOW_NONE )
                    {
                        return failure<void>( grab::ErrorCode::CapabilityUnavailable,
                                              "X11 connection is unavailable" );
                    }

                    const auto geometry_cookie = xcb_get_geometry( connection_, *xid );
                    auto*      geometry =
                        xcb_get_geometry_reply( connection_, geometry_cookie, nullptr );
                    if( geometry == nullptr )
                    {
                        return failure<void>(
                            grab::ErrorCode::CapabilityUnavailable,
                            "Could not query the X11 target geometry"
                        );
                    }

                    const auto center_x =
                        static_cast<std::int16_t>( geometry->width / 2U );
                    const auto center_y =
                        static_cast<std::int16_t>( geometry->height / 2U );
                    std::free( geometry );

                    const auto translate_cookie = xcb_translate_coordinates( connection_,
                                                                             *xid,
                                                                             root_,
                                                                             center_x,
                                                                             center_y );
                    auto* translated = xcb_translate_coordinates_reply( connection_,
                                                                        translate_cookie,
                                                                        nullptr );
                    if( translated == nullptr )
                    {
                        return failure<void>(
                            grab::ErrorCode::CapabilityUnavailable,
                            "Could not translate the X11 target coordinates"
                        );
                    }

                    const auto target_x = translated->dst_x;
                    const auto target_y = translated->dst_y;
                    std::free( translated );

                    auto result = seat_->move_pointer_absolute( target_x, target_y );
                    if( !result )
                    {
                        return result;
                    }

                    result = seat_->button( 1U, true );
                    if( !result )
                    {
                        return possibly_committed();
                    }

                    result = seat_->button( 1U, false );
                    if( !result )
                    {
                        return possibly_committed();
                    }

                    result = seat_->flush();
                    if( !result )
                    {
                        return possibly_committed();
                    }

                    return {};
                }

            private:

                X11TreeSource*    source_{};
                xcb_connection_t* connection_{};
                xcb_window_t      root_{};
                X11InputSeat*     seat_{};
                grab::WidgetRef   target_{};
        };

        class ActivationReservation final : public ReservationBase
        {
            public:

                ActivationReservation( X11TreeSource&          source,
                                       xcb_connection_t*       connection,
                                       xcb_window_t            root,
                                       grab::spi::EventSource& events,
                                       grab::Match             match,
                                       grab::WidgetRef         target_ref ) :
                    source_( &source ),
                    connection_( connection ),
                    root_( root ),
                    events_( &events ),
                    match_( std::move( match ) ),
                    target_ref_( target_ref )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                commit( const grab::OperationContext& ) final
                {
                    const auto xid = source_->resolve_xid( target_ref_ );
                    if( !xid )
                    {
                        return forward_error( xid );
                    }

                    if( connection_ == nullptr || root_ == XCB_WINDOW_NONE )
                    {
                        return failure<void>( grab::ErrorCode::CapabilityUnavailable,
                                              "X11 connection is unavailable" );
                    }

                    auto atom =
                        intern_atom( connection_,
                                     grab::platform::x11::atom_name::netActiveWindow,
                                     false );
                    if( !atom.has_value() )
                    {
                        return std::unexpected( std::move( atom.error() ) );
                    }

                    const xcb_client_message_event_t event{
                        .response_type = XCB_CLIENT_MESSAGE,
                        .format        = activationFormat32Bits,
                        .sequence      = 0U,
                        .window        = *xid,
                        .type          = *atom,
                        .data          = xcb_client_message_data_t{
                                                                   .data32 = {
                                activationSourceNormal,
                                XCB_CURRENT_TIME,
                                activationNoCurrentWindow,
                            }, },
                    };
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    const auto* const raw_event =
                        reinterpret_cast<const char*>( &event );
                    auto send_result =
                        check_request( connection_,
                                       xcb_send_event_checked( connection_,
                                                               activationNoPropagate,
                                                               root_,
                                                               activationEventMask,
                                                               raw_event ),
                                       "X11 active-window request" );
                    if( !send_result.has_value() )
                    {
                        return send_result;
                    }

                    auto stack_result = check_request(
                        connection_,
                        xcb_configure_window_checked( connection_,
                                                      *xid,
                                                      XCB_CONFIG_WINDOW_STACK_MODE,
                                                      &activationStackAbove ),
                        "X11 window raise request"
                    );
                    if( !stack_result.has_value() )
                    {
                        return stack_result;
                    }

                    if( xcb_flush( connection_ ) <= 0 )
                    {
                        return failure<void>( grab::ErrorCode::ProtocolError,
                                              "X11 window activation flush failed" );
                    }

                    return {};
                }

                [[nodiscard]]
                grab::Result<void>
                verify( const grab::OperationContext& context ) final
                {
                    namespace ka            = grab::kernel::action;
                    const auto       match  = match_;
                    auto* const      source = source_;
                    ka::NodeObserver observer =
                        [source, match, context]() -> grab::Result<ka::NodeObservation>
                    {
                        auto snapshot = source->snapshot( match.ref.tree, context );
                        if( !snapshot.has_value() )
                        {
                            return std::unexpected( std::move( snapshot.error() ) );
                        }
                        const grab::kernel::query::SnapshotTreeNav navigation{
                            *snapshot
                        };
                        const auto         metadata = navigation.metadata();
                        const grab::NodeId node{ match.ref.node };
                        const bool         present = metadata.runtime ==
                                                     match.ref.runtime &&
                                                     metadata.tree ==
                                                     match.ref.tree &&
                                                     metadata.epoch ==
                                                     match.ref.epoch &&
                                                     navigation.contains( node ) &&
                                                     navigation.generation( node ) ==
                                                     match.ref.generation;
                        return ka::NodeObservation{
                            .present = present,
                            .states  = present ? navigation.states( node ) : 0U,
                            .detail  = present ? "activation target present"
                                               : "activation target is stale or absent",
                        };
                    };
                    auto                 predicate = ka::window_mapped( observer );
                    const ka::WaitEngine engine{ context };
                    return engine.wait( predicate,
                                        ka::WaitParams{
                                            .deadline = context.deadline,
                                        },
                                        *events_ );
                }

            private:

                X11TreeSource*          source_{};
                xcb_connection_t*       connection_{};
                xcb_window_t            root_{};
                grab::spi::EventSource* events_{};
                grab::Match             match_{};
                grab::WidgetRef         target_ref_{};
        };

        class KeyboardReservation final : public ReservationBase
        {
            public:

                KeyboardReservation( X11InputSeat& seat,
                                     grab::Keymap& keymap,
                                     std::string   text ) :
                    seat_( &seat ),
                    keymap_( &keymap ),
                    text_( std::move( text ) )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                commit( const grab::OperationContext& ) final
                {
                    const auto keystrokes = keymap_->text_to_keystrokes( text_ );
                    if( !keystrokes )
                    {
                        return forward_error( keystrokes );
                    }

                    const auto     shift = keymap_->shift_keycode();
                    const auto     altgr = keymap_->altgr_keycode();
                    constexpr auto max_keycode =
                        std::numeric_limits<std::uint8_t>::max();

                    for( const auto& stroke : *keystrokes )
                    {
                        if( stroke.keycode >
                            max_keycode ||
                            ( stroke.shift && shift > max_keycode ) ||
                            ( stroke.altgr && altgr > max_keycode ) )
                        {
                            return failure<void>(
                                grab::ErrorCode::InvalidArgument,
                                "X11 keycode is outside the supported range"
                            );
                        }
                    }

                    bool       begun = false;
                    const auto send  = [this, &begun]( std::uint32_t keycode,
                                                       bool press ) -> grab::Result<void>
                    {
                        begun = true;
                        auto result =
                            seat_->key( static_cast<std::uint8_t>( keycode ), press );
                        if( !result )
                        {
                            return possibly_committed();
                        }
                        return {};
                    };

                    for( const auto& stroke : *keystrokes )
                    {
                        if( stroke.shift )
                        {
                            const auto result = send( shift, true );
                            if( !result )
                            {
                                return result;
                            }
                        }
                        if( stroke.altgr )
                        {
                            const auto result = send( altgr, true );
                            if( !result )
                            {
                                return result;
                            }
                        }

                        auto result = send( stroke.keycode, true );
                        if( !result )
                        {
                            return result;
                        }
                        result = send( stroke.keycode, false );
                        if( !result )
                        {
                            return result;
                        }

                        if( stroke.altgr )
                        {
                            result = send( altgr, false );
                            if( !result )
                            {
                                return result;
                            }
                        }
                        if( stroke.shift )
                        {
                            result = send( shift, false );
                            if( !result )
                            {
                                return result;
                            }
                        }
                    }

                    auto result = seat_->flush();
                    if( !result )
                    {
                        if( begun )
                        {
                            return possibly_committed();
                        }
                        return result;
                    }

                    return {};
                }

            private:

                X11InputSeat* seat_{};
                grab::Keymap* keymap_{};
                std::string   text_;
        };

        class PointerDragReservation final : public ReservationBase
        {
            public:

                PointerDragReservation( X11InputSeat&                   seat,
                                        grab::geometry::Point           from,
                                        grab::geometry::Point           to,
                                        const grab::input::DragOptions& options ) :
                    seat_( &seat ),
                    from_( from ),
                    to_( to ),
                    options_( options )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                commit( const grab::OperationContext& ) final
                {
                    return execute_drag( *seat_, from_, to_, options_ );
                }

            private:

                X11InputSeat*            seat_{};
                grab::geometry::Point    from_{};
                grab::geometry::Point    to_{};
                grab::input::DragOptions options_{};
        };

        class KeyReservation final : public ReservationBase
        {
            public:

                KeyReservation( X11InputSeat& seat,
                                grab::Keymap& keymap,
                                std::string   key_name ) :
                    seat_( &seat ),
                    keymap_( &keymap ),
                    key_name_( std::move( key_name ) )
                {
                }

                [[nodiscard]]
                grab::Result<void>
                commit( const grab::OperationContext& ) final
                {
                    const auto keystroke = keymap_->keystroke_for_key( key_name_ );
                    if( !keystroke.has_value() )
                    {
                        return failure<void>( grab::ErrorCode::UnsupportedCharacter,
                                              "named key is not available in keymap: " +
                                                  key_name_ );
                    }

                    constexpr auto max_keycode =
                        std::numeric_limits<std::uint8_t>::max();
                    if( keystroke->keycode == 0U || keystroke->keycode > max_keycode )
                    {
                        return failure<void>(
                            grab::ErrorCode::InvalidArgument,
                            "X11 keycode is outside the supported range"
                        );
                    }

                    std::uint32_t shift = 0U;
                    if( keystroke->shift )
                    {
                        shift = keymap_->shift_keycode();
                        if( shift == 0U || shift > max_keycode )
                        {
                            return failure<void>(
                                grab::ErrorCode::UnsupportedCharacter,
                                "keymap does not provide the required Shift modifier"
                            );
                        }
                    }
                    std::uint32_t altgr = 0U;
                    if( keystroke->altgr )
                    {
                        altgr = keymap_->altgr_keycode();
                        if( altgr == 0U || altgr > max_keycode )
                        {
                            return failure<void>(
                                grab::ErrorCode::UnsupportedCharacter,
                                "keymap does not provide the required AltGr modifier"
                            );
                        }
                    }

                    bool       begun = false;
                    const auto send  = [this, &begun]( std::uint32_t keycode,
                                                       bool press ) -> grab::Result<void>
                    {
                        begun = true;
                        auto result =
                            seat_->key( static_cast<std::uint8_t>( keycode ), press );
                        if( !result )
                        {
                            return possibly_committed();
                        }
                        return {};
                    };

                    if( keystroke->shift )
                    {
                        auto result = send( shift, true );
                        if( !result )
                        {
                            return result;
                        }
                    }
                    if( keystroke->altgr )
                    {
                        auto result = send( altgr, true );
                        if( !result )
                        {
                            return result;
                        }
                    }
                    auto result = send( keystroke->keycode, true );
                    if( !result )
                    {
                        return result;
                    }
                    result = send( keystroke->keycode, false );
                    if( !result )
                    {
                        return result;
                    }
                    if( keystroke->altgr )
                    {
                        result = send( altgr, false );
                        if( !result )
                        {
                            return result;
                        }
                    }
                    if( keystroke->shift )
                    {
                        result = send( shift, false );
                        if( !result )
                        {
                            return result;
                        }
                    }

                    result = seat_->flush();
                    if( !result )
                    {
                        if( begun )
                        {
                            return possibly_committed();
                        }
                        return result;
                    }
                    return {};
                }

            private:

                X11InputSeat* seat_{};
                grab::Keymap* keymap_{};
                std::string   key_name_;
        };

    }    // namespace

    X11InputSeat::X11InputSeat( grab::input::Seat seat,
                                InjectionLedger*  ledger ) noexcept :
        seat_( std::move( seat ) ),
        ledger_( ledger )
    {
    }

    grab::Result<void>
    X11InputSeat::move_pointer_absolute( std::int16_t x,
                                         std::int16_t y )
    {
        const std::scoped_lock lock( mutex_ );
        auto                   result = seat_.move_pointer_absolute( x, y );
        if( result && ledger_ != nullptr )
        {
            ledger_->record( InjectionKind::Motion, 0U );
        }
        return result;
    }

    grab::Result<void>
    X11InputSeat::button( std::uint8_t button,
                          bool         press )
    {
        const std::scoped_lock lock( mutex_ );
        auto                   result = seat_.button( button, press );
        if( result )
        {
            held_buttons_[button] = press;
            if( ledger_ != nullptr )
            {
                ledger_->record( press ? InjectionKind::ButtonPress
                                       : InjectionKind::ButtonRelease,
                                 button );
            }
        }
        return result;
    }

    grab::Result<void>
    X11InputSeat::key( std::uint8_t keycode,
                       bool         press )
    {
        const std::scoped_lock lock( mutex_ );
        auto                   result = seat_.key( keycode, press );
        if( result )
        {
            held_keys_[keycode] = press;
            if( ledger_ != nullptr )
            {
                ledger_->record( press ? InjectionKind::KeyPress
                                       : InjectionKind::KeyRelease,
                                 keycode );
            }
        }
        return result;
    }

    grab::Result<void>
    X11InputSeat::flush()
    {
        const std::scoped_lock lock( mutex_ );
        return seat_.flush();
    }

    SeatLane::Token
    X11InputSeat::acquire_lane()
    {
        return lane_.acquire();
    }

    bool
    X11InputSeat::held( std::uint8_t keycode ) const
    {
        const std::scoped_lock lock( mutex_ );
        return held_keys_[keycode];
    }

    bool
    X11InputSeat::set( std::uint8_t keycode,
                       bool         press )
    {
        return key( keycode, press ).has_value();
    }

    grab::Result<grab::NeutralizationOutcome>
    X11InputSeat::neutralize( const grab::OperationContext& )
    {
        const auto             lane = lane_.acquire();
        const std::scoped_lock lock( mutex_ );
        bool                   held   = false;
        bool                   failed = false;

        for( std::size_t index = 0U; index < held_buttons_.size(); ++index )
        {
            if( held_buttons_[index] )
            {
                held = true;
                const auto result =
                    seat_.button( static_cast<std::uint8_t>( index ), false );
                if( !result )
                {
                    failed = true;
                }
                else if( ledger_ != nullptr )
                {
                    ledger_->record( InjectionKind::ButtonRelease,
                                     static_cast<std::uint32_t>( index ) );
                }
            }
        }

        for( std::size_t index = 0U; index < held_keys_.size(); ++index )
        {
            if( held_keys_[index] )
            {
                held = true;
                const auto result =
                    seat_.key( static_cast<std::uint8_t>( index ), false );
                if( !result )
                {
                    failed = true;
                }
                else if( ledger_ != nullptr )
                {
                    ledger_->record( InjectionKind::KeyRelease,
                                     static_cast<std::uint32_t>( index ) );
                }
            }
        }

        if( !seat_.flush() )
        {
            failed = true;
        }

        held_buttons_.fill( false );
        held_keys_.fill( false );

        if( failed )
        {
            return grab::NeutralizationOutcome::Failed;
        }
        if( held )
        {
            return grab::NeutralizationOutcome::Released;
        }
        return grab::NeutralizationOutcome::NothingHeld;
    }

    X11PointerRoute::X11PointerRoute( X11TreeSource&    source,
                                      xcb_connection_t* connection,
                                      xcb_window_t      root,
                                      X11InputSeat&     seat ) noexcept :
        source_( &source ),
        connection_( connection ),
        root_( root ),
        seat_( &seat )
    {
    }

    grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
    X11PointerRoute::reserve( const grab::spi::ActionRequest& action,
                              const grab::OperationContext& )
    {
        if( action.verb == grab::spi::ActionVerb::Click )
        {
            const auto target = widget_ref_from( action.target );
            if( !target )
            {
                return failure<std::unique_ptr<grab::spi::RouteReservation>>(
                    grab::ErrorCode::InvalidArgument,
                    "X11 pointer route requires a WidgetRef target"
                );
            }

            return std::unique_ptr<grab::spi::RouteReservation>{
                std::make_unique<PointerReservation>( *source_,
                                                      connection_,
                                                      root_,
                                                      *seat_,
                                                      *target )
            };
        }
        if( action.verb == grab::spi::ActionVerb::Drag )
        {
            return std::unique_ptr<grab::spi::RouteReservation>{
                std::make_unique<PointerDragReservation>( *seat_,
                                                          action.drag_from,
                                                          action.drag_to,
                                                          action.drag_options )
            };
        }

        return failure<std::unique_ptr<grab::spi::RouteReservation>>(
            grab::ErrorCode::CapabilityUnavailable,
            "X11 pointer route only supports Click and Drag"
        );
    }

    X11KeyboardRoute::X11KeyboardRoute( X11TreeSource&    source,
                                        xcb_connection_t* connection,
                                        X11InputSeat&     seat,
                                        grab::Keymap      keymap ) noexcept :
        source_( &source ),
        connection_( connection ),
        seat_( &seat ),
        keymap_( std::move( keymap ) ),
        scratch_pool_( connection )
    {
    }

    grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
    X11KeyboardRoute::reserve( const grab::spi::ActionRequest& action,
                               const grab::OperationContext& )
    {
        static_cast<void>( source_ );
        static_cast<void>( connection_ );

        if( action.verb == grab::spi::ActionVerb::TypeText )
        {
            return std::unique_ptr<grab::spi::RouteReservation>{
                std::make_unique<KeyboardReservation>( *seat_, keymap_, action.text )
            };
        }
        if( action.verb == grab::spi::ActionVerb::PressKey )
        {
            return std::unique_ptr<grab::spi::RouteReservation>{
                std::make_unique<KeyReservation>( *seat_, keymap_, action.key_name )
            };
        }

        return failure<std::unique_ptr<grab::spi::RouteReservation>>(
            grab::ErrorCode::CapabilityUnavailable,
            "X11 keyboard route only supports TypeText and PressKey"
        );
    }

    X11ActivationRoute::X11ActivationRoute( X11TreeSource&          source,
                                            xcb_connection_t*       connection,
                                            xcb_window_t            root,
                                            grab::spi::EventSource& events ) noexcept :
        source_( &source ),
        connection_( connection ),
        root_( root ),
        events_( &events )
    {
    }

    grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
    X11ActivationRoute::reserve( const grab::spi::ActionRequest& action,
                                 const grab::OperationContext& )
    {
        if( action.verb == grab::spi::ActionVerb::Activate )
        {
            const auto target = widget_ref_from( action.target );
            if( !target )
            {
                return failure<std::unique_ptr<grab::spi::RouteReservation>>(
                    grab::ErrorCode::InvalidArgument,
                    "X11 activation route requires a WidgetRef target"
                );
            }

            return std::unique_ptr<grab::spi::RouteReservation>{
                std::make_unique<ActivationReservation>( *source_,
                                                         connection_,
                                                         root_,
                                                         *events_,
                                                         action.target,
                                                         *target )
            };
        }

        return failure<std::unique_ptr<grab::spi::RouteReservation>>(
            grab::ErrorCode::CapabilityUnavailable,
            "X11 activation route only supports Activate"
        );
    }

}    // namespace grab::drivers::desktop::x11
