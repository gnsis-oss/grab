#pragma once

#include "grab/result.hpp"
#include "grab/workspace.hpp"
#include "session/provider.hpp"
#include "session/record.hpp"
#include "session/registry.hpp"

#include <string_view>
#include <vector>

namespace grab::session
{

    class SessionManager
    {
        public:

            SessionManager( SessionRegistry&       registry,
                            const SessionProvider& provider ) noexcept;

            [[nodiscard]]
            grab::Result<SessionRecord>
            start( const WorkspaceDesc& desc );

            [[nodiscard]]
            grab::Result<void>
            stop( std::string_view name );

            [[nodiscard]]
            grab::Result<SessionRecord>
            get( std::string_view name );

            [[nodiscard]]
            std::vector<SessionRecord>
            list();

        private:

            SessionRegistry&       registry;
            const SessionProvider& provider;
    };

}    // namespace grab::session
