#include "drivers/desktop/x11/x11_routes.hpp"
#include "drivers/desktop/x11/x11_tree_source.hpp"

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

namespace grab::drivers::desktop::x11
{
    namespace
    {

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
                verify( const grab::OperationContext& ) final
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

    }    // namespace

    X11InputSeat::X11InputSeat( grab::input::Seat seat ) noexcept :
        seat_( std::move( seat ) )
    {
    }

    grab::Result<void>
    X11InputSeat::move_pointer_absolute( std::int16_t x,
                                         std::int16_t y )
    {
        const std::scoped_lock lock( mutex_ );
        return seat_.move_pointer_absolute( x, y );
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
        }
        return result;
    }

    grab::Result<void>
    X11InputSeat::flush()
    {
        const std::scoped_lock lock( mutex_ );
        return seat_.flush();
    }

    grab::Result<grab::NeutralizationOutcome>
    X11InputSeat::neutralize( const grab::OperationContext& )
    {
        const std::scoped_lock lock( mutex_ );
        bool                   held   = false;
        bool                   failed = false;

        for( std::size_t index = 0U; index < held_buttons_.size(); ++index )
        {
            if( held_buttons_[index] )
            {
                held = true;
                if( !seat_.button( static_cast<std::uint8_t>( index ), false ) )
                {
                    failed = true;
                }
            }
        }

        for( std::size_t index = 0U; index < held_keys_.size(); ++index )
        {
            if( held_keys_[index] )
            {
                held = true;
                if( !seat_.key( static_cast<std::uint8_t>( index ), false ) )
                {
                    failed = true;
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
        if( action.verb != grab::spi::ActionVerb::Click )
        {
            return failure<std::unique_ptr<grab::spi::RouteReservation>>(
                grab::ErrorCode::CapabilityUnavailable,
                "X11 pointer route only supports Click"
            );
        }

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

    X11KeyboardRoute::X11KeyboardRoute( X11TreeSource&    source,
                                        xcb_connection_t* connection,
                                        X11InputSeat&     seat,
                                        grab::Keymap      keymap ) noexcept :
        source_( &source ),
        connection_( connection ),
        seat_( &seat ),
        keymap_( std::move( keymap ) )
    {
    }

    grab::Result<std::unique_ptr<grab::spi::RouteReservation>>
    X11KeyboardRoute::reserve( const grab::spi::ActionRequest& action,
                               const grab::OperationContext& )
    {
        static_cast<void>( source_ );
        static_cast<void>( connection_ );

        if( action.verb != grab::spi::ActionVerb::TypeText )
        {
            return failure<std::unique_ptr<grab::spi::RouteReservation>>(
                grab::ErrorCode::CapabilityUnavailable,
                "X11 keyboard route only supports TypeText"
            );
        }

        return std::unique_ptr<grab::spi::RouteReservation>{
            std::make_unique<KeyboardReservation>( *seat_, keymap_, action.text )
        };
    }

}    // namespace grab::drivers::desktop::x11
