#include "drivers/desktop/x11/overlay_delegate.hpp"
#include "drivers/desktop/x11/x11_capture_route.hpp"
#include "drivers/desktop/x11/x11_runtime.hpp"
#include "grab/drag.hpp"
#include "grab/event.hpp"
#include "grab/geometry/point.hpp"
#include "grab/image.hpp"
#include "grab/interaction.hpp"
#include "grab/locator.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/role.hpp"
#include "grab/screen.hpp"
#include "grab/session.hpp"
#include "grab/space.hpp"
#include "grab/trace.hpp"
#include "grab/ui.hpp"
#include "grab/watch.hpp"
#include "kernel/presentation/overlay_scene.hpp"
#include "kernel/presentation/trail_animator.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>
// clang-format on

namespace
{

    using grab::drivers::desktop::x11::X11OverlayDelegate;
    namespace x11_detail = grab::drivers::desktop::x11::detail;

    constexpr std::string_view compositedDisplay{ ":88" };
    constexpr std::string_view bareDisplay{ ":90" };
    constexpr std::string_view randrDisplay{ ":98" };
    constexpr std::string_view compositorDependencyReason{
        "no compositing manager installed (test-only dependency)"
    };
    constexpr std::string_view compositorSelection{ "_NET_WM_CM_S0" };
    constexpr std::string_view sentinelTitle{ "grab overlay ring-3 sentinel" };
    constexpr std::string_view sentinelClass{ "grab-overlay-ring3" };
    constexpr std::string_view netClientList{ "_NET_CLIENT_LIST" };
    constexpr std::string_view netWmName{ "_NET_WM_NAME" };
    constexpr std::string_view utf8String{ "UTF8_STRING" };
    constexpr int              xcbSuccess             = 0;
    constexpr std::uint8_t     propertyFormat8        = 8U;
    constexpr std::uint8_t     propertyFormat32       = 32U;
    constexpr std::uint8_t     syntheticEventMask     = 0X7FU;
    constexpr std::uint16_t    sentinelX              = 120U;
    constexpr std::uint16_t    sentinelY              = 120U;
    constexpr std::uint16_t    sentinelWidth          = 560U;
    constexpr std::uint16_t    sentinelHeight         = 240U;
    constexpr std::int32_t     dragStartX             = 180;
    constexpr std::int32_t     dragEndX               = 600;
    constexpr std::int32_t     dragY                  = 220;
    constexpr double           trailProbeX            = 390.0;
    constexpr double           trailProbeTolerance    = 12.0;
    constexpr std::int32_t     dragInterpolationSteps = 512;
    constexpr std::size_t      dragQueueCapacity      = 4'096U;
    constexpr auto             integrationDeadline    = std::chrono::seconds{ 5 };
    constexpr auto             trailFade              = std::chrono::seconds{ 10 };
    constexpr float            trailWidth             = 7.0F;
    constexpr std::uint32_t    annotationX            = 40U;
    constexpr std::uint32_t    annotationY            = 40U;
    constexpr std::uint32_t    annotationWidth        = 80U;
    constexpr std::uint32_t    annotationHeight       = 60U;
    constexpr std::uint32_t    annotationProbeX = annotationX + ( annotationWidth / 2U );
    constexpr std::uint32_t annotationProbeY = annotationY + ( annotationHeight / 2U );
    constexpr std::uint8_t  strongChannel    = 180U;
    constexpr std::uint8_t  weakChannel      = 90U;
    constexpr std::uint8_t  opaqueChannel    = std::numeric_limits<std::uint8_t>::max();
    constexpr std::uint16_t resizedWidth     = 1'024U;
    constexpr std::uint16_t resizedHeight    = 768U;

    template<typename Type>
    using XcbOwned = std::unique_ptr<Type, decltype( &std::free )>;

    template<typename Type>
    [[nodiscard]]
    XcbOwned<Type>
    take_xcb_owned( Type* pointer ) noexcept
    {
        return XcbOwned<Type>{ pointer, &std::free };
    }

    class DisplayConnection final
    {
        public:

            explicit DisplayConnection( std::string_view display )
            {
                const std::string display_name{ display };
                connection_ = xcb_connect( display_name.c_str(), &screen_index_ );
                if( connection_ ==
                    nullptr ||
                    xcb_connection_has_error( connection_ ) != xcbSuccess )
                {
                    return;
                }
                auto iterator = xcb_setup_roots_iterator( xcb_get_setup( connection_ ) );
                for( int index = 0; index < screen_index_ && iterator.rem > 0; ++index )
                {
                    xcb_screen_next( &iterator );
                }
                screen_ = iterator.data;
            }

            ~DisplayConnection()
            {
                if( connection_ != nullptr )
                {
                    xcb_disconnect( connection_ );
                }
            }

            DisplayConnection( const DisplayConnection& ) = delete;
            DisplayConnection&
            operator=( const DisplayConnection& )    = delete;
            DisplayConnection( DisplayConnection&& ) = delete;
            DisplayConnection&
            operator=( DisplayConnection&& ) = delete;

            [[nodiscard]]
            bool
            valid() const noexcept
            {
                return connection_ !=
                       nullptr &&
                       screen_ !=
                       nullptr &&
                       xcb_connection_has_error( connection_ ) == xcbSuccess;
            }

            [[nodiscard]]
            xcb_connection_t*
            get() const noexcept
            {
                return connection_;
            }

            [[nodiscard]]
            xcb_screen_t*
            screen() const noexcept
            {
                return screen_;
            }

            [[nodiscard]]
            xcb_window_t
            root() const noexcept
            {
                return screen_ == nullptr ? XCB_WINDOW_NONE : screen_->root;
            }

        private:

            xcb_connection_t* connection_{};
            xcb_screen_t*     screen_{};
            int               screen_index_{};
    };

    // Environment mutation is confined to a standalone gtest process and ends
    // only after its Session has joined every worker.
    // NOLINTBEGIN(concurrency-mt-unsafe,misc-include-cleaner)
    class ScopedDisplay final
    {
        public:

            explicit ScopedDisplay( std::string_view display )
            {
                // NOLINTNEXTLINE(concurrency-mt-unsafe): isolated gtest process.
                if( const char* const current = std::getenv( "DISPLAY" );
                    current != nullptr )
                {
                    previous_ = current;
                }
                const std::string value{ display };
                // NOLINTNEXTLINE(concurrency-mt-unsafe): set before Session threads
                // start.
                active_ =
                    ::setenv( "DISPLAY", value.c_str(), 1 ) ==
                    xcbSuccess;    // NOLINT(concurrency-mt-unsafe,misc-include-cleaner)
            }

            ~ScopedDisplay()
            {
                if( !active_ )
                {
                    return;
                }
                if( previous_.has_value() )
                {
                    // NOLINTNEXTLINE(concurrency-mt-unsafe): Session is already
                    // destroyed.
                    static_cast<void>(
                        ::setenv( "DISPLAY", previous_->c_str(), 1 )
                    );    // NOLINT(concurrency-mt-unsafe,misc-include-cleaner)
                }
                else
                {
                    // NOLINTNEXTLINE(concurrency-mt-unsafe): Session is already
                    // destroyed.
                    static_cast<void>(
                        ::unsetenv( "DISPLAY" )
                    );    // NOLINT(concurrency-mt-unsafe,misc-include-cleaner)
                }
            }

            ScopedDisplay( const ScopedDisplay& ) = delete;
            ScopedDisplay&
            operator=( const ScopedDisplay& ) = delete;
            ScopedDisplay( ScopedDisplay&& )  = delete;
            ScopedDisplay&
            operator=( ScopedDisplay&& ) = delete;

            [[nodiscard]]
            bool
            active() const noexcept
            {
                return active_;
            }

        private:

            std::optional<std::string> previous_;
            bool                       active_{};
    };

    // NOLINTEND(concurrency-mt-unsafe,misc-include-cleaner)

    [[nodiscard]]
    bool
    request_succeeded( xcb_connection_t* connection,
                       xcb_void_cookie_t request )
    {
        const auto error = take_xcb_owned( xcb_request_check( connection, request ) );
        return error == nullptr;
    }

    [[nodiscard]]
    xcb_atom_t
    intern_atom( xcb_connection_t* connection,
                 std::string_view  name )
    {
        const auto reply = take_xcb_owned( xcb_intern_atom_reply(
            connection,
            xcb_intern_atom( connection,
                             0U,
                             static_cast<std::uint16_t>( name.size() ),
                             name.data() ),
            nullptr
        ) );
        return reply == nullptr ? XCB_ATOM_NONE : reply->atom;
    }

    [[nodiscard]]
    xcb_window_t
    compositor_owner( DisplayConnection& display )
    {
        const auto selection = intern_atom( display.get(), compositorSelection );
        if( selection == XCB_ATOM_NONE )
        {
            return XCB_WINDOW_NONE;
        }
        const auto reply = take_xcb_owned( xcb_get_selection_owner_reply(
            display.get(),
            xcb_get_selection_owner( display.get(), selection ),
            nullptr
        ) );
        return reply == nullptr ? XCB_WINDOW_NONE : reply->owner;
    }

    [[nodiscard]]
    bool
    executable_on_path( std::string_view executable )
    {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): read-only process environment.
        const char* const path_value = std::getenv( "PATH" );
        if( path_value == nullptr )
        {
            return false;
        }
        std::string_view path{ path_value };
        while( true )
        {
            const auto                  separator = path.find( ':' );
            const auto                  directory = path.substr( 0U, separator );
            const std::filesystem::path candidate =
                ( directory.empty() ? std::filesystem::path{ "." }
                                    : std::filesystem::path{ directory } ) /
                executable;
            if( ::access( candidate.c_str(), X_OK ) == xcbSuccess )
            {
                return true;
            }
            if( separator == std::string_view::npos )
            {
                return false;
            }
            path.remove_prefix( separator + 1U );
        }
    }

    [[nodiscard]]
    bool
    compositor_is_installed()
    {
        return executable_on_path( "picom" ) || executable_on_path( "xcompmgr" );
    }

    [[nodiscard]]
    std::vector<xcb_window_t>
    root_children( DisplayConnection& display )
    {
        const auto reply = take_xcb_owned(
            xcb_query_tree_reply( display.get(),
                                  xcb_query_tree( display.get(), display.root() ),
                                  nullptr )
        );
        if( reply == nullptr )
        {
            return {};
        }
        const auto                count = xcb_query_tree_children_length( reply.get() );
        const auto* const         children = xcb_query_tree_children( reply.get() );
        std::vector<xcb_window_t> result;
        if( count > 0 && children != nullptr )
        {
            const std::span<const xcb_window_t> children_view{
                children,
                static_cast<std::size_t>( count ),
            };
            result.assign( children_view.begin(), children_view.end() );
        }
        std::ranges::sort( result );
        return result;
    }

    [[nodiscard]]
    bool
    set_text_property( DisplayConnection& display,
                       xcb_window_t       window,
                       xcb_atom_t         property,
                       xcb_atom_t         type,
                       std::string_view   text )
    {
        return request_succeeded(
            display.get(),
            xcb_change_property_checked( display.get(),
                                         XCB_PROP_MODE_REPLACE,
                                         window,
                                         property,
                                         type,
                                         propertyFormat8,
                                         static_cast<std::uint32_t>( text.size() ),
                                         text.data() )
        );
    }

    struct SentinelWindow
    {
            DisplayConnection* display{};
            xcb_window_t       window{ XCB_WINDOW_NONE };

            SentinelWindow() = default;

            ~SentinelWindow()
            {
                if( display == nullptr || window == XCB_WINDOW_NONE )
                {
                    return;
                }
                const auto client_list = intern_atom( display->get(), netClientList );
                if( client_list != XCB_ATOM_NONE )
                {
                    static_cast<void>( xcb_delete_property( display->get(),
                                                            display->root(),
                                                            client_list ) );
                }
                static_cast<void>( xcb_destroy_window( display->get(), window ) );
                static_cast<void>( xcb_flush( display->get() ) );
            }

            SentinelWindow( const SentinelWindow& ) = delete;
            SentinelWindow&
            operator=( const SentinelWindow& ) = delete;
            SentinelWindow( SentinelWindow&& ) = delete;
            SentinelWindow&
            operator=( SentinelWindow&& ) = delete;
    };

    [[nodiscard]]
    bool
    create_sentinel( DisplayConnection& display,
                     SentinelWindow&    sentinel )
    {
        sentinel.display                  = &display;
        sentinel.window                   = xcb_generate_id( display.get() );
        constexpr std::uint32_t eventMask = XCB_EVENT_MASK_BUTTON_PRESS |
                                            XCB_EVENT_MASK_BUTTON_RELEASE |
                                            XCB_EVENT_MASK_POINTER_MOTION;
        const std::array        values{ display.screen()->black_pixel, eventMask };
        if( !request_succeeded( display.get(),
                                xcb_create_window_checked( display.get(),
                                                           XCB_COPY_FROM_PARENT,
                                                           sentinel.window,
                                                           display.root(),
                                                           sentinelX,
                                                           sentinelY,
                                                           sentinelWidth,
                                                           sentinelHeight,
                                                           0U,
                                                           XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                                           display.screen()->root_visual,
                                                           XCB_CW_BACK_PIXEL |
                                                               XCB_CW_EVENT_MASK,
                                                           values.data() ) ) )
        {
            return false;
        }

        const auto utf8        = intern_atom( display.get(), utf8String );
        const auto title       = intern_atom( display.get(), netWmName );
        const auto client_list = intern_atom( display.get(), netClientList );
        if( utf8 ==
            XCB_ATOM_NONE ||
            title ==
            XCB_ATOM_NONE ||
            client_list == XCB_ATOM_NONE )
        {
            return false;
        }
        std::string wm_class;
        wm_class.reserve( ( sentinelClass.size() * 2U ) + 2U );
        wm_class.append( sentinelClass );
        wm_class.push_back( '\0' );
        wm_class.append( sentinelClass );
        wm_class.push_back( '\0' );
        if( !set_text_property( display,
                                sentinel.window,
                                XCB_ATOM_WM_NAME,
                                XCB_ATOM_STRING,
                                sentinelTitle ) ||
            !set_text_property( display,
                                sentinel.window,
                                XCB_ATOM_WM_CLASS,
                                XCB_ATOM_STRING,
                                wm_class ) ||
            !set_text_property( display, sentinel.window, title, utf8, sentinelTitle ) )
        {
            return false;
        }
        if( !request_succeeded( display.get(),
                                // NOLINTNEXTLINE(readability-suspicious-call-argument)
                                xcb_change_property_checked( display.get(),
                                                             XCB_PROP_MODE_REPLACE,
                                                             display.root(),
                                                             client_list,
                                                             XCB_ATOM_WINDOW,
                                                             propertyFormat32,
                                                             1U,
                                                             &sentinel.window ) ) ||
            !request_succeeded( display.get(),
                                xcb_map_window_checked( display.get(),
                                                        sentinel.window ) ) )
        {
            return false;
        }
        return xcb_flush( display.get() ) > xcbSuccess;
    }

    struct ButtonDelivery
    {
            bool press{};
            bool release{};
    };

    [[nodiscard]]
    ButtonDelivery
    wait_for_drag_buttons( DisplayConnection& display,
                           xcb_window_t       sentinel )
    {
        ButtonDelivery delivered;
        const auto     deadline = std::chrono::steady_clock::now() + integrationDeadline;
        while( !( delivered.press && delivered.release ) )
        {
            while( const auto event =
                       take_xcb_owned( xcb_poll_for_event( display.get() ) ) )
            {
                const auto type = static_cast<std::uint8_t>( event->response_type &
                                                             syntheticEventMask );
                if( type == XCB_BUTTON_PRESS )
                {
                    const auto* const button =
                        // XCB owns a protocol buffer identified by response_type.
                        // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
                        std::bit_cast<const xcb_button_press_event_t*>( event.get() );
                    delivered.press = delivered.press || button->event == sentinel;
                }
                else if( type == XCB_BUTTON_RELEASE )
                {
                    const auto* const button =
                        // XCB owns a protocol buffer identified by response_type.
                        // NOLINTNEXTLINE(bugprone-bitwise-pointer-cast)
                        std::bit_cast<const xcb_button_release_event_t*>( event.get() );
                    delivered.release = delivered.release || button->event == sentinel;
                }
            }
            if( delivered.press && delivered.release )
            {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if( now >= deadline )
            {
                break;
            }
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>( deadline - now );
            // NOLINTNEXTLINE(misc-include-cleaner): POSIX poll API.
            pollfd target{
                .fd      = xcb_get_file_descriptor( display.get() ),
                .events  = POLLIN,    // NOLINT(misc-include-cleaner)
                .revents = 0,
            };
            static_cast<void>(
                ::poll( &target,    // NOLINT(misc-include-cleaner)
                        1U,
                        static_cast<int>( std::min<std::chrono::milliseconds::rep>(
                            remaining.count(),
                            std::numeric_limits<int>::max()
                        ) ) )
            );
        }
        return delivered;
    }

    [[nodiscard]]
    grab::overlay::Shape
    green_rectangle( grab::CoordinateSpaceId space )
    {
        return grab::overlay::Shape{
            .geometry =
                grab::overlay::Rect{
                                    .bounds =
                        {
                            .x     = static_cast<double>( annotationX ),
                            .y     = static_cast<double>( annotationY ),
                            .w     = static_cast<double>( annotationWidth ),
                            .h     = static_cast<double>( annotationHeight ),
                            .space = space,
                        }, },
            .stroke = std::nullopt,
            .fill =
                grab::overlay::FillStyle{
                                    .color =
                        {
                            .r = 0U,
                            .g = opaqueChannel,
                            .b = 0U,
                            .a = opaqueChannel,
                        }, },
            .lifetime = grab::overlay::Persistent{},
            .band     = grab::overlay::Band::Annotation,
        };
    }

    struct Rgb
    {
            std::uint8_t red{};
            std::uint8_t green{};
            std::uint8_t blue{};
    };

    [[nodiscard]]
    std::optional<Rgb>
    pixel_at( const grab::Image& image,
              std::uint32_t      x,
              std::uint32_t      y )
    {
        if( x >= image.width || y >= image.height )
        {
            return std::nullopt;
        }
        const auto row = image.row( y );
        const auto bytes =
            static_cast<std::size_t>( grab::bytes_per_pixel( image.format ) );
        const auto offset = static_cast<std::size_t>( x ) * bytes;
        if( bytes == 0U || offset > row.size() || row.size() - offset < bytes )
        {
            return std::nullopt;
        }
        const auto pixel   = row.subspan( offset, bytes );
        const auto channel = [pixel]( std::size_t index )
        {
            return std::to_integer<std::uint8_t>( pixel.subspan( index, 1U ).front() );
        };
        switch( image.format )
        {
            case grab::PixelFormat::Bgra :
            case grab::PixelFormat::Bgr :
                return Rgb{
                    .red   = channel( 2U ),
                    .green = channel( 1U ),
                    .blue  = channel( 0U ),
                };
            case grab::PixelFormat::Rgba :
            case grab::PixelFormat::Rgb :
                return Rgb{
                    .red   = channel( 0U ),
                    .green = channel( 1U ),
                    .blue  = channel( 2U ),
                };
            case grab::PixelFormat::Gray :
                return Rgb{
                    .red   = channel( 0U ),
                    .green = channel( 0U ),
                    .blue  = channel( 0U ),
                };
        }
        return std::nullopt;
    }

    [[nodiscard]]
    bool
    is_green( const Rgb& pixel ) noexcept
    {
        return pixel.green >=
               strongChannel &&
               pixel.red <=
               weakChannel &&
               pixel.blue <= weakChannel;
    }

    [[nodiscard]]
    bool
    is_blue( const Rgb& pixel ) noexcept
    {
        return pixel.blue >=
               strongChannel &&
               pixel.red <=
               weakChannel &&
               pixel.green <= weakChannel;
    }

    [[nodiscard]]
    bool
    has_blue_near_trail_probe( const grab::Image& image )
    {
        const auto center_x    = static_cast<std::int32_t>( trailProbeX );
        const auto half_width  = static_cast<std::int32_t>( trailProbeTolerance );
        const auto half_height = static_cast<std::int32_t>( trailWidth ) + 2;
        for( auto y = dragY - half_height; y <= dragY + half_height; ++y )
        {
            for( auto x = center_x - half_width; x <= center_x + half_width; ++x )
            {
                if( x < 0 || y < 0 )
                {
                    continue;
                }
                const auto pixel = pixel_at( image,
                                             static_cast<std::uint32_t>( x ),
                                             static_cast<std::uint32_t>( y ) );
                if( pixel.has_value() && is_blue( *pixel ) )
                {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]]
    bool
    reactor_barrier( grab::Session& session )
    {
        auto completion = std::make_shared<std::promise<void>>();
        auto future     = completion->get_future();
        auto posted     = session.post(
            [completion]
            {
                completion->set_value();
            }
        );
        return posted.has_value() &&
               future.wait_for( integrationDeadline ) == std::future_status::ready;
    }

    class TrailHarness final
    {
        public:

            TrailHarness( grab::Session&     session,
                          grab::Overlay&     overlay,
                          grab::Subscription subscription ) :
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
                    grab::kernel::presentation::TrailStyle{
                        .physical =
                            {
                                .r = opaqueChannel,
                                .g = 0U,
                                .b = 0U,
                                .a = opaqueChannel,
                            },
                        .injected =
                            {
                                .r = 0U,
                                .g = 0U,
                                .b = opaqueChannel,
                                .a = opaqueChannel,
                            },
                        .fade = std::chrono::duration_cast<std::chrono::milliseconds>(
                            trailFade
                        ),
                        .width_px = trailWidth,
                    }
                },
                session_{ &session },
                overlay_{ &overlay },
                subscription_{ std::move( subscription ) }
            {
                scene_.set_delta_sink(
                    [this]( const grab::overlay::SceneDelta& delta )
                    {
                        forward( delta );
                    }
                );
            }

            void
            install()
            {
                subscription_.set_notify(
                    [this]
                    {
                        schedule();
                    }
                );
            }

            [[nodiscard]]
            bool
            wait_for_midpoint()
            {
                std::unique_lock lock{ mutex_ };
                return changed_.wait_for( lock,
                                          integrationDeadline,
                                          [this]
                                          {
                                              return midpoint_covered_ ||
                                                     error_.has_value();
                                          } ) &&
                       midpoint_covered_ &&
                       !error_.has_value();
            }

            [[nodiscard]]
            bool
            stop()
            {
                subscription_.set_notify( {} );
                session_->stop_observation();
                return reactor_barrier( *session_ );
            }

            [[nodiscard]]
            std::optional<grab::Error>
            error() const
            {
                const std::scoped_lock lock{ mutex_ };
                return error_;
            }

        private:

            void
            schedule()
            {
                bool expected{};
                if( !scheduled_.compare_exchange_strong( expected, true ) )
                {
                    return;
                }
                auto posted = session_->post(
                    [this]
                    {
                        drain();
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
                while( auto item = subscription_.try_pop_item() )
                {
                    animator_.consume( *item );
                }
                scheduled_.store( false );
                if( auto raced = subscription_.try_pop_item() )
                {
                    animator_.consume( *raced );
                    schedule();
                }
            }

            void
            forward( const grab::overlay::SceneDelta& delta )
            {
                const auto* const upsert =
                    std::get_if<grab::overlay::Upsert>( &delta.change );
                if( upsert == nullptr )
                {
                    return;
                }
                auto added = overlay_->add( upsert->record.shape );
                if( !added.has_value() )
                {
                    remember_error( std::move( added.error() ) );
                    return;
                }
                const auto* const path =
                    std::get_if<grab::overlay::Path>( &upsert->record.shape.geometry );
                if( path == nullptr )
                {
                    return;
                }
                double minimum_x = std::numeric_limits<double>::max();
                double maximum_x = std::numeric_limits<double>::lowest();
                for( const auto& command : path->commands )
                {
                    const grab::SpacePoint* point{};
                    if( const auto* const move =
                            std::get_if<grab::overlay::MoveTo>( &command ) )
                    {
                        point = &move->point;
                    }
                    else if( const auto* const line =
                                 std::get_if<grab::overlay::LineTo>( &command ) )
                    {
                        point = &line->point;
                    }
                    if( point != nullptr )
                    {
                        minimum_x = std::min( minimum_x, point->x );
                        maximum_x = std::max( maximum_x, point->x );
                    }
                }
                if( minimum_x <=
                    trailProbeX +
                    trailProbeTolerance &&
                    maximum_x >=
                    trailProbeX -
                    trailProbeTolerance )
                {
                    {
                        const std::scoped_lock lock{ mutex_ };
                        midpoint_covered_ = true;
                    }
                    changed_.notify_all();
                }
            }

            void
            remember_error( grab::Error error )
            {
                {
                    const std::scoped_lock lock{ mutex_ };
                    if( !error_.has_value() )
                    {
                        error_ = std::move( error );
                    }
                }
                changed_.notify_all();
            }

            grab::kernel::presentation::OverlayScene  scene_;
            grab::kernel::presentation::TrailAnimator animator_;
            grab::Session*                            session_{};
            grab::Overlay*                            overlay_{};
            grab::Subscription                        subscription_;
            std::atomic_bool                          scheduled_;
            mutable std::mutex                        mutex_;
            std::condition_variable                   changed_;
            std::optional<grab::Error>                error_;
            bool                                      midpoint_covered_{};
    };

    [[nodiscard]]
    bool
    input_region_is_empty( DisplayConnection& display,
                           xcb_window_t       window )
    {
        xcb_generic_error_t* raw_error{};
        const auto           reply = take_xcb_owned( xcb_shape_get_rectangles_reply(
            display.get(),
            xcb_shape_get_rectangles( display.get(), window, XCB_SHAPE_SK_INPUT ),
            &raw_error
        ) );
        const auto           error = take_xcb_owned( raw_error );
        return error ==
               nullptr &&
               reply !=
               nullptr &&
               xcb_shape_get_rectangles_rectangles_length( reply.get() ) == 0;
    }

    struct WindowGeometry
    {
            std::uint16_t width{};
            std::uint16_t height{};
    };

    [[nodiscard]]
    std::optional<WindowGeometry>
    window_geometry( DisplayConnection& display,
                     xcb_window_t       window )
    {
        const auto reply = take_xcb_owned(
            xcb_get_geometry_reply( display.get(),
                                    xcb_get_geometry( display.get(), window ),
                                    nullptr )
        );
        if( reply == nullptr )
        {
            return std::nullopt;
        }
        return WindowGeometry{ .width = reply->width, .height = reply->height };
    }

    [[nodiscard]]
    bool
    request_randr_resize( DisplayConnection& display,
                          std::uint16_t      width,
                          std::uint16_t      height )
    {
        const auto* const screen = display.screen();
        if( screen ==
            nullptr ||
            screen->width_in_pixels ==
            0U ||
            screen->height_in_pixels == 0U )
        {
            return false;
        }
        const auto width_mm = std::max<std::uint32_t>(
            1U,
            ( static_cast<std::uint32_t>( screen->width_in_millimeters ) * width ) /
                screen->width_in_pixels
        );
        const auto height_mm = std::max<std::uint32_t>(
            1U,
            ( static_cast<std::uint32_t>( screen->height_in_millimeters ) * height ) /
                screen->height_in_pixels
        );
        const auto checked = xcb_randr_set_screen_size_checked( display.get(),
                                                                display.root(),
                                                                width,
                                                                height,
                                                                width_mm,
                                                                height_mm );
        return request_succeeded( display.get(), checked ) &&
               xcb_flush( display.get() ) > xcbSuccess;
    }

    [[nodiscard]]
    bool
    simulate_topology_on_reactor( grab::Session&      session,
                                  X11OverlayDelegate& delegate,
                                  std::uint16_t       width,
                                  std::uint16_t       height )
    {
        auto completion = std::make_shared<std::promise<void>>();
        auto future     = completion->get_future();
        auto posted     = session.post(
            [&delegate, width, height, completion]
            {
                x11_detail::X11OverlayDelegateTestAccess::simulate_topology_change(
                    delegate,
                    width,
                    height
                );
                completion->set_value();
            }
        );
        return posted.has_value() &&
               future.wait_for( integrationDeadline ) == std::future_status::ready;
    }

    [[nodiscard]]
    grab::Locator
    sentinel_locator()
    {
        return grab::sel::all( {
            grab::sel::role( grab::role::window ),
            grab::sel::property( grab::property::title, std::string{ sentinelTitle } ),
            grab::sel::property( grab::property::window_class,
                                 std::string{ sentinelClass } ),
        } );
    }

    TEST( OverlayRing3,
          BareXvfbRejectsOverlayWithoutMappingAWindow )
    {
        DisplayConnection display{ bareDisplay };
        if( !display.valid() )
        {
            GTEST_SKIP() << "requires the Xvfb fixture on " << bareDisplay;
        }
        ASSERT_EQ( compositor_owner( display ), XCB_WINDOW_NONE );
        const auto                              before = root_children( display );

        // Record every MapNotify on the root for the WHOLE attempt window —
        // a before/after child comparison cannot exclude transient mapping.
        constexpr std::array<std::uint32_t, 1U> substructureMask{
            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
        };
        xcb_change_window_attributes( display.get(),
                                      display.root(),
                                      XCB_CW_EVENT_MASK,
                                      substructureMask.data() );
        xcb_flush( display.get() );

        // Public resolution path: the session's overlay capability must be
        // unavailable on a compositor-less display.
        grab::SessionOptions options;
        options.display = std::string{ bareDisplay };
        auto session    = grab::Session::open( std::move( options ) );
        ASSERT_TRUE( session.has_value() ) << session.error().message;
        const auto facade = ( *session )->overlay();
        ASSERT_FALSE( facade.has_value() );
        EXPECT_EQ( facade.error().code, grab::ErrorCode::CapabilityUnavailable );
        ( *session )->close();

        // Direct-delegate probe keeps the distinct-reason assertion.
        auto delegate = X11OverlayDelegate::create( nullptr, bareDisplay );
        ASSERT_TRUE( delegate.has_value() ) << delegate.error().message;
        const auto opened = ( *delegate )->open( grab::CoordinateSpaceId{ 1U } );

        ASSERT_FALSE( opened.has_value() );
        EXPECT_EQ( opened.error().code, grab::ErrorCode::CapabilityUnavailable );
        EXPECT_TRUE( opened.error().message.contains( "compositing manager" ) )
            << opened.error().message;
        EXPECT_EQ( x11_detail::X11OverlayDelegateTestAccess::window( **delegate ),
                   XCB_WINDOW_NONE );
        ( *delegate )->close();

        // No MapNotify may have arrived at any point during the attempts.
        xcb_flush( display.get() );
        std::size_t map_notifications = 0U;
        while( auto* event = xcb_poll_for_event( display.get() ) )
        {
            const auto response = event->response_type & syntheticEventMask;
            if( response == XCB_MAP_NOTIFY )
            {
                ++map_notifications;
            }
            std::free( event );    // NOLINT(cppcoreguidelines-no-malloc,hicpp-no-malloc)
        }
        EXPECT_EQ( map_notifications, std::size_t{ 0U } );
        EXPECT_EQ( root_children( display ), before );
    }

    // GoogleTest assertion macros dominate the reported cognitive complexity.
    // NOLINTBEGIN(readability-function-cognitive-complexity)
    TEST( OverlayRing3,
          PublicOverlayTrailAndInputComposeWithClickThrough )
    {
        if( !compositor_is_installed() )
        {
            GTEST_SKIP() << compositorDependencyReason;
        }
        DisplayConnection display{ compositedDisplay };
        ASSERT_TRUE( display.valid() )
            << "requires the Xvfb fixture on " << compositedDisplay;
        ASSERT_NE( compositor_owner( display ), XCB_WINDOW_NONE )
            << "compositor fixture did not own " << compositorSelection;
        SentinelWindow sentinel;
        ASSERT_TRUE( create_sentinel( display, sentinel ) );

        ScopedDisplay environment{ compositedDisplay };
        ASSERT_TRUE( environment.active() );
        auto session = grab::Session::open();
        ASSERT_TRUE( session.has_value() ) << session.error().message;
        auto overlay = ( *session )->overlay();
        ASSERT_TRUE( overlay.has_value() ) << overlay.error().message;

        auto runtime_route = grab::drivers::desktop::x11::X11CaptureRoute::open(
            std::string{ compositedDisplay }.c_str()
        );
        ASSERT_TRUE( runtime_route.has_value() ) << runtime_route.error().message;
        const auto global_space = runtime_route->global_space();
        auto       rectangle    = ( *overlay )->add( green_rectangle( global_space ) );
        ASSERT_TRUE( rectangle.has_value() ) << rectangle.error().message;

        grab::SubscriptionScope scope;
        scope.kinds = { grab::EventKind::MouseMove };
        auto subscription =
            ( *session )
                ->watch( std::move( scope ),
                         grab::QueueOptions{
                             .capacity = dragQueueCapacity,
                             .overflow = grab::QueueOverflowPolicy::NeverDrop,
                         } );
        ASSERT_TRUE( subscription.has_value() ) << subscription.error().message;
        TrailHarness trail{ **session, **overlay, std::move( *subscription ) };
        trail.install();
        ASSERT_TRUE( ( *session )->start_observation().has_value() );

        const auto match = ( *session )->resolve( sentinel_locator() );
        ASSERT_TRUE( match.has_value() ) << match.error().message;
        const grab::geometry::Point drag_start{
            .x = dragStartX,
            .y = dragY,
        };
        const grab::geometry::Point drag_end{
            .x = dragEndX,
            .y = dragY,
        };
        const grab::input::DragOptions drag_options{
            .interpolation_steps = dragInterpolationSteps,
            .step_dwell          = std::chrono::milliseconds::zero(),
            .path                = grab::input::DragOptions::Path::Linear,
        };
        const grab::Drag drag{
            .target  = *match,
            .from    = drag_start,
            .to      = drag_end,
            .options = drag_options,
        };
        const grab::ActionOptions action_options{
            .deadline    = integrationDeadline,
            .cardinality = grab::Cardinality::ExactlyOne,
            .routing     = grab::RoutePolicy::PhysicalOnly,
            .retry       = grab::RetryClass::ResolveOnly,
            .force       = true,
            .stop        = {},
        };
        const auto receipt = ( *session )->perform( drag, action_options );
        ASSERT_TRUE( receipt.has_value() ) << receipt.error().message;
        EXPECT_NE( receipt->commit, grab::CommitStatus::FailedBeforeCommit );
        ASSERT_TRUE( trail.wait_for_midpoint() );
        ASSERT_TRUE( trail.stop() );
        ASSERT_FALSE( trail.error().has_value() );
        ASSERT_TRUE( ( *overlay )->flush().has_value() );

        auto screen = grab::Screen::open( std::string{ compositedDisplay }.c_str() );
        ASSERT_TRUE( screen.has_value() ) << screen.error().message;
        const auto image = screen->display();
        ASSERT_TRUE( image.has_value() ) << image.error().message;
        const auto annotation = pixel_at( *image, annotationProbeX, annotationProbeY );
        ASSERT_TRUE( annotation.has_value() );
        EXPECT_TRUE( is_green( *annotation ) );
        EXPECT_TRUE( has_blue_near_trail_probe( *image ) );

        const auto buttons = wait_for_drag_buttons( display, sentinel.window );
        EXPECT_TRUE( buttons.press );
        EXPECT_TRUE( buttons.release );
    }

    TEST( OverlayRing3,
          RandrRebuildRetainsSceneAndEmptyInputRegion )
    {
        if( !compositor_is_installed() )
        {
            GTEST_SKIP() << compositorDependencyReason;
        }
        DisplayConnection display{ randrDisplay };
        ASSERT_TRUE( display.valid() )
            << "requires the Xvfb fixture on " << randrDisplay;
        ASSERT_NE( compositor_owner( display ), XCB_WINDOW_NONE )
            << "compositor fixture did not own " << compositorSelection;

        ScopedDisplay environment{ randrDisplay };
        ASSERT_TRUE( environment.active() );
        auto runtime = std::make_unique<grab::drivers::desktop::x11::X11Runtime>();
        auto* const raw_runtime = runtime.get();
        auto        session = grab::Session::open_owning_runtime( std::move( runtime ) );
        ASSERT_TRUE( session.has_value() ) << session.error().message;
        auto overlay = ( *session )->overlay();
        ASSERT_TRUE( overlay.has_value() ) << overlay.error().message;
        auto* const route = raw_runtime->capture_route();
        ASSERT_NE( route, nullptr );
        const auto global_space = route->global_space();
        ASSERT_TRUE( ( *overlay )->add( green_rectangle( global_space ) ).has_value() );
        ASSERT_TRUE( ( *overlay )->flush().has_value() );

        auto* const delegate =
            dynamic_cast<X11OverlayDelegate*>( raw_runtime->overlay_delegate() );
        ASSERT_NE( delegate, nullptr );
        const auto old_window =
            x11_detail::X11OverlayDelegateTestAccess::window( *delegate );
        ASSERT_NE( old_window, XCB_WINDOW_NONE );
        ASSERT_TRUE( input_region_is_empty( display, old_window ) );

        const bool resized =
            request_randr_resize( display, resizedWidth, resizedHeight );
        ASSERT_TRUE( ( *overlay )->flush().has_value() );
        auto new_window = x11_detail::X11OverlayDelegateTestAccess::window( *delegate );
        if( !resized || new_window == old_window )
        {
            ASSERT_TRUE( simulate_topology_on_reactor( **session,
                                                       *delegate,
                                                       resizedWidth,
                                                       resizedHeight ) );
            ASSERT_TRUE( ( *overlay )->flush().has_value() );
            new_window = x11_detail::X11OverlayDelegateTestAccess::window( *delegate );
        }

        ASSERT_NE( new_window, XCB_WINDOW_NONE );
        EXPECT_NE( new_window, old_window );
        const auto geometry = window_geometry( display, new_window );
        ASSERT_TRUE( geometry.has_value() );
        EXPECT_EQ( geometry->width, resizedWidth );
        EXPECT_EQ( geometry->height, resizedHeight );
        EXPECT_TRUE( input_region_is_empty( display, new_window ) );

        auto screen = grab::Screen::open( std::string{ randrDisplay }.c_str() );
        ASSERT_TRUE( screen.has_value() ) << screen.error().message;
        const auto image = screen->display();
        ASSERT_TRUE( image.has_value() ) << image.error().message;
        const auto retained = pixel_at( *image, annotationProbeX, annotationProbeY );
        ASSERT_TRUE( retained.has_value() );
        EXPECT_TRUE( is_green( *retained ) );
    }

    // NOLINTEND(readability-function-cognitive-complexity)

}    // namespace
