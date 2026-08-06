#include "grab/event.hpp"
#include "grab/space.hpp"
#include "kernel/input/gesture_classifier.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace grab::kernel::input
{
    namespace
    {

        constexpr std::uint32_t noButton       = 0U;
        constexpr std::uint32_t firstButton    = 1U;
        constexpr std::uint32_t lastButton     = 3U;
        constexpr std::uint32_t buttonBitShift = 1U;

        [[nodiscard]]
        constexpr bool
        is_eligible_button( std::uint32_t button ) noexcept
        {
            return button >= firstButton && button <= lastButton;
        }

        [[nodiscard]]
        constexpr std::uint8_t
        button_bit( std::uint32_t button ) noexcept
        {
            return static_cast<std::uint8_t>( buttonBitShift
                                              << ( button - firstButton ) );
        }

        [[nodiscard]]
        std::chrono::milliseconds
        event_time( double timestamp )
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double>{ timestamp }
            );
        }

        [[nodiscard]]
        bool
        exceeds_slop( const SpacePoint& from,
                      const SpacePoint& to,
                      double            slop_px )
        {
            if( from.space != to.space )
            {
                return true;
            }
            return std::hypot( to.x - from.x, to.y - from.y ) > slop_px;
        }

        void
        emit( std::vector<GestureEvent>& output,
              Gesture                    kind,
              const SpacePoint&          at,
              std::uint32_t              button )
        {
            output.push_back( GestureEvent{
                .kind   = kind,
                .at     = at,
                .button = button,
            } );
        }

    }    // namespace

    GestureClassifier::GestureClassifier( GestureThresholds thresholds ) :
        thresholds_{ thresholds }
    {
    }

    std::vector<GestureEvent>
    GestureClassifier::feed( const Event& event )
    {
        const auto now    = event_time( event.timestamp );
        auto       output = process_deadline( now );

        switch( event.kind )
        {
            case EventKind::MouseMove :
                {
                    const auto* const motion = std::get_if<MouseMove>( &event.payload );
                    if( motion != nullptr && motion->position.has_value() )
                    {
                        handle_motion( *motion->position, now, output );
                    }
                    break;
                }
            case EventKind::MouseButtonDown :
                {
                    const auto* const button =
                        std::get_if<MouseButton>( &event.payload );
                    if( button != nullptr )
                    {
                        handle_button_down( button->button,
                                            button->position,
                                            now,
                                            output );
                    }
                    break;
                }
            case EventKind::MouseButtonUp :
                {
                    const auto* const button =
                        std::get_if<MouseButton>( &event.payload );
                    if( button != nullptr )
                    {
                        handle_button_up( button->button, button->position, output );
                    }
                    break;
                }
            default :
                break;
        }

        return output;
    }

    std::vector<GestureEvent>
    GestureClassifier::advance( std::chrono::milliseconds now )
    {
        return process_deadline( now );
    }

    void
    GestureClassifier::reset()
    {
        state_            = State::Idle;
        press_at_         = SpacePoint{};
        press_time_       = std::chrono::milliseconds{};
        last_motion_at_   = SpacePoint{};
        last_motion_time_ = std::chrono::milliseconds{};
        owner_button_     = noButton;
        buttons_down_     = 0U;
    }

    std::vector<GestureEvent>
    GestureClassifier::process_deadline( std::chrono::milliseconds now )
    {
        std::vector<GestureEvent> output;

        switch( state_ )
        {
            case State::Pressed :
                if( now - press_time_ >= thresholds_.hold )
                {
                    state_ = State::Holding;
                    emit( output, Gesture::Hold, press_at_, owner_button_ );
                }
                break;
            case State::Moving :
                if( now - last_motion_time_ >= thresholds_.pause )
                {
                    state_ = State::Idle;
                    emit( output, Gesture::Pause, last_motion_at_, noButton );
                }
                break;
            case State::AwaitDouble :
                // The double-click window includes its boundary, so a single
                // click is ready only once that window has expired.
                if( now - press_time_ > thresholds_.double_click )
                {
                    state_ = State::Idle;
                    emit( output, Gesture::Click, press_at_, owner_button_ );
                    if( !button_is_down( owner_button_ ) )
                    {
                        owner_button_ = noButton;
                    }
                }
                break;
            case State::Idle :
            case State::Holding :
            case State::PostDouble :
                break;
        }

        return output;
    }

    void
    GestureClassifier::handle_motion( const SpacePoint&          position,
                                      std::chrono::milliseconds  now,
                                      std::vector<GestureEvent>& output )
    {
        switch( state_ )
        {
            case State::Idle :
            case State::Moving :
                begin_moving( position, now );
                emit( output, Gesture::Move, position, noButton );
                break;
            case State::Pressed :
                if( exceeds_slop( press_at_, position, thresholds_.slop_px ) )
                {
                    begin_moving( position, now );
                    emit( output, Gesture::Move, position, noButton );
                }
                break;
            case State::Holding :
                if( exceeds_slop( press_at_, position, thresholds_.slop_px ) )
                {
                    state_ = State::Moving;
                    emit( output, Gesture::HoldCancel, position, owner_button_ );
                    begin_moving( position, now );
                    emit( output, Gesture::Move, position, noButton );
                }
                break;
            case State::AwaitDouble :
                if( exceeds_slop( press_at_, position, thresholds_.slop_px ) )
                {
                    emit( output, Gesture::Click, press_at_, owner_button_ );
                    owner_button_ = noButton;
                    begin_moving( position, now );
                    emit( output, Gesture::Move, position, noButton );
                }
                break;
            case State::PostDouble :
                if( !button_is_down( owner_button_ ) )
                {
                    owner_button_ = noButton;
                }
                begin_moving( position, now );
                emit( output, Gesture::Move, position, noButton );
                break;
        }
    }

    void
    GestureClassifier::handle_button_down( std::uint32_t                    button,
                                           const std::optional<SpacePoint>& position,
                                           std::chrono::milliseconds        now,
                                           std::vector<GestureEvent>&       output )
    {
        if( !is_eligible_button( button ) )
        {
            return;
        }

        const bool owner_is_down = button_is_down( owner_button_ );
        if( !mark_button_down( button ) )
        {
            return;
        }

        switch( state_ )
        {
            case State::Idle :
            case State::Moving :
                if( !owner_is_down )
                {
                    owner_button_ = button;
                    if( position.has_value() )
                    {
                        begin_press( button, *position, now );
                    }
                }
                break;
            case State::Pressed :
            case State::Holding :
                break;
            case State::AwaitDouble :
                if( button != owner_button_ || !position.has_value() )
                {
                    break;
                }
                if( now -
                    press_time_ <=
                    thresholds_.double_click &&
                    !exceeds_slop( press_at_, *position, thresholds_.slop_px ) )
                {
                    state_ = State::PostDouble;
                    emit( output, Gesture::DoubleClick, *position, button );
                    break;
                }

                emit( output, Gesture::Click, press_at_, owner_button_ );
                begin_press( button, *position, now );
                break;
            case State::PostDouble :
                if( !owner_is_down )
                {
                    owner_button_ = button;
                    if( position.has_value() )
                    {
                        begin_press( button, *position, now );
                    }
                }
                break;
        }
    }

    void
    GestureClassifier::handle_button_up( std::uint32_t                    button,
                                         const std::optional<SpacePoint>& position,
                                         std::vector<GestureEvent>&       output )
    {
        if( !is_eligible_button( button ) ||
            !mark_button_up( button ) ||
            button != owner_button_ )
        {
            return;
        }

        switch( state_ )
        {
            case State::Pressed :
                if( position.has_value() &&
                    exceeds_slop( press_at_, *position, thresholds_.slop_px ) )
                {
                    state_        = State::Idle;
                    owner_button_ = noButton;
                    break;
                }
                state_ = State::AwaitDouble;
                break;
            case State::Holding :
                state_ = State::Idle;
                emit( output,
                      Gesture::HoldEnd,
                      position.value_or( press_at_ ),
                      owner_button_ );
                owner_button_ = noButton;
                break;
            case State::Moving :
            case State::Idle :
                owner_button_ = noButton;
                break;
            case State::AwaitDouble :
            case State::PostDouble :
                break;
        }
    }

    void
    GestureClassifier::begin_press( std::uint32_t             button,
                                    const SpacePoint&         position,
                                    std::chrono::milliseconds now )
    {
        state_        = State::Pressed;
        press_at_     = position;
        press_time_   = now;
        owner_button_ = button;
    }

    void
    GestureClassifier::begin_moving( const SpacePoint&         position,
                                     std::chrono::milliseconds now )
    {
        state_            = State::Moving;
        last_motion_at_   = position;
        last_motion_time_ = now;
    }

    bool
    GestureClassifier::mark_button_down( std::uint32_t button ) noexcept
    {
        const auto bit      = button_bit( button );
        const bool was_down = ( buttons_down_ & bit ) != 0U;
        buttons_down_       = static_cast<std::uint8_t>( buttons_down_ | bit );
        return !was_down;
    }

    bool
    GestureClassifier::mark_button_up( std::uint32_t button ) noexcept
    {
        const auto bit      = button_bit( button );
        const bool was_down = ( buttons_down_ & bit ) != 0U;
        buttons_down_ = static_cast<std::uint8_t>( buttons_down_ &
                                                   static_cast<std::uint8_t>( ~bit ) );
        return was_down;
    }

    bool
    GestureClassifier::button_is_down( std::uint32_t button ) const noexcept
    {
        if( !is_eligible_button( button ) )
        {
            return false;
        }
        return ( buttons_down_ & button_bit( button ) ) != 0U;
    }

}    // namespace grab::kernel::input
