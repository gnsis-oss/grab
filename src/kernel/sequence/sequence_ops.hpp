#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Container operations over a Sequence: inject, estimate, validate.
//
// "Revert" is deliberately absent: it is dropping the Player and building a
// new one, which resets the RUN and not the WORLD. grab cannot un-click. A
// sequence that typed into a form and is then reverted replays from the top
// against an application that already holds the text. Real undo needs
// per-command compensation, which RetryClass::Compensated gestures at and
// nothing implements.
//
// optimise() is not here on purpose. Folding warp+click into click_at removes
// a motion event the target application may depend on — hover states,
// tooltips, :hover CSS — so an optimiser that runs by default silently breaks
// the sequences it speeds up.
//
// PHASE 0: declaration only. The sequence-ops unit implements it.

#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"

#include <chrono>
#include <cstddef>

namespace grab::kernel::sequence
{

    // Injected steps take fresh indices ABOVE the host's high-water mark
    // rather than being interleaved, so no live index is reused and every
    // existing StepId stays valid. Because no index is ever reused, the
    // generation half stays at 1 and splice() never bumps it. Labels colliding
    // between the two documents are an error, for the same reason duplicate
    // labels are a load error.
    [[nodiscard]]
    grab::Result<Sequence>
    splice( const Sequence&        host,
            grab::sequence::StepId at,
            const Sequence&        insert );

    // The critical path over declared durations PLUS grace. Steps with no
    // declared duration contribute zero and are counted separately by
    // unestimated_steps(), so a caller reports ">= 6.2 s, 3 steps unestimated"
    // rather than presenting a plan as more precise than it is.
    [[nodiscard]]
    std::chrono::nanoseconds
    planned( const Sequence& program );

    [[nodiscard]]
    std::size_t
    unestimated_steps( const Sequence& program );

    // A real seam that is pass-through BY DECISION: it returns success for
    // every input. Resource and policy rules — two pointer-claiming steps in
    // one parallel branch, for instance — land here later. Until then a
    // document can express interleaved garbage input and will be run as
    // written.
    [[nodiscard]]
    grab::Result<void>
    validate( const Sequence& program );

}    // namespace grab::kernel::sequence
