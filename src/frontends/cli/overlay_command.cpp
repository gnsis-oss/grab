#include "frontends/cli/common.hpp"
#include "frontends/cli/overlay_command.hpp"
#include "grab/event.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"
#include "kernel/scheduling/reactor.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
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

        constexpr std::string_view atFlag               = "--at";
        constexpr std::string_view colorFlag            = "--color";
        constexpr std::string_view injectedColorFlag    = "--injected-color";
        constexpr std::string_view fadeFlag             = "--fade";
        constexpr std::string_view fadeMsFlag           = "--fade-ms";
        constexpr std::string_view holdFlag             = "--hold";
        constexpr std::string_view ttlFlag              = "--ttl";
        constexpr std::string_view widthFlag            = "--width";
        constexpr std::string_view trailVerb            = "trail";
        constexpr std::string_view rectVerb             = "rect";
        constexpr std::string_view ellipseVerb          = "ellipse";
        constexpr std::string_view pathVerb             = "path";
        constexpr char             valueSeparator       = ',';
        constexpr std::size_t      colorTextLength      = 6U;
        constexpr std::size_t      hexDigitsPerByte     = 2U;
        constexpr std::size_t      redTextOffset        = 0U;
        constexpr std::size_t      greenTextOffset      = 2U;
        constexpr std::size_t      blueTextOffset       = 4U;
        constexpr std::size_t      rectValueCount       = 4U;
        constexpr std::size_t      ellipseValueCount    = 4U;
        constexpr std::size_t      minimumPathValues    = 4U;
        constexpr std::size_t      coordinatesPerPoint  = 2U;
        constexpr std::size_t      firstPathPointOffset = 2U;
        constexpr std::uint8_t opaqueChannel = std::numeric_limits<std::uint8_t>::max();
        constexpr std::uint8_t decimalDigitBase = 10U;
        constexpr std::uint8_t hexadecimalBase  = 16U;
        constexpr char         lowerHexA        = 'a';
        constexpr char         lowerHexF        = 'f';
        constexpr char         upperHexA        = 'A';
        constexpr char         upperHexF        = 'F';
        constexpr char         asciiZero        = '0';
        constexpr char         asciiNine        = '9';
        constexpr std::uint8_t alphaHexOffset   = 10U;
        constexpr std::uint8_t annotationGreen  = 196U;
        constexpr std::chrono::milliseconds defaultShapeTtl{ 3'000 };
        constexpr overlay::Color            defaultAnnotationColor{
            .r = opaqueChannel,
            .g = annotationGreen,
            .b = 0U,
            .a = opaqueChannel,
        };
        constexpr float             defaultAnnotationWidthPx = 3.0F;
        // Shape parsing runs before any session exists; geometry is produced
        // space-unresolved and open_shape stamps the overlay's real space.
        constexpr CoordinateSpaceId unresolvedSpace{};
        constexpr std::uint32_t     signalEvents =
            static_cast<std::uint32_t>( EPOLLIN | EPOLLERR | EPOLLHUP );
        constexpr int         posixSuccess    = 0;
        constexpr int         invalidFile     = -1;
        constexpr std::size_t firstArgument   = 0U;
        constexpr std::size_t optionValueStep = 2U;

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

        class BlockedSignals final
        {
            public:

                [[nodiscard]]
                static Result<BlockedSignals>
                create()
                {
                    sigset_t signals{};    // NOLINT(misc-include-cleaner)
                    // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX signal.h.
                    if( ::sigemptyset( &signals ) !=
                        posixSuccess ||
                        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX
                        // signal.h.
                        ::sigaddset( &signals, SIGINT ) !=
                        posixSuccess ||
                        // NOLINTNEXTLINE(misc-include-cleaner): provided by POSIX
                        // signal.h.
                        ::sigaddset( &signals, SIGTERM ) != posixSuccess )
                    {
                        return std::unexpected(
                            system_error( "configure overlay signals", errno )
                        );
                    }

                    sigset_t  old_mask{};    // NOLINT(misc-include-cleaner)
                    const int blocked =
                        ::pthread_sigmask( SIG_BLOCK, &signals, &old_mask );
                    if( blocked != posixSuccess )
                    {
                        return std::unexpected( system_error( "block overlay signals",
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
                            system_error( "open overlay signal descriptor", open_error )
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
                    descriptor_{
                        std::exchange( other.descriptor_,
                                       invalidFile ),
                    },
                    old_mask_{ other.old_mask_ },
                    restore_mask_{
                        std::exchange( other.restore_mask_,
                                       false ),
                    }
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
        std::optional<std::uint8_t>
        hex_digit( char value ) noexcept
        {
            if( value >= asciiZero && value <= asciiNine )
            {
                return static_cast<std::uint8_t>( value - asciiZero );
            }
            if( value >= lowerHexA && value <= lowerHexF )
            {
                return static_cast<std::uint8_t>( value - lowerHexA + alphaHexOffset );
            }
            if( value >= upperHexA && value <= upperHexF )
            {
                return static_cast<std::uint8_t>( value - upperHexA + alphaHexOffset );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        Result<overlay::Color>
        parse_color( std::string_view text,
                     std::string_view flag )
        {
            if( text.size() != colorTextLength )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } + " must match RRGGBB" );
            }

            const auto parse_channel =
                [text, flag]( std::size_t offset ) -> Result<std::uint8_t>
            {
                const auto channel = text.substr( offset, hexDigitsPerByte );
                const auto high    = hex_digit( channel.front() );
                const auto low     = hex_digit( channel.back() );
                if( !high.has_value() || !low.has_value() )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 std::string{ flag } + " must match RRGGBB" );
                }
                return static_cast<std::uint8_t>( ( *high * hexadecimalBase ) + *low );
            };
            auto red   = parse_channel( redTextOffset );
            auto green = parse_channel( greenTextOffset );
            auto blue  = parse_channel( blueTextOffset );
            if( !red.has_value() )
            {
                return std::unexpected( std::move( red.error() ) );
            }
            if( !green.has_value() )
            {
                return std::unexpected( std::move( green.error() ) );
            }
            if( !blue.has_value() )
            {
                return std::unexpected( std::move( blue.error() ) );
            }
            return overlay::Color{
                .r = *red,
                .g = *green,
                .b = *blue,
                .a = opaqueChannel,
            };
        }

        [[nodiscard]]
        Result<std::chrono::milliseconds>
        parse_duration( std::string_view text,
                        std::string_view flag )
        {
            std::chrono::milliseconds::rep value{};
            const auto* const              begin = text.begin();
            const auto* const              end   = text.end();
            const auto parsed = std::from_chars( begin, end, value, decimalDigitBase );
            if( text.empty() ||
                parsed.ec !=
                std::errc{} ||
                parsed.ptr !=
                end ||
                value <= 0 )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } +
                                 " must be a positive millisecond count" );
            }
            return std::chrono::milliseconds{ value };
        }

        [[nodiscard]]
        Result<double>
        parse_finite_number( std::string_view text,
                             std::string_view field )
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
                !std::isfinite( value ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ field } + " must be a finite number" );
            }
            return value;
        }

        [[nodiscard]]
        Result<float>
        parse_width( std::string_view text )
        {
            auto value = parse_finite_number( text, widthFlag );
            if( !value.has_value() ||
                *value <=
                0.0 ||
                *value > static_cast<double>( std::numeric_limits<float>::max() ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "--width must be a positive finite number" );
            }
            return static_cast<float>( *value );
        }

        [[nodiscard]]
        Result<std::vector<double>>
        parse_coordinates( std::string_view text )
        {
            std::vector<double> values;
            std::size_t         begin{};
            while( begin <= text.size() )
            {
                const auto separator = text.find( valueSeparator, begin );
                const auto end =
                    separator == std::string_view::npos ? text.size() : separator;
                auto value =
                    parse_finite_number( text.substr( begin, end - begin ), atFlag );
                if( !value.has_value() )
                {
                    return std::unexpected( std::move( value.error() ) );
                }
                values.push_back( *value );
                if( separator == std::string_view::npos )
                {
                    break;
                }
                begin = separator + 1U;
            }
            return values;
        }

        [[nodiscard]]
        overlay::StrokeStyle
        annotation_stroke() noexcept
        {
            return overlay::StrokeStyle{
                .color    = defaultAnnotationColor,
                .width_px = defaultAnnotationWidthPx,
            };
        }

        [[nodiscard]]
        Result<overlay::Geometry>
        geometry_for( std::string_view        verb,
                      std::span<const double> coordinates )
        {
            if( verb == rectVerb )
            {
                if( coordinates.size() != rectValueCount )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "rect --at must match X,Y,W,H with positive W,H" );
                }
                const auto values = coordinates.first<rectValueCount>();
                const auto x      = values.front();
                const auto y      = values.subspan( 1U ).front();
                const auto width  = values.subspan( 2U ).front();
                const auto height = values.back();
                if( width <= 0.0 || height <= 0.0 )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "rect --at must match X,Y,W,H with positive W,H" );
                }
                return overlay::Rect{
                    .bounds = {
                               .x     = x,
                               .y     = y,
                               .w     = width,
                               .h     = height,
                               .space = unresolvedSpace,
                               },
                };
            }
            if( verb == ellipseVerb )
            {
                if( coordinates.size() != ellipseValueCount )
                {
                    return fail(
                        ErrorCode::InvalidArgument,
                        "ellipse --at must match X,Y,RX,RY with positive radii"
                    );
                }
                const auto values   = coordinates.first<ellipseValueCount>();
                const auto center_x = values.front();
                const auto center_y = values.subspan( 1U ).front();
                const auto radius_x = values.subspan( 2U ).front();
                const auto radius_y = values.back();
                if( radius_x <= 0.0 || radius_y <= 0.0 )
                {
                    return fail(
                        ErrorCode::InvalidArgument,
                        "ellipse --at must match X,Y,RX,RY with positive radii"
                    );
                }
                return overlay::Ellipse{
                    .center =
                        {
                                 .x     = center_x,
                                 .y     = center_y,
                                 .space = unresolvedSpace,
                                 },
                    .radius_x = radius_x,
                    .radius_y = radius_y,
                };
            }
            if( verb != pathVerb )
            {
                return fail( ErrorCode::InvalidArgument,
                             "overlay shape must be rect, ellipse, or path" );
            }
            if( coordinates.size() <
                minimumPathValues ||
                coordinates.size() %
                coordinatesPerPoint != 0U )
            {
                return fail( ErrorCode::InvalidArgument,
                             "path --at must contain at least two X,Y points" );
            }

            overlay::Path path;
            path.commands.reserve( coordinates.size() / coordinatesPerPoint );
            const auto first_point = coordinates.first<coordinatesPerPoint>();
            path.commands.emplace_back( overlay::MoveTo{
                .point = {
                          .x     = first_point.front(),
                          .y     = first_point.back(),
                          .space = unresolvedSpace,
                          },
            } );
            for( std::size_t index  = firstPathPointOffset; index < coordinates.size();
                 index             += coordinatesPerPoint )
            {
                const auto point = coordinates.subspan( index, coordinatesPerPoint );
                path.commands.emplace_back( overlay::LineTo{
                    .point = {
                              .x     = point.front(),
                              .y     = point.back(),
                              .space = unresolvedSpace,
                              },
                } );
            }
            return path;
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
                                 "overlay command argument is missing" );
                }
                result.emplace_back( argument );
            }
            return result;
        }

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
        wait_until_timer( Session&                  session,
                          std::chrono::milliseconds duration )
        {
            auto state = std::make_shared<CompletionState>();
            try
            {
                static_cast<void>( session.reactor().add_timer( duration,
                                                                [state]
                                                                {
                                                                    complete( state );
                                                                } ) );
            }
            catch( const std::exception& error )
            {
                return fail( ErrorCode::InternalFault,
                             std::string{ "schedule overlay lifetime: " } +
                                 error.what() );
            }
            catch( ... )
            {
                return fail( ErrorCode::InternalFault,
                             "schedule overlay lifetime failed" );
            }
            wait_for_completion( *state );
            return {};
        }

        [[nodiscard]]
        Result<void>
        wait_until_signal( Session&              session,
                           const BlockedSignals& signals )
        {
            auto          state = std::make_shared<CompletionState>();
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
                             std::string{ "watch overlay signals: " } + error.what() );
            }
            catch( ... )
            {
                return fail( ErrorCode::InternalFault, "watch overlay signals failed" );
            }

            wait_for_completion( *state );
            session.reactor().remove_fd( token );
            return reactor_barrier( session );
        }

        struct ActiveOverlay
        {
                std::unique_ptr<Session> session;
                Overlay*                 overlay{};
        };

        void
        stamp_space( overlay::Shape&   shape,
                     CoordinateSpaceId space )
        {
            std::visit(
                [space]( auto& geometry )
                {
                    using Geometry = std::decay_t<decltype( geometry )>;
                    if constexpr( std::is_same_v<Geometry, overlay::Rect> )
                    {
                        geometry.bounds.space = space;
                    }
                    else if constexpr( std::is_same_v<Geometry, overlay::Ellipse> )
                    {
                        geometry.center.space = space;
                    }
                    else if constexpr( std::is_same_v<Geometry, overlay::Polygon> )
                    {
                        for( auto& point : geometry.points )
                        {
                            point.space = space;
                        }
                    }
                    else
                    {
                        for( auto& command : geometry.commands )
                        {
                            std::visit(
                                [space]( auto& step )
                                {
                                    using Step = std::decay_t<decltype( step )>;
                                    if constexpr( std::is_same_v<Step,
                                                                 overlay::MoveTo> ||
                                                  std::is_same_v<Step, overlay::LineTo> )
                                    {
                                        step.point.space = space;
                                    }
                                    else if constexpr( std::is_same_v<
                                                           Step,
                                                           overlay::BezierTo> )
                                    {
                                        for( auto& control : step.control )
                                        {
                                            control.space = space;
                                        }
                                    }
                                },
                                command
                            );
                        }
                    }
                },
                shape.geometry
            );
        }

        [[nodiscard]]
        Result<ActiveOverlay>
        open_shape( overlay::Shape shape )
        {
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
            auto surface_space = ( *overlay )->space();
            if( !surface_space.has_value() )
            {
                return std::unexpected( std::move( surface_space.error() ) );
            }
            stamp_space( shape, *surface_space );
            auto added = ( *overlay )->add( std::move( shape ) );
            if( !added.has_value() )
            {
                return std::unexpected( std::move( added.error() ) );
            }
            auto flushed = ( *overlay )->flush();
            if( !flushed.has_value() )
            {
                return std::unexpected( std::move( flushed.error() ) );
            }
            return ActiveOverlay{
                .session = std::move( *session ),
                .overlay = *overlay,
            };
        }

        class TrailBridge final
        {
            public:

                TrailBridge( Overlay&                   overlay,
                             const OverlayTrailOptions& options ) :
                    scene_{
                        []
                        {
                            return std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()
                            );
                          }
                },
                    animator_{
                        scene_,
                        kernel::presentation::TrailStyle{
                            .physical = options.physical_color,
                            .injected = options.injected_color,
                            .fade     = options.fade,
                            .width_px = options.width_px,
                        }
                    },
                    overlay_{ &overlay }
                {
                    scene_.set_delta_sink(
                        [this]( const overlay::SceneDelta& delta )
                        {
                            const auto* const upsert =
                                std::get_if<overlay::Upsert>( &delta.change );
                            if( upsert == nullptr )
                            {
                                return;
                            }
                            auto added = overlay_->add( upsert->record.shape );
                            if( !added.has_value() )
                            {
                                const std::scoped_lock lock{ error_mutex_ };
                                if( !error_.has_value() )
                                {
                                    error_ = std::move( added.error() );
                                }
                            }
                        }
                    );
                }

                void
                consume( const SubscriptionEvent& item )
                {
                    animator_.consume( item );
                }

                [[nodiscard]]
                std::optional<Error>
                error() const
                {
                    const std::scoped_lock lock{ error_mutex_ };
                    return error_;
                }

            private:

                kernel::presentation::OverlayScene  scene_;
                kernel::presentation::TrailAnimator animator_;
                Overlay*                            overlay_{};
                mutable std::mutex                  error_mutex_;
                std::optional<Error>                error_;
        };

        class TrailDrainState final
            : public std::enable_shared_from_this<TrailDrainState>
        {
            public:

                TrailDrainState( Session&     session,
                                 Subscription subscription,
                                 TrailBridge& bridge ) :
                    session_{ &session },
                    subscription_{ std::move( subscription ) },
                    bridge_{ &bridge }
                {
                }

                void
                install()
                {
                    const std::weak_ptr<TrailDrainState> weak = weak_from_this();
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
                    subscription_.set_notify( {} );
                    session_->stop_observation();
                    return reactor_barrier( *session_ );
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
                    while( true )
                    {
                        while( auto item = subscription_.try_pop_item() )
                        {
                            bridge_->consume( *item );
                        }

                        scheduled_.store( false );
                        auto raced = subscription_.try_pop_item();
                        if( !raced.has_value() )
                        {
                            return;
                        }

                        bool expected = false;
                        if( scheduled_.compare_exchange_strong( expected, true ) )
                        {
                            bridge_->consume( *raced );
                            continue;
                        }
                        bridge_->consume( *raced );
                        return;
                    }
                }

                void
                remember_error( Error error )
                {
                    const std::scoped_lock lock{ error_mutex_ };
                    if( !error_.has_value() )
                    {
                        error_ = std::move( error );
                    }
                }

                Session*             session_{};
                Subscription         subscription_;
                TrailBridge*         bridge_{};
                std::atomic_bool     scheduled_;
                mutable std::mutex   error_mutex_;
                std::optional<Error> error_;
        };

        [[nodiscard]]
        Result<void>
        execute_shape( OverlayShapeRequest request )
        {
            if( request.wait_for.has_value() )
            {
                // The lifetime clock starts when add() stamps started_at, not
                // when flush() returns; wait only the time remaining to the
                // absolute policy deadline so fence latency never extends the
                // shape's on-screen life.
                const auto policy_start = std::chrono::steady_clock::now();
                auto       active       = open_shape( std::move( request.shape ) );
                if( !active.has_value() )
                {
                    return std::unexpected( std::move( active.error() ) );
                }
                const auto deadline = policy_start + *request.wait_for;
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()
                    );
                if( remaining <= std::chrono::milliseconds::zero() )
                {
                    return {};
                }
                return wait_until_timer( *active->session, remaining );
            }

            auto signals = BlockedSignals::create();
            if( !signals.has_value() )
            {
                return std::unexpected( std::move( signals.error() ) );
            }
            auto active = open_shape( std::move( request.shape ) );
            if( !active.has_value() )
            {
                return std::unexpected( std::move( active.error() ) );
            }
            return wait_until_signal( *active->session, *signals );
        }

        [[nodiscard]]
        Result<void>
        execute_trail( const OverlayTrailOptions& options )
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
            scope.kinds       = { EventKind::MouseMove };
            auto subscription = ( *session )->watch( std::move( scope ) );
            if( !subscription.has_value() )
            {
                return std::unexpected( std::move( subscription.error() ) );
            }

            TrailBridge bridge{ **overlay, options };
            auto drain = std::make_shared<TrailDrainState>( **session,
                                                            std::move( *subscription ),
                                                            bridge );
            drain->install();
            auto observation = ( *session )->start_observation();
            if( !observation.has_value() )
            {
                return std::unexpected( std::move( observation.error() ) );
            }

            auto waited  = wait_until_signal( **session, *signals );
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
            if( auto error = bridge.error(); error.has_value() )
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

    Result<OverlayTrailOptions>
    parse_overlay_trail_options( std::span<const std::string_view> args )
    {
        OverlayTrailOptions options{
            .physical_color = kernel::presentation::defaultPhysicalTrailColor,
            .injected_color = kernel::presentation::defaultInjectedTrailColor,
            .fade           = kernel::presentation::defaultTrailFade,
            .width_px       = kernel::presentation::defaultTrailWidthPx,
        };
        bool has_color{};
        bool has_injected_color{};
        bool has_fade{};
        bool has_width{};
        for( std::size_t index = firstArgument; index < args.size(); )
        {
            const auto flag = args.subspan( index, 1U ).front();
            if( index + 1U >= args.size() )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } + " requires a value" );
            }
            const auto pair  = args.subspan( index, optionValueStep );
            const auto value = pair.back();
            if( flag == colorFlag && !has_color )
            {
                auto color = parse_color( value, flag );
                if( !color.has_value() )
                {
                    return std::unexpected( std::move( color.error() ) );
                }
                options.physical_color = *color;
                has_color              = true;
            }
            else if( flag == injectedColorFlag && !has_injected_color )
            {
                auto color = parse_color( value, flag );
                if( !color.has_value() )
                {
                    return std::unexpected( std::move( color.error() ) );
                }
                options.injected_color = *color;
                has_injected_color     = true;
            }
            else if( flag == fadeMsFlag && !has_fade )
            {
                auto fade = parse_duration( value, flag );
                if( !fade.has_value() )
                {
                    return std::unexpected( std::move( fade.error() ) );
                }
                options.fade = *fade;
                has_fade     = true;
            }
            else if( flag == widthFlag && !has_width )
            {
                auto width = parse_width( value );
                if( !width.has_value() )
                {
                    return std::unexpected( std::move( width.error() ) );
                }
                options.width_px = *width;
                has_width        = true;
            }
            else
            {
                return fail( ErrorCode::InvalidArgument,
                             "unknown or repeated trail option: " +
                                 std::string{ flag } );
            }
            index += optionValueStep;
        }
        return options;
    }

    Result<OverlayShapeRequest>
    parse_overlay_shape_options( std::string_view                  verb,
                                 std::span<const std::string_view> args )
    {
        std::optional<std::vector<double>>       coordinates;
        std::optional<std::chrono::milliseconds> duration;
        enum class Lifetime : std::uint8_t
        {
            DefaultTtl,
            Ttl,
            Fade,
            Hold,
        };
        Lifetime lifetime = Lifetime::DefaultTtl;
        bool     has_lifetime{};

        for( std::size_t index = firstArgument; index < args.size(); )
        {
            const auto flag = args.subspan( index, 1U ).front();
            if( flag == holdFlag )
            {
                if( has_lifetime )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "overlay accepts one lifetime policy" );
                }
                lifetime     = Lifetime::Hold;
                has_lifetime = true;
                ++index;
                continue;
            }
            if( index + 1U >= args.size() )
            {
                return fail( ErrorCode::InvalidArgument,
                             std::string{ flag } + " requires a value" );
            }
            const auto value = args.subspan( index, optionValueStep ).back();
            if( flag == atFlag )
            {
                if( coordinates.has_value() )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "overlay accepts one --at value" );
                }
                auto parsed = parse_coordinates( value );
                if( !parsed.has_value() )
                {
                    return std::unexpected( std::move( parsed.error() ) );
                }
                coordinates = std::move( *parsed );
            }
            else if( flag == ttlFlag || flag == fadeFlag )
            {
                if( has_lifetime )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "overlay accepts one lifetime policy" );
                }
                auto parsed = parse_duration( value, flag );
                if( !parsed.has_value() )
                {
                    return std::unexpected( std::move( parsed.error() ) );
                }
                duration     = *parsed;
                lifetime     = flag == ttlFlag ? Lifetime::Ttl : Lifetime::Fade;
                has_lifetime = true;
            }
            else
            {
                return fail( ErrorCode::InvalidArgument,
                             "unknown overlay shape option: " + std::string{ flag } );
            }
            index += optionValueStep;
        }

        if( !coordinates.has_value() )
        {
            return fail( ErrorCode::InvalidArgument,
                         std::string{ verb } + " requires --at" );
        }
        auto geometry = geometry_for( verb, *coordinates );
        if( !geometry.has_value() )
        {
            return std::unexpected( std::move( geometry.error() ) );
        }

        overlay::LifetimePolicy                  policy;
        std::optional<std::chrono::milliseconds> wait_for;
        const auto selected_duration = duration.value_or( defaultShapeTtl );
        switch( lifetime )
        {
            case Lifetime::DefaultTtl :
                policy   = overlay::Ttl{ .duration = defaultShapeTtl };
                wait_for = defaultShapeTtl;
                break;
            case Lifetime::Ttl :
                policy   = overlay::Ttl{ .duration = selected_duration };
                wait_for = duration;
                break;
            case Lifetime::Fade :
                policy   = overlay::Fade{ .duration = selected_duration };
                wait_for = duration;
                break;
            case Lifetime::Hold :
                policy = overlay::Persistent{};
                break;
        }

        return OverlayShapeRequest{
            .shape =
                overlay::Shape{
                               .geometry = std::move( *geometry ),
                               .stroke   = annotation_stroke(),
                               .fill     = std::nullopt,
                               .lifetime = policy,
                               .band     = overlay::Band::Annotation,
                               },
            .wait_for = wait_for,
        };
    }

    int
    run_overlay_command( std::span<char* const> args )
    {
        auto views = argument_views( args );
        if( !views.has_value() )
        {
            return report_parse_error( views.error() );
        }
        if( views->empty() )
        {
            return report_parse_error( Error{
                .code       = ErrorCode::InvalidArgument,
                .message    = "overlay requires trail, rect, ellipse, or path",
                .capability = {},
                .target     = {},
                .attempts   = {},
            } );
        }
        const auto verb    = views->front();
        const auto options = std::span<const std::string_view>{ *views }.subspan( 1U );
        if( verb == trailVerb )
        {
            auto parsed = parse_overlay_trail_options( options );
            if( !parsed.has_value() )
            {
                return report_parse_error( parsed.error() );
            }
            return report_runtime_result( execute_trail( *parsed ) );
        }

        auto parsed = parse_overlay_shape_options( verb, options );
        if( !parsed.has_value() )
        {
            return report_parse_error( parsed.error() );
        }
        return report_runtime_result( execute_shape( std::move( *parsed ) ) );
    }

    int
    run_trail_command( std::span<char* const> args )
    {
        auto views = argument_views( args );
        if( !views.has_value() )
        {
            return report_parse_error( views.error() );
        }
        auto parsed = parse_overlay_trail_options( *views );
        if( !parsed.has_value() )
        {
            return report_parse_error( parsed.error() );
        }
        return report_runtime_result( execute_trail( *parsed ) );
    }

}    // namespace grab::cli
