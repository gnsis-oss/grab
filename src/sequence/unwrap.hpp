#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// In-repo seam between the public facade and the kernel document. The CLI
// loads through the public loader — one loader, one set of diagnostics — and
// still reaches the kernel Sequence for the planning and tracing surfaces
// the facade deliberately hides. Internal on purpose: an out-of-repo
// consumer gets the facade and nothing else.

#include "grab/sequence.hpp"
#include "kernel/sequence/sequence.hpp"

namespace grab::sequence::detail
{

    [[nodiscard]]
    const grab::kernel::sequence::Sequence&
    unwrap( const Sequence& sequence ) noexcept;

    [[nodiscard]]
    Sequence
    wrap( grab::kernel::sequence::Sequence program );

}    // namespace grab::sequence::detail
