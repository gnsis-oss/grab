#include "grab/ids.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/player.hpp"
#include "kernel/sequence/sequence.hpp"

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

namespace grab::kernel::sequence
{

    // PHASE 0 STUB. The player unit replaces this file.
    Player::Player( const Sequence& program ) :
        program_( &program ),
        status_( program.steps().size(),
                 grab::sequence::StepStatus::Pending )
    {
    }

    grab::Result<void>
    Player::play()
    {
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence play is not implemented yet" );
    }

    grab::Result<void>
    Player::pause()
    {
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence pause is not implemented yet" );
    }

    grab::Result<void>
    Player::interrupt()
    {
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence interrupt is not implemented yet" );
    }

    grab::Result<void>
    Player::skip()
    {
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence skip is not implemented yet" );
    }

    grab::Result<void>
    Player::goto_step( grab::sequence::StepId target )
    {
        ( void )target;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence goto_step is not implemented yet" );
    }

    grab::Result<void>
    Player::goto_label( std::string_view label )
    {
        ( void )label;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence goto_label is not implemented yet" );
    }

    grab::Result<void>
    Player::pump( std::chrono::steady_clock::time_point now )
    {
        ( void )now;
        return grab::fail( grab::ErrorCode::CapabilityUnavailable,
                           "sequence pump is not implemented yet" );
    }

    grab::sequence::PlayState
    Player::state() const noexcept
    {
        return state_;
    }

    grab::sequence::StepStatus
    Player::status_of( grab::sequence::StepId id ) const
    {
        const auto index = static_cast<std::size_t>( id.index() );
        if( id.is_nil() || index >= status_.size() )
        {
            return grab::sequence::StepStatus::Pending;
        }
        return status_[index];
    }

    std::span<const grab::sequence::StepId>
    Player::frontier() const noexcept
    {
        return std::span<const grab::sequence::StepId>{ frontier_ };
    }

    grab::Uuid
    Player::run_id() const noexcept
    {
        return run_id_;
    }

    const Sequence*
    Player::program() const noexcept
    {
        return program_;
    }

}    // namespace grab::kernel::sequence
