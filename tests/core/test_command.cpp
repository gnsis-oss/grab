// Contract tests for the closed command variant and the Step that carries it.
//
// Two things are pinned here. First, that every one of the 23 alternatives
// reports the CommandKind it actually is — kind_of() is how the player routes,
// so a mislabelled alternative would silently execute the wrong body. Second,
// that Step's defaults are the safe ones: Abort, no grace, no identity.

#include "grab/command.hpp"
#include "grab/command_descriptor.hpp"
#include "grab/pointer_button.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/sequence/execute.hpp"
#include "support/recording_seat.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
// clang-format on

namespace
{

    using grab::sequence::CaptureCommand;
    using grab::sequence::ClickAtCommand;
    using grab::sequence::ClickCommand;
    using grab::sequence::Command;
    using grab::sequence::DragCommand;
    using grab::sequence::ErrorPolicy;
    using grab::sequence::FollowCommand;
    using grab::sequence::KeyCommand;
    using grab::sequence::KeyDownCommand;
    using grab::sequence::KeyUpCommand;
    using grab::sequence::MoveCommand;
    using grab::sequence::OverlayAddCommand;
    using grab::sequence::OverlayAttachCommand;
    using grab::sequence::OverlayClearCommand;
    using grab::sequence::OverlayDetachCommand;
    using grab::sequence::OverlayGrabCommand;
    using grab::sequence::OverlayReleaseCommand;
    using grab::sequence::OverlayRemoveCommand;
    using grab::sequence::OverlayUpdateCommand;
    using grab::sequence::PressCommand;
    using grab::sequence::ReleaseCommand;
    using grab::sequence::ScrollCommand;
    using grab::sequence::Step;
    using grab::sequence::TypeCommand;
    using grab::sequence::WaitCommand;
    using grab::sequence::WarpCommand;

    constexpr std::size_t expectedAlternativeCount = 23U;

    // The kind each variant alternative must report, in variant order. This is
    // the table kind_of() is checked against; writing it out separately is the
    // whole point, since reading commandKind back off the same struct would
    // assert nothing.
    constexpr auto        expectedKinds = std::to_array( {
        grab::CommandKind::Type,
        grab::CommandKind::Key,
        grab::CommandKind::KeyDown,
        grab::CommandKind::KeyUp,
        grab::CommandKind::Click,
        grab::CommandKind::ClickAt,
        grab::CommandKind::Press,
        grab::CommandKind::Release,
        grab::CommandKind::Scroll,
        grab::CommandKind::Warp,
        grab::CommandKind::Move,
        grab::CommandKind::Follow,
        grab::CommandKind::Drag,
        grab::CommandKind::Capture,
        grab::CommandKind::Wait,
        grab::CommandKind::OverlayAdd,
        grab::CommandKind::OverlayUpdate,
        grab::CommandKind::OverlayRemove,
        grab::CommandKind::OverlayClear,
        grab::CommandKind::OverlayGrab,
        grab::CommandKind::OverlayRelease,
        grab::CommandKind::OverlayAttach,
        grab::CommandKind::OverlayDetach,
    } );

    template<std::size_t... Index>
    [[nodiscard]]
    constexpr bool
    alternative_kinds_match( std::index_sequence<Index...> ) noexcept
    {
        return ( ... && ( std::variant_alternative_t<Index, Command>::commandKind ==
                          expectedKinds[Index] ) );
    }

    [[nodiscard]]
    constexpr bool
    expected_kinds_are_unique() noexcept
    {
        for( std::size_t left = 0U; left < expectedKinds.size(); ++left )
        {
            for( std::size_t right = left + 1U; right < expectedKinds.size(); ++right )
            {
                if( expectedKinds[left] == expectedKinds[right] )
                {
                    return false;
                }
            }
        }
        return true;
    }

    // The 23-of-38 gap: is_sequence_command must be true for exactly the kinds
    // that have a payload struct, because the interpreter distinguishes "not
    // available as a sequence step" from "unknown op" on that answer alone.
    [[nodiscard]]
    constexpr bool
    only_payload_kinds_are_sequence_commands() noexcept
    {
        for( std::size_t raw = 0U;
             raw < static_cast<std::size_t>( grab::CommandKind::Count );
             ++raw )
        {
            const auto kind = static_cast<grab::CommandKind>( raw );
            const bool listed =
                std::ranges::find( expectedKinds, kind ) != expectedKinds.end();
            if( grab::sequence::is_sequence_command( kind ) != listed )
            {
                return false;
            }
        }
        return true;
    }

}    // namespace

TEST( CommandVariant,
      IsClosedAtTwentyThreeAlternatives )
{
    static_assert( std::variant_size_v<Command> == expectedAlternativeCount );
    static_assert( grab::sequence::sequenceCommandCount == expectedAlternativeCount );
    static_assert( expectedKinds.size() == expectedAlternativeCount );
    SUCCEED();
}

TEST( CommandVariant,
      AlternativeOrderMatchesTheExpectedKinds )
{
    static_assert(
        alternative_kinds_match( std::make_index_sequence<expectedAlternativeCount>{} )
    );
    static_assert( expected_kinds_are_unique() );
    SUCCEED();
}

TEST( CommandVariant,
      KindOfMapsEveryAlternative )
{
    using grab::sequence::kind_of;

    EXPECT_EQ( kind_of( Command{ TypeCommand{} } ), grab::CommandKind::Type );
    EXPECT_EQ( kind_of( Command{ KeyCommand{} } ), grab::CommandKind::Key );
    EXPECT_EQ( kind_of( Command{ KeyDownCommand{} } ), grab::CommandKind::KeyDown );
    EXPECT_EQ( kind_of( Command{ KeyUpCommand{} } ), grab::CommandKind::KeyUp );
    EXPECT_EQ( kind_of( Command{ ClickCommand{} } ), grab::CommandKind::Click );
    EXPECT_EQ( kind_of( Command{ ClickAtCommand{} } ), grab::CommandKind::ClickAt );
    EXPECT_EQ( kind_of( Command{ PressCommand{} } ), grab::CommandKind::Press );
    EXPECT_EQ( kind_of( Command{ ReleaseCommand{} } ), grab::CommandKind::Release );
    EXPECT_EQ( kind_of( Command{ ScrollCommand{} } ), grab::CommandKind::Scroll );
    EXPECT_EQ( kind_of( Command{ WarpCommand{} } ), grab::CommandKind::Warp );
    EXPECT_EQ( kind_of( Command{ MoveCommand{} } ), grab::CommandKind::Move );
    EXPECT_EQ( kind_of( Command{ FollowCommand{} } ), grab::CommandKind::Follow );
    EXPECT_EQ( kind_of( Command{ DragCommand{} } ), grab::CommandKind::Drag );
    EXPECT_EQ( kind_of( Command{ CaptureCommand{} } ), grab::CommandKind::Capture );
    EXPECT_EQ( kind_of( Command{ WaitCommand{} } ), grab::CommandKind::Wait );
    EXPECT_EQ( kind_of( Command{ OverlayAddCommand{} } ),
               grab::CommandKind::OverlayAdd );
    EXPECT_EQ( kind_of( Command{ OverlayUpdateCommand{} } ),
               grab::CommandKind::OverlayUpdate );
    EXPECT_EQ( kind_of( Command{ OverlayRemoveCommand{} } ),
               grab::CommandKind::OverlayRemove );
    EXPECT_EQ( kind_of( Command{ OverlayClearCommand{} } ),
               grab::CommandKind::OverlayClear );
    EXPECT_EQ( kind_of( Command{ OverlayGrabCommand{} } ),
               grab::CommandKind::OverlayGrab );
    EXPECT_EQ( kind_of( Command{ OverlayReleaseCommand{} } ),
               grab::CommandKind::OverlayRelease );
    EXPECT_EQ( kind_of( Command{ OverlayAttachCommand{} } ),
               grab::CommandKind::OverlayAttach );
    EXPECT_EQ( kind_of( Command{ OverlayDetachCommand{} } ),
               grab::CommandKind::OverlayDetach );
}

TEST( CommandVariant,
      EveryAlternativeKindNamesADescriptorRow )
{
    for( std::size_t index = 0U; index < expectedKinds.size(); ++index )
    {
        SCOPED_TRACE( index );
        // Every kind names a real descriptor row, and that row round-trips.
        const auto name = grab::command_name( expectedKinds[index] );
        EXPECT_FALSE( name.empty() );
        EXPECT_EQ( grab::command_kind( name ), expectedKinds[index] );
    }
}

TEST( CommandVariant,
      MoveAndWarpAreDifferentAlternatives )
{
    // An interpolated walk and a teleport are different operations; the
    // library only ever had the teleport, welded to a drag.
    static_assert( !std::is_same_v<MoveCommand, WarpCommand> );
    static_assert( MoveCommand::commandKind != WarpCommand::commandKind );
    EXPECT_NE( grab::sequence::kind_of( Command{ MoveCommand{} } ),
               grab::sequence::kind_of( Command{ WarpCommand{} } ) );
}

TEST( CommandVariant,
      DefaultConstructsTheFirstAlternative )
{
    const Command command{};
    EXPECT_EQ( command.index(), 0U );
    EXPECT_EQ( grab::sequence::kind_of( command ), grab::CommandKind::Type );
}

TEST( CommandVariant,
      OnlyThePayloadKindsAreSequenceCommands )
{
    static_assert( only_payload_kinds_are_sequence_commands() );

    // Spot-checked in both directions so a broken helper cannot pass vacuously.
    static_assert( grab::sequence::is_sequence_command( grab::CommandKind::KeyDown ) );
    static_assert( grab::sequence::is_sequence_command( grab::CommandKind::Wait ) );
    static_assert( !grab::sequence::is_sequence_command( grab::CommandKind::Doctor ) );
    static_assert(
        !grab::sequence::is_sequence_command( grab::CommandKind::DragCurve )
    );
    static_assert(
        !grab::sequence::is_sequence_command( grab::CommandKind::OverlayTrail )
    );
    static_assert( !grab::sequence::is_sequence_command( grab::CommandKind::Play ) );

    // The overlay STEPS are steps; the four overlay.* CLI verbs are not, and
    // they are a different set of kinds with different names.
    static_assert(
        grab::sequence::is_sequence_command( grab::CommandKind::OverlayAdd )
    );
    static_assert(
        grab::sequence::is_sequence_command( grab::CommandKind::OverlayDetach )
    );
    static_assert(
        !grab::sequence::is_sequence_command( grab::CommandKind::OverlaySketch )
    );
    SUCCEED();
}

TEST( CommandVariant,
      TheRecordingSeatSatisfiesEverySeam )
{
    // A seat that MISSES a seam still compiles: the enter() arm takes the
    // missing-capability branch instead. So a signature drift in the overlay
    // seam would be silent until the units built on it found every assertion
    // empty. static_assert because a compile failure is the stronger signal.
    static_assert( grab::kernel::sequence::PointerSeat<grab::testing::RecordingSeat> );
    static_assert( grab::kernel::sequence::OverlaySeat<grab::testing::RecordingSeat> );
    SUCCEED();
}

TEST( CommandStep,
      DefaultsToAbortWithNoGraceNoLabelAndANilId )
{
    const Step step{};

    // Abort is the default because a sequence that keeps going after an
    // unexplained failure feeds input into an application in an unknown state.
    EXPECT_EQ( step.on_error, ErrorPolicy::Abort );
    EXPECT_EQ( step.extra_grace, std::chrono::milliseconds::zero() );
    EXPECT_TRUE( step.label.empty() );
    EXPECT_TRUE( step.on_error_target.empty() );
    EXPECT_TRUE( step.after.empty() );

    // Identity is assigned by the document, never by the struct: a
    // default-constructed Step names no position.
    EXPECT_TRUE( step.id.is_nil() );
    EXPECT_EQ( step.id, grab::sequence::StepId::nil() );
}

TEST( CommandStep,
      ErrorPolicyNamesRoundTrip )
{
    static_assert( grab::sequence::error_policy_name( ErrorPolicy::Abort ) == "abort" );
    static_assert( grab::sequence::error_policy_name( ErrorPolicy::Continue ) ==
                   "continue" );
    static_assert( grab::sequence::error_policy_name( ErrorPolicy::Goto ) == "goto" );

    static_assert( grab::sequence::error_policy_from_name( "abort" ) ==
                   ErrorPolicy::Abort );
    static_assert( grab::sequence::error_policy_from_name( "continue" ) ==
                   ErrorPolicy::Continue );
    static_assert( grab::sequence::error_policy_from_name( "goto" ) ==
                   ErrorPolicy::Goto );

    constexpr std::string_view unknownPolicy = "retry";
    static_assert(
        !grab::sequence::error_policy_from_name( unknownPolicy ).has_value()
    );
    static_assert( grab::sequence::error_policy_name( ErrorPolicy::Count ).empty() );
    SUCCEED();
}

TEST( CommandStep,
      ButtonBearingCommandsDefaultToThePrimaryButton )
{
    static_assert( ClickCommand{}.button == grab::input::primaryButton );
    static_assert( ClickAtCommand{}.button == grab::input::primaryButton );
    static_assert( PressCommand{}.button == grab::input::primaryButton );
    static_assert( ReleaseCommand{}.button == grab::input::primaryButton );
    static_assert( DragCommand{}.button == grab::input::primaryButton );
    SUCCEED();
}
