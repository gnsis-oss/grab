#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "eventgrab/v1/events.pb.h"
#include "grab/event.hpp"
#include "grab/result.hpp"

namespace grab::compat::eventgrab_v1
{

    // True for the eventgrab.v1 legacy-vocabulary event kinds whose payload is carried
    // in the freeform proto `data` map (input/window/accessibility/integration). False
    // for the current-model kinds (graph_change nodes/relations, state_snapshot) and
    // Unspecified.
    [[nodiscard]]
    bool
    is_v1_payload_kind( grab::EventKind kind ) noexcept;

    // Encode a legacy-vocabulary payload into the wire `data` map. Precondition:
    // is_v1_payload_kind(kind). `payload` must hold the variant alternative matching
    // `kind`.
    void
    encode_v1_payload( eventgrab::v1::Event& wire,
                       grab::EventKind       kind,
                       const grab::Payload&  payload );

    // Decode a legacy-vocabulary payload from the wire `data` map. Precondition:
    // is_v1_payload_kind(kind). Returns a grab::Payload (the matching variant
    // alternative) or a ProtocolError (message prefixed "malformed event: ") on a
    // missing/unparseable field.
    [[nodiscard]]
    grab::Result<grab::Payload>
    decode_v1_payload( const eventgrab::v1::Event& wire,
                       grab::EventKind             kind );

}    // namespace grab::compat::eventgrab_v1
