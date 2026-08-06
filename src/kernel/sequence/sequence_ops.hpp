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
    //
    // WHERE THE INJECTION LANDS: `insert` is spliced INTO the flow at `at`,
    // not merely alongside it. Every root of `insert` gains `at` as a
    // predecessor, and every host step that depended on `at` is rewired onto
    // `insert`'s sinks instead. So A -> B with a one-step X spliced at A
    // becomes A -> X -> B, never A -> {X, B}. The parallel reading would put
    // the injected input concurrently with the input it was injected before,
    // and interleaved synthetic input is exactly the hazard validate() has no
    // rule for yet.
    //
    // The HOST's pacing and name survive; `insert`'s are dropped. The pacing
    // mode is the sole authority over one document, and a splice produces one
    // document.
    [[nodiscard]]
    grab::Result<Sequence>
    splice( const Sequence&        host,
            grab::sequence::StepId at,
            const Sequence&        insert );

    // The critical path over declared durations PLUS grace. Steps with no
    // declared duration contribute zero and are counted separately by
    // unestimated_steps(), so a caller reports ">= 6.2 s, 3 steps unestimated"
    // rather than presenting a plan as more precise than it is.
    //
    // The only duration a document DECLARES is time.wait's `ms`; every other
    // op's duration is unknown until measured, per the governing rule that no
    // duration defaults to zero.
    [[nodiscard]]
    std::chrono::nanoseconds
    planned( const Sequence& program );

    // The same estimate under pacing the caller supplies rather than the
    // document's own. `grab play --pacing`/`--grace-ms` override the document,
    // and printing the document's figure under an overridden mode would report
    // a plan the run will not follow.
    [[nodiscard]]
    std::chrono::nanoseconds
    planned( const Sequence&               program,
             grab::sequence::PacingOptions pacing );

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
