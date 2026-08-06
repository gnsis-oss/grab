#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"
#include "kernel/events/event_bus.hpp"
#include "kernel/input/gesture_classifier.hpp"
#include "kernel/presentation/cursor_feedback.hpp"
#include "kernel/presentation/overlay_scene.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr double hiddenScale    = 0.0;
        constexpr double visibleScale   = 1.0;
        constexpr double hiddenOpacity  = 0.0;
        constexpr double visibleOpacity = 1.0;
        constexpr double hiddenReveal   = 0.0;
        constexpr double visibleReveal  = 1.0;
        constexpr double centerDivisor  = 2.0;

        [[nodiscard]]
        input::GestureThresholds
        classifier_thresholds( const GestureThresholds& thresholds ) noexcept
        {
            return input::GestureThresholds{
                .hold         = thresholds.hold,
                .double_click = thresholds.double_click,
                .pause        = thresholds.pause,
                .slop_px      = thresholds.slop_px,
            };
        }

        [[nodiscard]]
        bool
        finite_nonnegative( double value ) noexcept
        {
            return std::isfinite( value ) && value >= hiddenScale;
        }

        [[nodiscard]]
        bool
        nonnegative( std::chrono::milliseconds duration ) noexcept
        {
            return duration >= std::chrono::milliseconds::zero();
        }

        [[nodiscard]]
        Error
        exception_error( std::string_view      step,
                         const std::exception& exception )
        {
            return Error{
                .code       = ErrorCode::InternalFault,
                .message    = std::string{ step } + ": " + exception.what(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Error
        unknown_exception_error( std::string_view step )
        {
            return Error{
                .code       = ErrorCode::InternalFault,
                .message    = std::string{ step } + " failed",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        bool
        already_removed( const Error& error ) noexcept
        {
            return error.code == ErrorCode::StaleShape;
        }

        [[nodiscard]]
        Error
        queue_gap_error()
        {
            return Error{
                .code       = ErrorCode::QueueGap,
                .message    = "cursor feedback input queue lost events",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Result<void>
        first_failure( Result<void> first,
                       Result<void> second )
        {
            if( !first.has_value() )
            {
                return first;
            }
            return second;
        }

    }    // namespace

    Result<void>
    validate_cursor_feedback_config( const CursorFeedbackConfig& config )
    {
        if( !nonnegative( config.thresholds.hold ) ||
            !nonnegative( config.thresholds.double_click ) ||
            !nonnegative( config.thresholds.pause ) ||
            !finite_nonnegative( config.thresholds.slop_px ) )
        {
            return fail( ErrorCode::InvalidArgument,
                         "cursor feedback thresholds must be finite and nonnegative" );
        }

        if( config.click.has_value() &&
            ( !finite_nonnegative( config.click->radius_px ) ||
              !nonnegative( config.click->duration ) ) )
        {
            return fail( ErrorCode::InvalidArgument,
                         "cursor feedback ripple dimensions and duration must be "
                         "finite and nonnegative" );
        }

        if( config.hold.has_value() && ( !finite_nonnegative( config.hold->width_px ) ||
                                         !finite_nonnegative( config.hold->height_px ) ||
                                         !std::isfinite( config.hold->offset_y_px ) ) )
        {
            return fail( ErrorCode::InvalidArgument,
                         "cursor feedback progress dimensions and offset must be "
                         "finite" );
        }
        return {};
    }

    CursorFeedbackPresenter::CursorFeedbackPresenter(
        CursorFeedbackAddShape    add_shape,
        CursorFeedbackRemoveShape remove_shape,
        CursorFeedbackConfig      config
    ) :
        add_shape_{ std::move( add_shape ) },
        remove_shape_{ std::move( remove_shape ) },
        config_{ config },
        classifier_{ classifier_thresholds( config_.thresholds ) }
    {
    }

    CursorFeedbackPresenter::CursorFeedbackPresenter( OverlayScene&        scene,
                                                      CursorFeedbackConfig config ) :
        CursorFeedbackPresenter{
            [&scene]( overlay::Shape shape )
            {
                return scene.add( std::move( shape ) );
            },
            [&scene]( overlay::ShapeId id )
            {
                return scene.remove( id );
            },
            config
        }
    {
    }

    CursorFeedbackPresenter::~CursorFeedbackPresenter()
    {
        try
        {
            [[maybe_unused]]
            auto torn_down = teardown();
        }
        catch( ... )
        {
            return;
        }
    }

    Result<void>
    CursorFeedbackPresenter::feed( const Event& event )
    {
        return consume_all( classifier_.feed( event ) );
    }

    Result<void>
    CursorFeedbackPresenter::advance( std::chrono::milliseconds now )
    {
        return consume_all( classifier_.advance( now ) );
    }

    Result<void>
    CursorFeedbackPresenter::consume( const input::GestureEvent& gesture )
    {
        switch( gesture.kind )
        {
            case input::Gesture::Hold :
                if( config_.hold.has_value() )
                {
                    return add_progress( gesture );
                }
                return {};
            case input::Gesture::HoldEnd :
            case input::Gesture::HoldCancel :
                return remove_progress();
            case input::Gesture::Click :
            case input::Gesture::DoubleClick :
                {
                    auto removed = remove_progress();
                    if( !removed.has_value() )
                    {
                        return removed;
                    }
                    if( config_.click.has_value() )
                    {
                        return add_ripple( gesture );
                    }
                    return {};
                }
            case input::Gesture::Move :
            case input::Gesture::Pause :
                return {};
        }
        return {};
    }

    Result<void>
    CursorFeedbackPresenter::cancel()
    {
        classifier_.reset();
        return remove_progress();
    }

    Result<void>
    CursorFeedbackPresenter::teardown()
    {
        classifier_.reset();
        progress_.reset();

        std::optional<Error> first_error;
        for( const auto id : owned_shapes_ )
        {
            auto removed = remove_shape_( id );
            if( !removed.has_value() &&
                !already_removed( removed.error() ) &&
                !first_error.has_value() )
            {
                first_error = std::move( removed.error() );
            }
        }
        owned_shapes_.clear();
        if( first_error.has_value() )
        {
            return std::unexpected( std::move( *first_error ) );
        }
        return {};
    }

    void
    CursorFeedbackPresenter::abandon() noexcept
    {
        classifier_.reset();
        progress_.reset();
        owned_shapes_.clear();
    }

    Result<void>
    CursorFeedbackPresenter::consume_all(
        const std::vector<input::GestureEvent>& gestures
    )
    {
        for( const auto& gesture : gestures )
        {
            auto consumed = consume( gesture );
            if( !consumed.has_value() )
            {
                return consumed;
            }
        }
        return {};
    }

    Result<void>
    CursorFeedbackPresenter::add_ripple( const input::GestureEvent& gesture )
    {
        if( !config_.click.has_value() )
        {
            return {};
        }
        const auto&           style = config_.click.value();

        overlay::ScaleChannel scale;
        scale.easing   = overlay::Easing::OutCubic;
        scale.duration = style.duration;
        scale.from     = hiddenScale;
        scale.to       = visibleScale;

        overlay::OpacityChannel opacity;
        opacity.duration = style.duration;
        opacity.from     = visibleOpacity;
        opacity.to       = hiddenOpacity;

        auto added       = add_shape_( overlay::Shape{
            .geometry =
                overlay::Ellipse{
                                 .center   = gesture.at,
                                 .radius_x = style.radius_px,
                                 .radius_y = style.radius_px,
                                 },
            .stroke    = std::nullopt,
            .fill      = overlay::FillStyle{ .color = style.color },
            .lifetime  = overlay::Ttl{ .duration = style.duration },
            .band      = overlay::Band::Annotation,
            .animation = overlay::AnimationSpec{
                                 .scale   = scale,
                                 .opacity = opacity,
                                 },
        } );
        if( !added.has_value() )
        {
            return std::unexpected( std::move( added.error() ) );
        }

        auto remembered = remember_owned( *added );
        if( !remembered.has_value() )
        {
            [[maybe_unused]]
            auto removed_after_failure = remove_shape_( *added );
            return remembered;
        }
        return {};
    }

    Result<void>
    CursorFeedbackPresenter::add_progress( const input::GestureEvent& gesture )
    {
        if( !config_.hold.has_value() )
        {
            return {};
        }
        auto removed = remove_progress();
        if( !removed.has_value() )
        {
            return removed;
        }

        const auto&            style = config_.hold.value();
        overlay::RevealChannel reveal;
        reveal.duration  = config_.thresholds.hold;
        reveal.axis      = overlay::Axis::X;
        reveal.from_edge = overlay::Edge::Min;
        reveal.from      = hiddenReveal;
        reveal.to        = visibleReveal;

        auto added       = add_shape_( overlay::Shape{
            .geometry =
                overlay::Rect{
                              .bounds =
                        {
                            .x     = gesture.at.x - ( style.width_px / centerDivisor ),
                            .y     = gesture.at.y + style.offset_y_px,
                            .w     = style.width_px,
                            .h     = style.height_px,
                            .space = gesture.at.space,
                        }, },
            .stroke    = std::nullopt,
            .fill      = overlay::FillStyle{ .color = style.color },
            .lifetime  = overlay::Persistent{},
            .band      = overlay::Band::Annotation,
            .animation = overlay::AnimationSpec{
                              .reveal = reveal,
                              },
        } );
        if( !added.has_value() )
        {
            return std::unexpected( std::move( added.error() ) );
        }

        auto remembered = remember_owned( *added );
        if( !remembered.has_value() )
        {
            [[maybe_unused]]
            auto removed_after_failure = remove_shape_( *added );
            return remembered;
        }
        progress_ = *added;
        return {};
    }

    Result<void>
    CursorFeedbackPresenter::remove_progress()
    {
        if( !progress_.has_value() )
        {
            return {};
        }
        return remove_owned( *progress_ );
    }

    Result<void>
    CursorFeedbackPresenter::remove_owned( overlay::ShapeId id )
    {
        auto removed = remove_shape_( id );
        if( !removed.has_value() && !already_removed( removed.error() ) )
        {
            return std::unexpected( std::move( removed.error() ) );
        }

        std::erase( owned_shapes_, id );
        if( progress_ == id )
        {
            progress_.reset();
        }
        return {};
    }

    Result<void>
    CursorFeedbackPresenter::remember_owned( overlay::ShapeId id )
    {
        try
        {
            owned_shapes_.push_back( id );
            return {};
        }
        catch( const std::exception& exception )
        {
            return std::unexpected( exception_error( "track cursor feedback shape",
                                                     exception ) );
        }
        catch( ... )
        {
            return std::unexpected(
                unknown_exception_error( "track cursor feedback shape" )
            );
        }
    }

    CursorFeedbackObserver::CursorFeedbackObserver( CursorFeedbackConfig        config,
                                                    CursorFeedbackObserverHooks hooks ) :
        hooks_{ std::move( hooks ) },
        presenter_{
            hooks_.add_shape,
            hooks_.remove_shape,
            config
        }
    {
    }

    Result<std::shared_ptr<CursorFeedbackObserver>>
    CursorFeedbackObserver::start( CursorFeedbackConfig        config,
                                   CursorFeedbackObserverHooks hooks )
    {
        auto valid = validate_cursor_feedback_config( config );
        if( !valid.has_value() )
        {
            return std::unexpected( std::move( valid.error() ) );
        }
        if( hooks.bus ==
            nullptr ||
            !hooks.post ||
            !hooks.schedule ||
            !hooks.clock ||
            !hooks.add_shape ||
            !hooks.remove_shape ||
            !hooks.on_reactor_thread ||
            !hooks.reactor_alive )
        {
            return fail( ErrorCode::InvalidArgument,
                         "cursor feedback observer hooks are incomplete" );
        }

        std::shared_ptr<CursorFeedbackObserver> observer;
        try
        {
            observer = std::shared_ptr<CursorFeedbackObserver>(
                new CursorFeedbackObserver{ config, std::move( hooks ) }
            );
            auto installed = observer->install();
            if( !installed.has_value() )
            {
                auto error = std::move( installed.error() );
                observer->rollback_start();
                return std::unexpected( std::move( error ) );
            }
            observer->owns_observation_.store( true, std::memory_order_release );
            return observer;
        }
        catch( const std::exception& exception )
        {
            if( observer != nullptr )
            {
                observer->rollback_start();
            }
            return std::unexpected( exception_error( "start cursor feedback observer",
                                                     exception ) );
        }
        catch( ... )
        {
            if( observer != nullptr )
            {
                observer->rollback_start();
            }
            return std::unexpected(
                unknown_exception_error( "start cursor feedback observer" )
            );
        }
    }

    CursorFeedbackObserver::~CursorFeedbackObserver()
    {
        try
        {
            [[maybe_unused]]
            auto stopped = stop();
        }
        catch( ... )
        {
            return;
        }
    }

    Result<void>
    CursorFeedbackObserver::stop()
    {
        const std::scoped_lock lifecycle_lock{ lifecycle_mutex_ };
        if( !active_.exchange( false, std::memory_order_acq_rel ) )
        {
            return status();
        }

        clear_subscription();

        Result<void> cleaned;
        try
        {
            if( hooks_.on_reactor_thread() )
            {
                cleaned = cleanup_on_reactor();
            }
            else
            {
                auto completion = std::make_shared<std::promise<Result<void>>>();
                auto completed  = completion->get_future();
                const std::weak_ptr<CursorFeedbackObserver> weak   = weak_from_this();
                auto                                        posted = hooks_.post(
                    [weak, completion]
                    {
                        try
                        {
                            if( const auto observer = weak.lock() )
                            {
                                auto cleaned_on_reactor = observer->cleanup_on_reactor();
                                auto released = observer->release_observation();
                                completion->set_value(
                                    first_failure( std::move( cleaned_on_reactor ),
                                                   std::move( released ) )
                                );
                            }
                            else
                            {
                                completion->set_value( fail(
                                    ErrorCode::SessionClosed,
                                    "cursor feedback owner was released before teardown"
                                ) );
                            }
                        }
                        catch( const std::exception& exception )
                        {
                            try
                            {
                                completion->set_value( std::unexpected(
                                    exception_error( "tear down cursor feedback",
                                                     exception )
                                ) );
                            }
                            catch( ... )
                            {
                                return;
                            }
                        }
                        catch( ... )
                        {
                            try
                            {
                                completion->set_value(
                                    std::unexpected( unknown_exception_error(
                                        "tear down cursor feedback"
                                    ) )
                                );
                            }
                            catch( ... )
                            {
                                return;
                            }
                        }
                    }
                );
                if( !posted.has_value() )
                {
                    cleaned = std::unexpected( std::move( posted.error() ) );
                }
                else
                {
                    while( completed.wait_for( cursorFeedbackAdvanceInterval ) !=
                           std::future_status::ready )
                    {
                        if( !hooks_.reactor_alive() )
                        {
                            cleaned = fail(
                                ErrorCode::SessionClosed,
                                "cursor feedback reactor stopped before teardown"
                            );
                            break;
                        }
                    }
                    if( cleaned.has_value() )
                    {
                        cleaned = completed.get();
                    }
                }
            }
        }
        catch( const std::exception& exception )
        {
            cleaned =
                std::unexpected( exception_error( "dispatch cursor feedback teardown",
                                                  exception ) );
        }
        catch( ... )
        {
            cleaned = std::unexpected(
                unknown_exception_error( "dispatch cursor feedback teardown" )
            );
        }

        auto released = release_observation();
        cleaned       = first_failure( std::move( cleaned ), std::move( released ) );
        if( !cleaned.has_value() )
        {
            remember_error( std::move( cleaned.error() ) );
            abandon_presenter();
        }
        return status();
    }

    Result<void>
    CursorFeedbackObserver::status() const
    {
        const std::scoped_lock error_lock{ error_mutex_ };
        if( error_.has_value() )
        {
            return std::unexpected( *error_ );
        }
        return {};
    }

    Result<void>
    CursorFeedbackObserver::install()
    {
        const std::scoped_lock startup_lock{ startup_mutex_ };
        {
            const std::scoped_lock subscription_lock{ subscription_mutex_ };
            subscription_ = hooks_.bus->subscribe( SubscriptionScope{
                .kinds =
                    {
                            EventKind::MouseMove,
                            EventKind::MouseButtonDown,
                            EventKind::MouseButtonUp,
                            },
                .filter = {}
            } );

            const std::weak_ptr<CursorFeedbackObserver> weak = weak_from_this();
            subscription_.set_notify(
                [weak]
                {
                    if( const auto observer = weak.lock() )
                    {
                        observer->notify();
                    }
                }
            );
        }

        auto timer = schedule_timer();
        if( !timer.has_value() )
        {
            return timer;
        }
        auto drain = schedule_drain();
        if( !drain.has_value() )
        {
            return drain;
        }
        active_.store( true, std::memory_order_release );

        // A test hook (or another inline executor) may run the initial drain
        // before activation. Re-arm it so events queued during installation
        // cannot be stranded without a later notification.
        if( !drain_scheduled_.load( std::memory_order_acquire ) )
        {
            auto rescheduled = schedule_drain();
            if( !rescheduled.has_value() )
            {
                active_.store( false, std::memory_order_release );
                return rescheduled;
            }
        }
        return {};
    }

    void
    CursorFeedbackObserver::rollback_start() noexcept
    {
        const std::scoped_lock startup_lock{ startup_mutex_ };
        active_.store( false, std::memory_order_release );
        clear_subscription();
        try
        {
            const std::scoped_lock presenter_lock{ presenter_mutex_ };
            [[maybe_unused]]
            auto torn_down = presenter_.teardown();
        }
        catch( ... )
        {
            return;
        }
    }

    Result<void>
    CursorFeedbackObserver::schedule_drain()
    {
        bool expected = false;
        if( !drain_scheduled_.compare_exchange_strong( expected,
                                                       true,
                                                       std::memory_order_acq_rel ) )
        {
            return {};
        }

        try
        {
            const std::weak_ptr<CursorFeedbackObserver> weak   = weak_from_this();
            auto                                        posted = hooks_.post(
                [weak]
                {
                    if( const auto observer = weak.lock() )
                    {
                        observer->drain();
                    }
                }
            );
            if( !posted.has_value() )
            {
                drain_scheduled_.store( false, std::memory_order_release );
                return std::unexpected( std::move( posted.error() ) );
            }
            return {};
        }
        catch( const std::exception& exception )
        {
            drain_scheduled_.store( false, std::memory_order_release );
            return std::unexpected( exception_error( "schedule cursor feedback drain",
                                                     exception ) );
        }
        catch( ... )
        {
            drain_scheduled_.store( false, std::memory_order_release );
            return std::unexpected(
                unknown_exception_error( "schedule cursor feedback drain" )
            );
        }
    }

    Result<void>
    CursorFeedbackObserver::schedule_timer()
    {
        try
        {
            const std::weak_ptr<CursorFeedbackObserver> weak = weak_from_this();
            return hooks_.schedule( cursorFeedbackAdvanceInterval,
                                    [weak]
                                    {
                                        if( const auto observer = weak.lock() )
                                        {
                                            observer->timer_fired();
                                        }
                                    } );
        }
        catch( const std::exception& exception )
        {
            return std::unexpected( exception_error( "schedule cursor feedback timer",
                                                     exception ) );
        }
        catch( ... )
        {
            return std::unexpected(
                unknown_exception_error( "schedule cursor feedback timer" )
            );
        }
    }

    void
    CursorFeedbackObserver::notify() noexcept
    {
        if( !active_.load( std::memory_order_acquire ) )
        {
            return;
        }
        try
        {
            auto scheduled = schedule_drain();
            if( !scheduled.has_value() )
            {
                remember_error( std::move( scheduled.error() ) );
            }
        }
        catch( const std::exception& exception )
        {
            remember_error( exception_error( "notify cursor feedback", exception ) );
        }
        catch( ... )
        {
            remember_error( unknown_exception_error( "notify cursor feedback" ) );
        }
    }

    void
    CursorFeedbackObserver::drain() noexcept
    {
        {
            const std::scoped_lock startup_lock{ startup_mutex_ };
            if( !active_.load( std::memory_order_acquire ) )
            {
                drain_scheduled_.store( false, std::memory_order_release );
                return;
            }
        }
        try
        {
            while( active_.load( std::memory_order_acquire ) )
            {
                while( active_.load( std::memory_order_acquire ) )
                {
                    auto item = try_pop_item();
                    if( !item.has_value() )
                    {
                        break;
                    }

                    consume_item( *item );
                }

                drain_scheduled_.store( false, std::memory_order_release );
                if( !active_.load( std::memory_order_acquire ) )
                {
                    return;
                }

                auto raced = try_pop_item();
                if( !raced.has_value() )
                {
                    return;
                }

                bool       expected          = false;
                const bool continue_draining = drain_scheduled_.compare_exchange_strong(
                    expected,
                    true,
                    std::memory_order_acq_rel
                );
                consume_item( *raced );
                if( !continue_draining )
                {
                    return;
                }
            }
        }
        catch( const std::exception& exception )
        {
            drain_scheduled_.store( false, std::memory_order_release );
            remember_error( exception_error( "drain cursor feedback", exception ) );
        }
        catch( ... )
        {
            drain_scheduled_.store( false, std::memory_order_release );
            remember_error( unknown_exception_error( "drain cursor feedback" ) );
        }
    }

    void
    CursorFeedbackObserver::timer_fired() noexcept
    {
        {
            const std::scoped_lock startup_lock{ startup_mutex_ };
            if( !active_.load( std::memory_order_acquire ) )
            {
                return;
            }
        }

        try
        {
            Result<void> advanced;
            {
                const std::scoped_lock presenter_lock{ presenter_mutex_ };
                advanced = presenter_.advance( hooks_.clock() );
            }
            if( !advanced.has_value() )
            {
                remember_error( std::move( advanced.error() ) );
            }

            if( active_.load( std::memory_order_acquire ) )
            {
                auto scheduled = schedule_timer();
                if( !scheduled.has_value() )
                {
                    remember_error( std::move( scheduled.error() ) );
                }
            }
        }
        catch( const std::exception& exception )
        {
            remember_error( exception_error( "advance cursor feedback", exception ) );
        }
        catch( ... )
        {
            remember_error( unknown_exception_error( "advance cursor feedback" ) );
        }
    }

    std::optional<SubscriptionEvent>
    CursorFeedbackObserver::try_pop_item()
    {
        const std::scoped_lock subscription_lock{ subscription_mutex_ };
        return subscription_.try_pop_item();
    }

    void
    CursorFeedbackObserver::consume_item( const SubscriptionEvent& item )
    {
        Result<void> consumed;
        if( const auto* event = std::get_if<Event>( &item ) )
        {
            {
                const std::scoped_lock presenter_lock{ presenter_mutex_ };
                consumed = presenter_.feed( *event );
            }
            if( !consumed.has_value() )
            {
                remember_error( std::move( consumed.error() ) );
            }
            return;
        }

        {
            const std::scoped_lock presenter_lock{ presenter_mutex_ };
            consumed = presenter_.cancel();
        }
        if( !consumed.has_value() )
        {
            remember_error( std::move( consumed.error() ) );
        }
        remember_error( queue_gap_error() );
    }

    void
    CursorFeedbackObserver::clear_subscription() noexcept
    {
        try
        {
            const std::scoped_lock subscription_lock{ subscription_mutex_ };
            subscription_.set_notify( {} );
            subscription_ = Subscription{};
        }
        catch( ... )
        {
            return;
        }
    }

    Result<void>
    CursorFeedbackObserver::cleanup_on_reactor()
    {
        drain_scheduled_.store( false, std::memory_order_release );
        try
        {
            const std::scoped_lock presenter_lock{ presenter_mutex_ };
            return presenter_.teardown();
        }
        catch( const std::exception& exception )
        {
            return std::unexpected( exception_error( "tear down cursor feedback",
                                                     exception ) );
        }
        catch( ... )
        {
            return std::unexpected(
                unknown_exception_error( "tear down cursor feedback" )
            );
        }
    }

    Result<void>
    CursorFeedbackObserver::release_observation()
    {
        if( !owns_observation_.exchange( false, std::memory_order_acq_rel ) ||
            !hooks_.release_observation )
        {
            return {};
        }

        try
        {
            return hooks_.release_observation();
        }
        catch( const std::exception& exception )
        {
            return std::unexpected(
                exception_error( "release cursor feedback observation", exception )
            );
        }
        catch( ... )
        {
            return std::unexpected(
                unknown_exception_error( "release cursor feedback observation" )
            );
        }
    }

    void
    CursorFeedbackObserver::abandon_presenter() noexcept
    {
        try
        {
            const std::scoped_lock presenter_lock{ presenter_mutex_ };
            presenter_.abandon();
        }
        catch( ... )
        {
            return;
        }
    }

    void
    CursorFeedbackObserver::remember_error( Error error ) noexcept
    {
        try
        {
            const std::scoped_lock error_lock{ error_mutex_ };
            if( !error_.has_value() )
            {
                error_ = std::move( error );
            }
        }
        catch( ... )
        {
            return;
        }
    }

}    // namespace grab::kernel::presentation
