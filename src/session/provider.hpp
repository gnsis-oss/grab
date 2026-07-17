#pragma once

#include "core/environment.hpp"
#include "grab/pid.hpp"
#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "kernel/routing/provider.hpp"

#include <string>

namespace grab::session
{

    struct SessionRuntime
    {
            std::string endpoint;
            std::string control_socket;
            grab::Pid   supervisor_pid;
    };

    class SessionProvider
    {
        public:

            SessionProvider()                         = default;
            SessionProvider( const SessionProvider& ) = delete;
            SessionProvider&
            operator=( const SessionProvider& )  = delete;
            SessionProvider( SessionProvider&& ) = delete;
            SessionProvider&
            operator=( SessionProvider&& ) = delete;
            virtual ~SessionProvider()     = default;

            [[nodiscard]]
            virtual const grab::core::ProviderInfo&
            info() const noexcept = 0;

            [[nodiscard]]
            virtual grab::Availability
            probe( const grab::core::Environment& env,
                   grab::WorkspaceMode            mode ) const = 0;

            [[nodiscard]]
            virtual grab::Result<SessionRuntime>
            create( const WorkspaceDesc& desc ) const = 0;

            [[nodiscard]]
            virtual grab::Result<void>
            destroy( const SessionRuntime& runtime ) const = 0;
    };

}    // namespace grab::session
