#pragma once

#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "kernel/routing/provider.hpp"
#include "kernel/support/environment.hpp"
#include "session/provider.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace grab::session
{

    // A shared-seat session provider backed by an XInput2 master device pair on
    // the current X server. Cooperative isolation only (spec section 2): the
    // human's seat keeps priority by convention, not enforcement.
    class X11SeatSessionProvider final : public SessionProvider
    {
        public:

            X11SeatSessionProvider();
            ~X11SeatSessionProvider() override;

            X11SeatSessionProvider( const X11SeatSessionProvider& ) = delete;
            X11SeatSessionProvider&
            operator=( const X11SeatSessionProvider& )         = delete;
            X11SeatSessionProvider( X11SeatSessionProvider&& ) = delete;
            X11SeatSessionProvider&
            operator=( X11SeatSessionProvider&& ) = delete;

            [[nodiscard]]
            const grab::core::ProviderInfo&
            info() const noexcept override;

            [[nodiscard]]
            grab::Availability
            probe( const grab::core::Environment& env,
                   grab::WorkspaceMode            mode ) const override;

            [[nodiscard]]
            grab::Result<SessionRuntime>
            create( const WorkspaceDescriptor& descriptor ) const override;

            [[nodiscard]]
            grab::Result<void>
            destroy( const SessionRuntime& runtime ) const override;

        private:

            struct ActiveSeat;

            grab::core::ProviderInfo                         provider_info;
            mutable std::mutex                               mutex;
            mutable std::vector<std::unique_ptr<ActiveSeat>> active;
    };

}    // namespace grab::session
