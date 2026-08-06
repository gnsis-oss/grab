#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/sequence/sequence_ops.hpp"

#include <chrono>
#include <cstddef>

namespace grab::kernel::sequence
{

    // PHASE 0 STUB. The sequence-ops unit replaces this file.
    grab::Result<Sequence>
    splice( const Sequence&        host,
            grab::sequence::StepId at,
            const Sequence&        insert )
    {
        ( void )host;
        ( void )at;
        ( void )insert;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence splice is not implemented yet" );
    }

    std::chrono::nanoseconds
    planned( const Sequence& program )
    {
        ( void )program;
        return std::chrono::nanoseconds::zero();
    }

    std::size_t
    unestimated_steps( const Sequence& program )
    {
        ( void )program;
        return 0U;
    }

    grab::Result<void>
    validate( const Sequence& program )
    {
        // Pass-through by decision, not by omission.
        ( void )program;
        return {};
    }

}    // namespace grab::kernel::sequence
