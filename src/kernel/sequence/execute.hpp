#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The enter/tick/exit triple every command runs through, ported in shape from
// bead::Node.
//
// THE SEAT IS A TEMPLATE PARAMETER, NOT A grab::Input*. grab::Input is a pimpl
// whose only construction path is open() against a live X display, so a
// context holding one would make every display-free scheduler test
// unwritable — and the display-free tests are the whole point of a
// caller-driven pump. Anything exposing move_pointer_absolute, button and
// flush qualifies, which includes tests/support/recording_seat.hpp.
//
// exit() ALWAYS runs, including on interrupt, and is what releases a held
// button or key. The caller owns what it presses: a button left down survives
// the process and reaches the next application.
//
// PHASE 0: the contract, with stub bodies. The execution unit fills them in.

#include "grab/command.hpp"
#include "grab/sequence_types.hpp"

#include <chrono>
#include <cstddef>

namespace grab::kernel::scheduling
{

    class TimerThread;

}

namespace grab::kernel::sequence
{

    template<typename SeatT>
    struct ExecContext
    {
            SeatT*                                 seat{ nullptr };
            grab::kernel::scheduling::TimerThread* timers{ nullptr };
            std::chrono::steady_clock::time_point  now{};
    };

    // Per-step scratch, owned by whoever is running the step. It is separate
    // from the Command because the Command belongs to the immutable document
    // and a run must not write into it.
    struct CommandState
    {
            std::chrono::steady_clock::time_point started{};
            std::chrono::steady_clock::time_point deadline{};
            // How many waypoints (or characters, or notches) have been emitted
            // so far; a multi-tick command resumes from here.
            std::size_t                           emitted{ 0U };
            bool                                  entered{ false };
            // A button or key is down and exit() must release it.
            bool                                  held{ false };
    };

    // Where a command's duration comes from, and whether its body has to run
    // off the timing thread. Both read the descriptor table rather than
    // carrying a second copy of the policy.
    [[nodiscard]]
    grab::sequence::TimingClass
    timing_class_of( const grab::sequence::Command& command ) noexcept;

    [[nodiscard]]
    bool
    is_blocking( const grab::sequence::Command& command ) noexcept;

    template<typename SeatT>
    [[nodiscard]]
    grab::sequence::Status
    enter( const grab::sequence::Command& command,
           CommandState&                  state,
           ExecContext<SeatT>&            context )
    {
        ( void )command;
        ( void )state;
        ( void )context;
        return grab::sequence::Status::Failure;
    }

    template<typename SeatT>
    [[nodiscard]]
    grab::sequence::Status
    tick( const grab::sequence::Command&        command,
          CommandState&                         state,
          ExecContext<SeatT>&                   context,
          std::chrono::steady_clock::time_point now )
    {
        ( void )command;
        ( void )state;
        ( void )context;
        ( void )now;
        return grab::sequence::Status::Failure;
    }

    template<typename SeatT>
    void
    exit( const grab::sequence::Command& command,
          CommandState&                  state,
          ExecContext<SeatT>&            context )
    {
        ( void )command;
        ( void )state;
        ( void )context;
    }

}    // namespace grab::kernel::sequence
