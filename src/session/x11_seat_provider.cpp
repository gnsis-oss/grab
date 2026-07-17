#include "core/environment.hpp"
#include "drivers/desktop/x11/xcb_connection.hpp"
#include "drivers/desktop/x11/xi_seat.hpp"
#include "grab/capability.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "kernel/routing/provider.hpp"
#include "session/provider.hpp"
#include "session/x11_seat_provider.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace grab::session
{
    namespace
    {

        constexpr int              x11_seat_quality = 50;
        constexpr std::string_view provider_name    = "x11-seat";
        constexpr std::string_view seat_name_prefix = "grab-session-";
        constexpr std::string_view endpoint_prefix  = "x11-seat:";

    }    // namespace

    struct X11SeatSessionProvider::ActiveSeat
    {
            std::unique_ptr<grab::platform::x11::XcbConnection> conn;
            std::unique_ptr<grab::platform::x11::XiSeat>        seat;
            std::string                                         endpoint;
    };

    X11SeatSessionProvider::X11SeatSessionProvider() :
        provider_info{
            .name         = std::string{ provider_name },
            .capabilities = {},
            .quality      = x11_seat_quality,
        }
    {
    }

    X11SeatSessionProvider::~X11SeatSessionProvider() = default;

    const grab::core::ProviderInfo&
    X11SeatSessionProvider::info() const noexcept
    {
        return provider_info;
    }

    grab::Availability
    X11SeatSessionProvider::probe( const grab::core::Environment& env,
                                   grab::WorkspaceMode            mode ) const
    {
        if( env.session != grab::core::SessionType::X11 )
        {
            return grab::Availability{
                .state   = grab::AvailabilityState::Unavailable,
                .reason  = "not an X11 session",
                .quality = 0,
            };
        }
        if( mode == grab::WorkspaceMode::Offscreen )
        {
            return grab::Availability{
                .state   = grab::AvailabilityState::Unavailable,
                .reason  = "X11 has no offscreen isolation; use --mode shared or "
                           "the Wayland provider",
                .quality = 0,
            };
        }
        return grab::Availability{
            .state   = grab::AvailabilityState::Available,
            .reason  = "",
            .quality = x11_seat_quality,
        };
    }

    grab::Result<SessionRuntime>
    X11SeatSessionProvider::create( const WorkspaceDesc& desc ) const
    {
        auto connection = grab::platform::x11::XcbConnection::open( "" );
        if( !connection.has_value() )
        {
            return grab::fail( grab::ErrorCode::ProviderFailed,
                               "X11 seat: display unavailable" );
        }
        auto conn = std::make_unique<grab::platform::x11::XcbConnection>(
            std::move( connection.value() )
        );

        const std::string seat_name = std::string{ seat_name_prefix } + desc.name;
        auto              seat = grab::platform::x11::XiSeat::create( *conn, seat_name );
        if( !seat.has_value() )
        {
            return grab::fail( seat.error().code, seat.error().message );
        }
        auto owned_seat =
            std::make_unique<grab::platform::x11::XiSeat>( std::move( seat.value() ) );

        const std::string endpoint =
            std::string{ endpoint_prefix } + std::to_string( owned_seat->pointer_id() );

        const std::scoped_lock guard( mutex );
        active.push_back( std::make_unique<ActiveSeat>( std::move( conn ),
                                                        std::move( owned_seat ),
                                                        endpoint ) );

        return SessionRuntime{
            .endpoint       = endpoint,
            .control_socket = {},
            .supervisor_pid = grab::Pid{},
        };
    }

    grab::Result<void>
    X11SeatSessionProvider::destroy( const SessionRuntime& runtime ) const
    {
        const std::scoped_lock guard( mutex );
        const auto             match =
            std::ranges::find_if( active,
                                  [&runtime]( const std::unique_ptr<ActiveSeat>& entry )
                                  {
                                      return entry->endpoint == runtime.endpoint;
                                  } );
        if( match == active.end() )
        {
            return grab::fail( grab::ErrorCode::SessionNotFound,
                               "X11 seat: unknown session runtime" );
        }
        active.erase( match );    // ~XiSeat removes the master device pair
        return {};
    }

}    // namespace grab::session
