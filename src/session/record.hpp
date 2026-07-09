#pragma once

#include "grab/result.hpp"
#include "grab/session.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace grab::session
{

    struct SessionRecord
    {
            std::string     name;
            std::string     provider;
            std::string     endpoint;    // display / wayland socket
            std::string     control_socket;
            SessionMode     mode = SessionMode::offscreen;
            SessionGeometry geometry;
            SessionState    state             = SessionState::starting;
            std::int64_t    supervisor_pid    = 0;
            std::uint64_t   created_monotonic = 0U;
    };

    [[nodiscard]]
    std::string
    to_json( const SessionRecord& record );

    [[nodiscard]]
    grab::Result<SessionRecord>
    parse_record( std::string_view text );

}    // namespace grab::session
