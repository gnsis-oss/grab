#ifndef GRAB_TRANSPORT_CODEC_HPP
#define GRAB_TRANSPORT_CODEC_HPP

#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/result.hpp"

namespace grab::transport
{

    inline constexpr int kMaxDataEntries = 32;
    inline constexpr int kMaxValueBytes  = 65'536;

    // Event::sequence is process-local ingress metadata. The proto Event has no
    // sequence field, so to_wire ignores it and from_wire sets sequence to zero.
    [[nodiscard]]
    grab::Result<eventgrab::v1::Event>
    to_wire( const grab::Event& event );

    [[nodiscard]]
    grab::Result<grab::Event>
    from_wire( const eventgrab::v1::Event& wire );

}    // namespace grab::transport

#endif    // GRAB_TRANSPORT_CODEC_HPP
