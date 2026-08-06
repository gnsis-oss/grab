// PLACEHOLDER — registered in tests/CMakeLists.txt by Phase 0 so the execution
// unit can replace this file without touching a shared build file.
//
// It also proves the two seams the execution unit depends on actually compile
// together: ExecContext is templated on a seat, and RecordingSeat satisfies it
// with no X connection.

#include "grab/command.hpp"
#include "kernel/sequence/execute.hpp"
#include "support/recording_seat.hpp"

// clang-format off
#include <gtest/gtest.h>
// clang-format on

namespace
{

    TEST( Placeholder,
          ExecuteCompiles )
    {
        grab::testing::RecordingSeat                                      seat;
        grab::kernel::sequence::ExecContext<grab::testing::RecordingSeat> context{
            .seat = &seat,
        };
        ASSERT_NE( context.seat, nullptr );
        EXPECT_TRUE( context.seat->events().empty() );

        const grab::sequence::Command command{ grab::sequence::WaitCommand{} };
        EXPECT_EQ( grab::kernel::sequence::timing_class_of( command ),
                   grab::sequence::TimingClass::Timed );
        EXPECT_FALSE( grab::kernel::sequence::is_blocking( command ) );
    }

}    // namespace
