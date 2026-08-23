#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The session-owned comet trail: a fading tail that follows the cursor,
// exposed through Session::cursor_trail() the way the ripple is exposed
// through Session::cursor_feedback(). The trail geometry engine
// (TrailAnimator) and the scene it draws into (OverlayScene) are internal, so
// a public consumer cannot drive them directly; this controller wraps them and
// forwards the resulting Band::Trail shapes onto the public Overlay.
//
// It is modelled on the CLI's TrailBridge + TrailDrainState (overlay_command),
// with one deliberate difference: it does NOT start or stop observation. The
// CLI trail owns the whole session and manages observation itself; a session
// trail shares a session that is already observing (for its ripple, its reads,
// its clicks), so touching observation here would tear those down. It only adds
// a MouseMove subscription onto the running observation and drops it on stop.

#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace grab::kernel::presentation
{

    // Runs TrailAnimator against a private scene and forwards every shape it
    // produces onto the public Overlay via add_many. Lifted from the CLI's
    // TrailBridge; kept here so the session and the CLI can share the engine.
    class SessionTrailBridge final
    {
        public:

            SessionTrailBridge( Overlay&   overlay,
                                TrailStyle style ) :
                scene_{ []
                        {
                            return std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()
                            );
                        } },
                animator_{
                    scene_,
                    style
                },
                overlay_{ &overlay }
            {
                scene_.set_delta_sink(
                    [this]( const overlay::SceneDelta& delta )
                    {
                        forward( delta );
                    }
                );
            }

            void
            consume( const SubscriptionEvent& item )
            {
                animator_.consume( item );
            }

            // Push everything the animator has produced so far onto the overlay.
            // The delta sink defers Upserts into pending_shapes_ (so a burst of
            // motion is one add_many, not N adds); this is what actually lands
            // them. It MUST be called at the end of every drain cycle — without
            // it, new trail segments sit un-flushed until a fade delta happens to
            // trigger a flush, which is a visible lag between cursor and trail.
            void
            flush()
            {
                flush_pending();
            }

            [[nodiscard]]
            std::optional<Error>
            error() const
            {
                const std::scoped_lock lock{ error_mutex_ };
                return error_;
            }

        private:

            void
            forward( const overlay::SceneDelta& delta )
            {
                const auto* const upsert = std::get_if<overlay::Upsert>( &delta.change );
                if( upsert != nullptr )
                {
                    pending_shapes_.push_back( upsert->record.shape );
                    return;
                }
                flush_pending();
            }

            void
            flush_pending()
            {
                if( pending_shapes_.empty() )
                {
                    return;
                }
                auto added = overlay_->add_many( pending_shapes_ );
                pending_shapes_.clear();
                if( !added.has_value() )
                {
                    const std::scoped_lock lock{ error_mutex_ };
                    if( !error_.has_value() )
                    {
                        error_ = std::move( added.error() );
                    }
                }
            }

            OverlayScene                scene_;
            TrailAnimator               animator_;
            Overlay*                    overlay_{};
            std::vector<overlay::Shape> pending_shapes_;
            mutable std::mutex          error_mutex_;
            std::optional<Error>        error_;
    };

    // Drains a MouseMove subscription on the session reactor into the bridge.
    // Lifted from the CLI's TrailDrainState, minus observation ownership.
    class CursorTrailObserver final
        : public std::enable_shared_from_this<CursorTrailObserver>
    {
        public:

            CursorTrailObserver( Session&     session,
                                 Subscription subscription,
                                 Overlay&     overlay,
                                 TrailStyle   style ) :
                session_{ &session },
                subscription_{ std::move( subscription ) },
                bridge_{
                    overlay,
                    style
                }
            {
            }

            void
            install()
            {
                const std::weak_ptr<CursorTrailObserver> weak = weak_from_this();
                subscription_.set_notify(
                    [weak]
                    {
                        if( const auto state = weak.lock() )
                        {
                            state->schedule();
                        }
                    }
                );
            }

            void
            stop() noexcept
            {
                subscription_.set_notify( {} );
            }

            [[nodiscard]]
            std::optional<Error>
            error() const
            {
                {
                    const std::scoped_lock lock{ error_mutex_ };
                    if( error_.has_value() )
                    {
                        return error_;
                    }
                }
                return bridge_.error();
            }

        private:

            void
            schedule()
            {
                bool expected = false;
                if( !scheduled_.compare_exchange_strong( expected, true ) )
                {
                    return;
                }
                auto self   = shared_from_this();
                auto posted = session_->post(
                    [self]
                    {
                        self->drain();
                    }
                );
                if( !posted.has_value() )
                {
                    scheduled_.store( false );
                    remember_error( std::move( posted.error() ) );
                }
            }

            void
            drain()
            {
                // Mirrors the CLI's TrailDrainState::drain: pop the batch, then
                // flush the accumulated segments to the overlay THIS cycle (not
                // whenever the next fade delta lands) — that flush is what keeps
                // the trail glued to the cursor. The re-check after clearing
                // scheduled_ closes the lost-wakeup race where an event arrives
                // between the drain emptying the queue and releasing the slot.
                while( true )
                {
                    while( auto item = subscription_.try_pop_item() )
                    {
                        bridge_.consume( *item );
                    }
                    scheduled_.store( false );
                    auto raced = subscription_.try_pop_item();
                    if( !raced.has_value() )
                    {
                        bridge_.flush();
                        return;
                    }
                    bool expected = false;
                    if( scheduled_.compare_exchange_strong( expected, true ) )
                    {
                        bridge_.consume( *raced );
                        continue;
                    }
                    bridge_.consume( *raced );
                    bridge_.flush();
                    return;
                }
            }

            void
            remember_error( Error error ) noexcept
            {
                const std::scoped_lock lock{ error_mutex_ };
                if( !error_.has_value() )
                {
                    error_ = std::move( error );
                }
            }

            Session*             session_{};
            Subscription         subscription_;
            SessionTrailBridge   bridge_;
            std::atomic<bool>    scheduled_{ false };
            mutable std::mutex   error_mutex_;
            std::optional<Error> error_;
    };

    // Assemble the observer: subscribe to MouseMove on the (already running)
    // observation and install the reactor drain. The caller owns the returned
    // observer; dropping it (via stop) ends the trail.
    [[nodiscard]]
    inline Result<std::shared_ptr<CursorTrailObserver>>
    start_cursor_trail( Session&   session,
                        Overlay&   overlay,
                        TrailStyle style )
    {
        SubscriptionScope scope;
        scope.kinds       = { EventKind::MouseMove };
        auto subscription = session.watch( std::move( scope ) );
        if( !subscription.has_value() )
        {
            return std::unexpected( std::move( subscription.error() ) );
        }
        auto observer =
            std::make_shared<CursorTrailObserver>( session,
                                                   std::move( *subscription ),
                                                   overlay,
                                                   style );
        observer->install();
        return observer;
    }

}    // namespace grab::kernel::presentation
