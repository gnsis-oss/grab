#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/execute.hpp"

namespace grab::kernel::sequence
{

    grab::sequence::TimingClass
    timing_class_of( const grab::sequence::Command& command ) noexcept
    {
        return grab::timing_class_of( grab::sequence::kind_of( command ) );
    }

    bool
    is_blocking( const grab::sequence::Command& command ) noexcept
    {
        return grab::is_blocking_command( grab::sequence::kind_of( command ) );
    }

}    // namespace grab::kernel::sequence
