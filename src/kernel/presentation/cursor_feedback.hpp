#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"
#include "kernel/input/gesture_classifier.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace grab
{

    class EventBus;

}

namespace grab::kernel::presentation
{

    class OverlayScene;

    inline constexpr std::chrono::milliseconds cursorFeedbackAdvanceInterval{ 16 };

    using CursorFeedbackAddShape =
        std::function<Result<overlay::ShapeId>( overlay::Shape )>;
    using CursorFeedbackRemoveShape = std::function<Result<void>( overlay::ShapeId )>;

    [[nodiscard]]
    Result<void>
    validate_cursor_feedback_config( const CursorFeedbackConfig& config );

    class CursorFeedbackPresenter final
    {
        public:

            CursorFeedbackPresenter( CursorFeedbackAddShape    add_shape,
                                     CursorFeedbackRemoveShape remove_shape,
                                     CursorFeedbackConfig      config );

            CursorFeedbackPresenter( OverlayScene&        scene,
                                     CursorFeedbackConfig config );

            ~CursorFeedbackPresenter();

            CursorFeedbackPresenter( const CursorFeedbackPresenter& ) = delete;
            CursorFeedbackPresenter&
            operator=( const CursorFeedbackPresenter& )          = delete;
            CursorFeedbackPresenter( CursorFeedbackPresenter&& ) = delete;
            CursorFeedbackPresenter&
            operator=( CursorFeedbackPresenter&& ) = delete;

            [[nodiscard]]
            Result<void>
            feed( const Event& event );

            [[nodiscard]]
            Result<void>
            advance( std::chrono::milliseconds now );

            [[nodiscard]]
            Result<void>
            consume( const input::GestureEvent& gesture );

            [[nodiscard]]
            Result<void>
            cancel();

            [[nodiscard]]
            Result<void>
            teardown();

            void
            abandon() noexcept;

        private:

            [[nodiscard]]
            Result<void>
            consume_all( const std::vector<input::GestureEvent>& gestures );

            [[nodiscard]]
            Result<void>
            add_ripple( const input::GestureEvent& gesture );

            [[nodiscard]]
            Result<void>
            add_progress( const input::GestureEvent& gesture );

            [[nodiscard]]
            Result<void>
            remove_progress();

            [[nodiscard]]
            Result<void>
            remove_owned( overlay::ShapeId id );

            [[nodiscard]]
            Result<void>
                                            remember_owned( overlay::ShapeId id );

            CursorFeedbackAddShape          add_shape_;
            CursorFeedbackRemoveShape       remove_shape_;
            CursorFeedbackConfig            config_;
            input::GestureClassifier        classifier_;
            std::vector<overlay::ShapeId>   owned_shapes_;
            std::optional<overlay::ShapeId> progress_;
    };

    struct CursorFeedbackObserverHooks
    {
            EventBus*                                            bus{};
            std::function<Result<void>( std::function<void()> )> post;
            std::function<Result<void>( std::chrono::nanoseconds,
                                        std::function<void()> )>
                                                       schedule;
            std::function<std::chrono::milliseconds()> clock;
            CursorFeedbackAddShape                     add_shape;
            CursorFeedbackRemoveShape                  remove_shape;
            std::function<bool()>                      on_reactor_thread;
            std::function<bool()>                      reactor_alive;
            std::function<Result<void>()>              release_observation;
    };

    class CursorFeedbackObserver final
        : public std::enable_shared_from_this<CursorFeedbackObserver>
    {
        public:

            [[nodiscard]]
            static Result<std::shared_ptr<CursorFeedbackObserver>>
            start( CursorFeedbackConfig        config,
                   CursorFeedbackObserverHooks hooks );

            ~CursorFeedbackObserver();

            CursorFeedbackObserver( const CursorFeedbackObserver& ) = delete;
            CursorFeedbackObserver&
            operator=( const CursorFeedbackObserver& )         = delete;
            CursorFeedbackObserver( CursorFeedbackObserver&& ) = delete;
            CursorFeedbackObserver&
            operator=( CursorFeedbackObserver&& ) = delete;

            [[nodiscard]]
            Result<void>
            stop();

            [[nodiscard]]
            Result<void>
            status() const;

        private:

            CursorFeedbackObserver( CursorFeedbackConfig        config,
                                    CursorFeedbackObserverHooks hooks );

            [[nodiscard]]
            Result<void>
            install();

            void
            rollback_start() noexcept;

            [[nodiscard]]
            Result<void>
            schedule_drain();

            [[nodiscard]]
            Result<void>
            schedule_timer();

            void
            notify() noexcept;

            void
            drain() noexcept;

            void
            timer_fired() noexcept;

            [[nodiscard]]
            std::optional<SubscriptionEvent>
            try_pop_item();

            void
            consume_item( const SubscriptionEvent& item );

            void
            clear_subscription() noexcept;

            [[nodiscard]]
            Result<void>
            cleanup_on_reactor();

            [[nodiscard]]
            Result<void>
            release_observation();

            void
            abandon_presenter() noexcept;

            void
                                        remember_error( Error error ) noexcept;

            CursorFeedbackObserverHooks hooks_;
            CursorFeedbackPresenter     presenter_;
            Subscription                subscription_;
            std::recursive_mutex        startup_mutex_;
            std::atomic_bool            active_{};
            std::atomic_bool            drain_scheduled_{};
            std::atomic_bool            owns_observation_{};
            mutable std::mutex          subscription_mutex_;
            mutable std::mutex          lifecycle_mutex_;
            mutable std::mutex          presenter_mutex_;
            mutable std::mutex          error_mutex_;
            std::optional<Error>        error_;
    };

}    // namespace grab::kernel::presentation
