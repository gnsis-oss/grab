#include "frontends/cli/common.hpp"
#include "frontends/cli/sketch_command.hpp"
#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_draw.hpp"
#include "kernel/scheduling/reactor.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
// NOLINTNEXTLINE(modernize-deprecated-headers,misc-include-cleaner): POSIX signals.
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
// NOLINTNEXTLINE(misc-include-cleaner): EPOLL* constants are provided here.
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace grab::cli
{
    namespace
    {

        constexpr std::string_view strokePxFlag      = "--stroke-px";
        constexpr std::string_view filledFlag        = "--filled";
        constexpr std::string_view colorFlag         = "--color";
        constexpr std::string_view editKey           = "e";
        constexpr std::string_view rectangleKey      = "r";
        constexpr std::string_view ellipseKey        = "o";
        constexpr std::string_view pathKey           = "p";
        constexpr std::string_view escapeKey         = "Escape";
        constexpr std::string_view backspaceKey      = "BackSpace";
        constexpr std::size_t      colorTextLength   = 6U;
        constexpr std::uint8_t     hexadecimalBase   = 16U;
        constexpr std::uint32_t    redChannelShift   = 16U;
        constexpr std::uint32_t    greenChannelShift = 8U;
        constexpr std::uint32_t    channelMask       = 0XFFU;
        constexpr std::uint8_t  opaqueChannel = std::numeric_limits<std::uint8_t>::max();
        constexpr std::size_t   optionValueStep       = 2U;
        constexpr std::size_t   maximumEventsPerDrain = 64U;
        constexpr std::uint32_t signalEvents =
            static_cast<std::uint32_t>( EPOLLIN | EPOLLERR | EPOLLHUP );
        constexpr int posixSuccess = 0;
        constexpr int invalidFile  = -1;

        [[nodiscard]]
        Error
        missing_backend_operation( std::string_view operation )
        {
            return Error{
                .code       = ErrorCode::InternalFault,
                .message    = "sketch backend has no " + std::string{ operation },
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Error
        system_error( std::string_view operation,
                      int              error_number )
        {
            return Error{
                .code = ErrorCode::InternalFault,
                .message =
                    std::string{ operation }
                    +
                    ": " +
                    std::error_code{ error_number, std::generic_category() }
                    .message(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Result<overlay::Color>
        parse_color( std::string_view text )
        {
            std::uint32_t value{};
            const auto    parsed =
                std::from_chars( text.begin(), text.end(), value, hexadecimalBase );
            if( text.size() !=
                colorTextLength ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr != text.end() )
            {
                return fail( ErrorCode::InvalidArgument, "--color must match RRGGBB" );
            }
            return overlay::Color{
                .r = static_cast<std::uint8_t>( value >> redChannelShift & channelMask ),
                .g = static_cast<std::uint8_t>( value >>
                                                greenChannelShift &
                                                channelMask ),
                .b = static_cast<std::uint8_t>( value & channelMask ),
                .a = opaqueChannel,
            };
        }

        [[nodiscard]]
        Result<float>
        parse_stroke_width( std::string_view text )
        {
            double            value{};
            const auto* const begin  = text.begin();
            const auto* const end    = text.end();
            const auto        parsed = std::from_chars( begin, end, value );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                !std::isfinite( value ) ||
                value <=
                0.0 ||
                value > static_cast<double>( std::numeric_limits<float>::max() ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "--stroke-px must be a positive finite number" );
            }
            return static_cast<float>( value );
        }

        [[nodiscard]]
        Result<std::vector<std::string_view>>
        argument_views( std::span<char* const> args )
        {
            std::vector<std::string_view> result;
            result.reserve( args.size() );
            for( const char* const argument : args )
            {
                if( argument == nullptr )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "sketch command argument is missing" );
                }
                result.emplace_back( argument );
            }
            return result;
        }

        struct CompletionState
        {
                std::mutex              mutex;
                std::condition_variable changed;
                bool                    complete{};
        };

        void
        complete( const std::shared_ptr<CompletionState>& state ) noexcept
        {
            {
                const std::scoped_lock lock{ state->mutex };
                state->complete = true;
            }
            state->changed.notify_all();
        }

        void
        wait_for_completion( CompletionState& state )
        {
            std::unique_lock lock{ state.mutex };
            state.changed.wait( lock,
                                [&state]
                                {
                                    return state.complete;
                                } );
        }

        class BlockedSignals final
        {
            public:

                [[nodiscard]]
                static Result<BlockedSignals>
                create()
                {
                    sigset_t signals{};    // NOLINT(misc-include-cleaner)
                    // NOLINTNEXTLINE(misc-include-cleaner): POSIX signal.h.
                    if( ::sigemptyset( &signals ) !=
                        posixSuccess ||
                        // NOLINTNEXTLINE(misc-include-cleaner): POSIX signal.h.
                        ::sigaddset( &signals, SIGINT ) !=
                        posixSuccess ||
                        // NOLINTNEXTLINE(misc-include-cleaner): POSIX signal.h.
                        ::sigaddset( &signals, SIGTERM ) != posixSuccess )
                    {
                        return std::unexpected( system_error( "configure sketch signals",
                                                              errno ) );
                    }

                    sigset_t  old_mask{};    // NOLINT(misc-include-cleaner)
                    const int blocked =
                        ::pthread_sigmask( SIG_BLOCK, &signals, &old_mask );
                    if( blocked != posixSuccess )
                    {
                        return std::unexpected( system_error( "block sketch signals",
                                                              blocked ) );
                    }

                    const int descriptor =
                        ::signalfd( invalidFile, &signals, SFD_CLOEXEC | SFD_NONBLOCK );
                    if( descriptor == invalidFile )
                    {
                        const int open_error = errno;
                        static_cast<void>(
                            ::pthread_sigmask( SIG_SETMASK, &old_mask, nullptr )
                        );
                        return std::unexpected(
                            system_error( "open sketch signal descriptor", open_error )
                        );
                    }
                    return BlockedSignals{ descriptor, old_mask };
                }

                ~BlockedSignals()
                {
                    reset();
                }

                BlockedSignals( const BlockedSignals& ) = delete;
                BlockedSignals&
                operator=( const BlockedSignals& ) = delete;

                BlockedSignals( BlockedSignals&& other ) noexcept :
                    descriptor_{ std::exchange( other.descriptor_,
                                                invalidFile ) },
                    old_mask_{ other.old_mask_ },
                    restore_mask_{ std::exchange( other.restore_mask_,
                                                  false ) }
                {
                }

                BlockedSignals&
                operator=( BlockedSignals&& other ) noexcept
                {
                    if( this != &other )
                    {
                        reset();
                        descriptor_   = std::exchange( other.descriptor_, invalidFile );
                        old_mask_     = other.old_mask_;
                        restore_mask_ = std::exchange( other.restore_mask_, false );
                    }
                    return *this;
                }

                [[nodiscard]]
                int
                descriptor() const noexcept
                {
                    return descriptor_;
                }

            private:

                BlockedSignals(
                    int      descriptor,
                    sigset_t old_mask    // NOLINT(misc-include-cleaner)
                ) noexcept :
                    descriptor_{ descriptor },
                    old_mask_{ old_mask },
                    restore_mask_{ true }
                {
                }

                void
                reset() noexcept
                {
                    if( descriptor_ != invalidFile )
                    {
                        static_cast<void>( ::close( descriptor_ ) );
                        descriptor_ = invalidFile;
                    }
                    if( restore_mask_ )
                    {
                        static_cast<void>(
                            ::pthread_sigmask( SIG_SETMASK, &old_mask_, nullptr )
                        );
                        restore_mask_ = false;
                    }
                }

                int      descriptor_{ invalidFile };
                sigset_t old_mask_{};    // NOLINT(misc-include-cleaner)
                bool     restore_mask_{};
        };

        [[nodiscard]]
        Result<void>
        reactor_barrier( Session& session )
        {
            auto completion = std::make_shared<std::promise<void>>();
            auto completed  = completion->get_future();
            auto posted     = session.post(
                [completion]
                {
                    completion->set_value();
                }
            );
            if( !posted.has_value() )
            {
                return std::unexpected( std::move( posted.error() ) );
            }
            completed.get();
            return {};
        }

        [[nodiscard]]
        Result<void>
        wait_until_complete( Session&                                session,
                             const BlockedSignals&                   signals,
                             const std::shared_ptr<CompletionState>& state )
        {
            std::uint64_t token{};
            try
            {
                token = session.reactor().add_fd(
                    signals.descriptor(),
                    signalEvents,
                    [state, descriptor = signals.descriptor()]( std::uint32_t events )
                    {
                        signalfd_siginfo information{};
                        const auto       bytes =
                            ::read( descriptor, &information, sizeof( information ) );
                        if( bytes >
                            0 ||
                            ( events &
                              static_cast<std::uint32_t>( EPOLLERR | EPOLLHUP ) ) != 0U )
                        {
                            complete( state );
                        }
                    }
                );
            }
            catch( const std::exception& error )
            {
                return fail( ErrorCode::InternalFault,
                             std::string{ "watch sketch signals: " } + error.what() );
            }
            catch( ... )
            {
                return fail( ErrorCode::InternalFault, "watch sketch signals failed" );
            }

            wait_for_completion( *state );
            session.reactor().remove_fd( token );
            return reactor_barrier( session );
        }

        class RuntimeBackendState final
        {
            public:

                explicit RuntimeBackendState( Overlay& overlay ) noexcept :
                    overlay_{ &overlay }
                {
                }

                [[nodiscard]]
                Result<overlay::ShapeId>
                add( overlay::Shape shape )
                {
                    return overlay_->add( std::move( shape ) );
                }

                [[nodiscard]]
                Result<void>
                update( overlay::ShapeId id,
                        overlay::Shape   shape )
                {
                    return overlay_->update( id, std::move( shape ) );
                }

                [[nodiscard]]
                Result<void>
                remove( overlay::ShapeId id )
                {
                    return overlay_->remove( id );
                }

                [[nodiscard]]
                Result<void>
                begin_edit( std::span<const overlay::ShapeId> editable )
                {
                    auto started = overlay_edit( *overlay_, editable, EditCallbacks{} );
                    if( !started.has_value() )
                    {
                        return std::unexpected( std::move( started.error() ) );
                    }
                    edit_.emplace( std::move( *started ) );
                    return {};
                }

                [[nodiscard]]
                Result<void>
                end_edit()
                {
                    Result<void> status;
                    if( edit_.has_value() )
                    {
                        status = edit_->status();
                    }
                    // EditSession teardown synchronously restores an empty input
                    // region. Reset even when status already reports a failure.
                    edit_.reset();
                    return status;
                }

            private:

                Overlay*                   overlay_{};
                std::optional<EditSession> edit_;
        };

        [[nodiscard]]
        SketchBackend
        make_backend( const std::shared_ptr<RuntimeBackendState>& state )
        {
            return SketchBackend{
                .add_shape =
                    [state]( overlay::Shape shape )
                {
                    return state->add( std::move( shape ) );
                },
                .update_shape =
                    [state]( overlay::ShapeId id, overlay::Shape shape )
                {
                    return state->update( id, std::move( shape ) );
                },
                .remove_shape =
                    [state]( overlay::ShapeId id )
                {
                    return state->remove( id );
                },
                .begin_edit =
                    [state]( std::span<const overlay::ShapeId> editable )
                {
                    return state->begin_edit( editable );
                },
                .end_edit =
                    [state]
                {
                    return state->end_edit();
                },
            };
        }

        class SketchDrainState final
            : public std::enable_shared_from_this<SketchDrainState>
        {
            public:

                SketchDrainState( Session&                         session,
                                  Subscription                     subscription,
                                  SketchController                 controller,
                                  std::shared_ptr<CompletionState> completion ) :
                    session_{ &session },
                    subscription_{ std::move( subscription ) },
                    controller_{ std::move( controller ) },
                    completion_{ std::move( completion ) }
                {
                }

                void
                install()
                {
                    const std::weak_ptr<SketchDrainState> weak = weak_from_this();
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

                [[nodiscard]]
                Result<void>
                stop()
                {
                    terminal_.store( true );
                    subscription_.set_notify( {} );
                    session_->stop_observation();
                    auto barrier  = reactor_barrier( *session_ );
                    auto finished = controller_.finish();
                    if( !barrier.has_value() )
                    {
                        return std::unexpected( std::move( barrier.error() ) );
                    }
                    return finished;
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
                schedule()
                {
                    if( terminal_.load() )
                    {
                        return;
                    }
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

                [[nodiscard]]
                bool
                consume( const SubscriptionEvent& item )
                {
                    if( std::holds_alternative<QueueGapMarker>( item ) )
                    {
                        remember_error( Error{
                            .code       = ErrorCode::QueueGap,
                            .message    = "sketch input subscription lost events",
                            .capability = {},
                            .target     = {},
                            .attempts   = {},
                        } );
                        return false;
                    }
                    auto consumed = controller_.consume( std::get<Event>( item ) );
                    if( !consumed.has_value() )
                    {
                        remember_error( std::move( consumed.error() ) );
                        return false;
                    }
                    return true;
                }

                [[nodiscard]]
                bool
                flush_preview()
                {
                    auto flushed = controller_.flush_preview();
                    if( !flushed.has_value() )
                    {
                        remember_error( std::move( flushed.error() ) );
                        return false;
                    }
                    return true;
                }

                void
                drain()
                {
                    if( terminal_.load() )
                    {
                        scheduled_.store( false );
                        return;
                    }

                    std::size_t consumed{};
                    while( consumed < maximumEventsPerDrain )
                    {
                        auto item = subscription_.try_pop_item();
                        if( !item.has_value() )
                        {
                            break;
                        }
                        if( !consume( *item ) )
                        {
                            scheduled_.store( false );
                            return;
                        }
                        ++consumed;
                    }

                    if( !flush_preview() )
                    {
                        scheduled_.store( false );
                        return;
                    }

                    scheduled_.store( false );
                    if( terminal_.load() )
                    {
                        return;
                    }

                    // Close the scheduled window before probing for a race. A
                    // concurrent enqueue can arrange the continuation; an item
                    // already queued notified while scheduled=true, so arrange
                    // its continuation here after consuming it.
                    auto raced = subscription_.try_pop_item();
                    if( !raced.has_value() )
                    {
                        return;
                    }
                    if( !consume( *raced ) )
                    {
                        return;
                    }
                    schedule();
                }

                void
                remember_error( Error error )
                {
                    {
                        const std::scoped_lock lock{ error_mutex_ };
                        if( !error_.has_value() )
                        {
                            error_ = std::move( error );
                        }
                    }
                    terminal_.store( true );
                    complete( completion_ );
                }

                Session*                         session_{};
                Subscription                     subscription_;
                SketchController                 controller_;
                std::shared_ptr<CompletionState> completion_;
                std::atomic_bool                 scheduled_;
                std::atomic_bool                 terminal_;
                mutable std::mutex               error_mutex_;
                std::optional<Error>             error_;
        };

        [[nodiscard]]
        Result<void>
        execute_sketch( const SketchOptions& options )
        {
            auto signals = BlockedSignals::create();
            if( !signals.has_value() )
            {
                return std::unexpected( std::move( signals.error() ) );
            }

            auto session = Session::open();
            if( !session.has_value() )
            {
                return std::unexpected( std::move( session.error() ) );
            }
            auto overlay = ( *session )->overlay();
            if( !overlay.has_value() )
            {
                return std::unexpected( std::move( overlay.error() ) );
            }

            SubscriptionScope scope;
            scope.kinds = {
                EventKind::KeyDown,
                EventKind::MouseMove,
                EventKind::MouseButtonDown,
                EventKind::MouseButtonUp,
            };
            auto subscription = ( *session )->watch( std::move( scope ) );
            if( !subscription.has_value() )
            {
                return std::unexpected( std::move( subscription.error() ) );
            }

            auto completion    = std::make_shared<CompletionState>();
            auto backend_state = std::make_shared<RuntimeBackendState>( **overlay );
            auto drain         = std::make_shared<SketchDrainState>(
                **session,
                std::move( *subscription ),
                SketchController{ options, make_backend( backend_state ) },
                completion
            );
            drain->install();
            auto observation = ( *session )->start_observation();
            if( !observation.has_value() )
            {
                [[maybe_unused]]
                auto stopped = drain->stop();
                return std::unexpected( std::move( observation.error() ) );
            }

            auto waited  = wait_until_complete( **session, *signals, completion );
            auto stopped = drain->stop();
            if( !waited.has_value() )
            {
                return std::unexpected( std::move( waited.error() ) );
            }
            if( !stopped.has_value() )
            {
                return std::unexpected( std::move( stopped.error() ) );
            }
            if( auto error = drain->error(); error.has_value() )
            {
                return std::unexpected( std::move( *error ) );
            }
            return {};
        }

        int
        report_parse_error( const Error& error )
        {
            print_error( error.message );
            return usageExitCode;
        }

        int
        report_runtime_result( Result<void> result )
        {
            if( result.has_value() )
            {
                return successExitCode;
            }
            print_error( result.error().message );
            return runtimeExitCode;
        }

    }    // namespace

    SketchController::SketchController( SketchOptions options,
                                        SketchBackend backend ) :
        options_{ options },
        backend_{ std::move( backend ) }
    {
    }

    Result<void>
    SketchController::consume( const Event& event )
    {
        switch( event.kind )
        {
            case EventKind::KeyDown :
                if( const auto* key = std::get_if<InputKey>( &event.payload ) )
                {
                    return consume_key( *key );
                }
                break;
            case EventKind::MouseButtonDown :
                if( const auto* button = std::get_if<MouseButton>( &event.payload ) )
                {
                    return consume_button_down( *button );
                }
                break;
            case EventKind::MouseMove :
                if( const auto* motion = std::get_if<MouseMove>( &event.payload ) )
                {
                    return consume_motion( *motion );
                }
                break;
            case EventKind::MouseButtonUp :
                if( const auto* button = std::get_if<MouseButton>( &event.payload ) )
                {
                    return consume_button_up( *button );
                }
                break;
            default :
                break;
        }
        return {};
    }

    Result<void>
    SketchController::flush_preview()
    {
        if( !preview_dirty_ )
        {
            return {};
        }
        preview_dirty_ = false;
        if( !pending_preview_.has_value() )
        {
            return clear_preview();
        }

        auto shape = std::move( *pending_preview_ );
        pending_preview_.reset();
        if( preview_id_.has_value() )
        {
            if( !backend_.update_shape )
            {
                return std::unexpected(
                    missing_backend_operation( "update operation" )
                );
            }
            return backend_.update_shape( *preview_id_, std::move( shape ) );
        }
        if( !backend_.add_shape )
        {
            return std::unexpected( missing_backend_operation( "add operation" ) );
        }
        auto added = backend_.add_shape( std::move( shape ) );
        if( !added.has_value() )
        {
            return std::unexpected( std::move( added.error() ) );
        }
        preview_id_ = *added;
        return {};
    }

    Result<void>
    SketchController::finish()
    {
        std::optional<Error> first_error;
        auto                 cancelled = cancel_draw();
        if( !cancelled.has_value() )
        {
            first_error = std::move( cancelled.error() );
        }
        if( editing_ )
        {
            editing_ = false;
            if( !backend_.end_edit )
            {
                if( !first_error.has_value() )
                {
                    first_error = missing_backend_operation( "end-edit operation" );
                }
            }
            else
            {
                auto ended = backend_.end_edit();
                if( !ended.has_value() && !first_error.has_value() )
                {
                    first_error = std::move( ended.error() );
                }
            }
        }
        if( first_error.has_value() )
        {
            return std::unexpected( std::move( *first_error ) );
        }
        return {};
    }

    overlay::DrawKind
    SketchController::kind() const noexcept
    {
        return options_.kind;
    }

    bool
    SketchController::editing() const noexcept
    {
        return editing_;
    }

    bool
    SketchController::drawing() const noexcept
    {
        return interaction_.active();
    }

    bool
    SketchController::preview_visible() const noexcept
    {
        return preview_id_.has_value();
    }

    std::span<const overlay::ShapeId>
    SketchController::editable_shapes() const noexcept
    {
        return editable_;
    }

    Result<void>
    SketchController::consume_key( const InputKey& key )
    {
        if( key.name == editKey )
        {
            return toggle_edit();
        }
        if( key.name == rectangleKey )
        {
            return select_kind( overlay::DrawKind::Rectangle );
        }
        if( key.name == ellipseKey )
        {
            return select_kind( overlay::DrawKind::Ellipse );
        }
        if( key.name == pathKey )
        {
            return select_kind( overlay::DrawKind::Path );
        }
        if( key.name == escapeKey && !editing_ )
        {
            return cancel_draw();
        }
        if( key.name == backspaceKey )
        {
            return remove_last_shape();
        }
        return {};
    }

    Result<void>
    SketchController::consume_button_down( const MouseButton& button )
    {
        if( editing_ || !button.position.has_value() )
        {
            return {};
        }
        auto cancelled = cancel_draw();
        if( !cancelled.has_value() )
        {
            return std::unexpected( std::move( cancelled.error() ) );
        }
        interaction_.begin( options_.kind, *button.position, options_.style );
        pressed_button_ = button.button;
        return {};
    }

    Result<void>
    SketchController::consume_motion( const MouseMove& motion )
    {
        if( editing_ || !interaction_.active() || !motion.position.has_value() )
        {
            return {};
        }
        auto preview = interaction_.update( *motion.position );
        if( preview.has_value() )
        {
            pending_preview_ = std::move( *preview );
            preview_dirty_   = true;
        }
        else if( interaction_.kind() != overlay::DrawKind::Path )
        {
            pending_preview_.reset();
            preview_dirty_ = true;
        }
        return {};
    }

    Result<void>
    SketchController::consume_button_up( const MouseButton& button )
    {
        if( editing_ || !interaction_.active() )
        {
            return {};
        }
        if( !pressed_button_.has_value() || button.button != *pressed_button_ )
        {
            return {};
        }
        if( !button.position.has_value() )
        {
            return cancel_draw();
        }
        return commit( *button.position );
    }

    Result<void>
    SketchController::select_kind( overlay::DrawKind kind )
    {
        if( editing_ )
        {
            editing_ = false;
            if( !backend_.end_edit )
            {
                return std::unexpected(
                    missing_backend_operation( "end-edit operation" )
                );
            }
            auto ended = backend_.end_edit();
            if( !ended.has_value() )
            {
                return std::unexpected( std::move( ended.error() ) );
            }
        }
        auto cancelled = cancel_draw();
        if( !cancelled.has_value() )
        {
            return std::unexpected( std::move( cancelled.error() ) );
        }
        options_.kind = kind;
        return {};
    }

    Result<void>
    SketchController::toggle_edit()
    {
        if( editing_ )
        {
            editing_ = false;
            if( !backend_.end_edit )
            {
                return std::unexpected(
                    missing_backend_operation( "end-edit operation" )
                );
            }
            return backend_.end_edit();
        }

        auto cancelled = cancel_draw();
        if( !cancelled.has_value() )
        {
            return std::unexpected( std::move( cancelled.error() ) );
        }
        if( !backend_.begin_edit )
        {
            return std::unexpected(
                missing_backend_operation( "begin-edit operation" )
            );
        }
        auto started = backend_.begin_edit( editable_ );
        if( !started.has_value() )
        {
            return std::unexpected( std::move( started.error() ) );
        }
        editing_ = true;
        return {};
    }

    Result<void>
    SketchController::cancel_draw()
    {
        interaction_.cancel();
        pressed_button_.reset();
        pending_preview_.reset();
        preview_dirty_ = false;
        return clear_preview();
    }

    Result<void>
    SketchController::clear_preview()
    {
        if( !preview_id_.has_value() )
        {
            return {};
        }
        if( !backend_.remove_shape )
        {
            return std::unexpected( missing_backend_operation( "remove operation" ) );
        }
        auto removed = backend_.remove_shape( *preview_id_ );
        if( !removed.has_value() )
        {
            return std::unexpected( std::move( removed.error() ) );
        }
        preview_id_.reset();
        return {};
    }

    Result<void>
    SketchController::commit( SpacePoint at )
    {
        auto shape = interaction_.commit( at );
        pressed_button_.reset();
        pending_preview_.reset();
        preview_dirty_ = false;
        auto cleared   = clear_preview();
        if( !cleared.has_value() )
        {
            return std::unexpected( std::move( cleared.error() ) );
        }
        if( !shape.has_value() )
        {
            return {};
        }
        if( !backend_.add_shape )
        {
            return std::unexpected( missing_backend_operation( "add operation" ) );
        }
        auto added = backend_.add_shape( std::move( *shape ) );
        if( !added.has_value() )
        {
            return std::unexpected( std::move( added.error() ) );
        }
        editable_.push_back( *added );
        return {};
    }

    Result<void>
    SketchController::remove_last_shape()
    {
        if( editable_.empty() )
        {
            return {};
        }
        if( !backend_.remove_shape )
        {
            return std::unexpected( missing_backend_operation( "remove operation" ) );
        }
        auto removed = backend_.remove_shape( editable_.back() );
        if( !removed.has_value() )
        {
            return std::unexpected( std::move( removed.error() ) );
        }
        editable_.pop_back();
        return {};
    }

    Result<SketchOptions>
    parse_sketch_options( std::span<const std::string_view> args )
    {
        SketchOptions options;
        bool          has_stroke{};
        bool          has_filled{};
        bool          has_color{};

        for( std::size_t index = 0U; index < args.size(); )
        {
            const auto flag = args.subspan( index, 1U ).front();
            if( flag == filledFlag )
            {
                if( has_filled )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "unknown or repeated sketch option: " +
                                     std::string{ flag } );
                }
                options.style.filled = true;
                has_filled           = true;
                ++index;
                continue;
            }
            if( flag != strokePxFlag && flag != colorFlag )
            {
                return fail( ErrorCode::InvalidArgument,
                             "unknown or repeated sketch option: " +
                                 std::string{ flag } );
            }
            if( ( flag == strokePxFlag && has_stroke ) ||
                ( flag == colorFlag && has_color ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "unknown or repeated sketch option: " +
                                 std::string{ flag } );
            }
            if( index + 1U >= args.size() )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } + " requires a value" );
            }
            const auto value = args.subspan( index, optionValueStep ).back();
            if( flag == strokePxFlag )
            {
                auto parsed = parse_stroke_width( value );
                if( !parsed.has_value() )
                {
                    return std::unexpected( std::move( parsed.error() ) );
                }
                options.style.stroke_px = *parsed;
                has_stroke              = true;
            }
            else
            {
                auto parsed = parse_color( value );
                if( !parsed.has_value() )
                {
                    return std::unexpected( std::move( parsed.error() ) );
                }
                options.style.color = *parsed;
                has_color           = true;
            }
            index += optionValueStep;
        }
        return options;
    }

    int
    run_sketch_command( std::span<char* const> args )
    {
        auto views = argument_views( args );
        if( !views.has_value() )
        {
            return report_parse_error( views.error() );
        }
        auto options = parse_sketch_options( *views );
        if( !options.has_value() )
        {
            return report_parse_error( options.error() );
        }
        return report_runtime_result( execute_sketch( *options ) );
    }

}    // namespace grab::cli
