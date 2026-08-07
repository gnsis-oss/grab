// Contract tests for the sequence-era extension of the descriptor table: the
// eleven new kinds, and the two new columns (timing, blocking) the player
// schedules against.
//
// The table is the single source of truth for both. A wrong timing class makes
// the player mis-plan; a wrong `blocking` puts a synchronous body on the thread
// that owns deadlines, which slips every deadline in the frontier by its full
// duration. Both are asserted here rather than inferred at run time.

#include "grab/command_descriptor.hpp"
#include "grab/sequence_types.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
// clang-format on

namespace
{

    using grab::CommandKind;
    using grab::sequence::TimingClass;

    constexpr std::size_t    expectedDescriptorCount = 38U;
    constexpr std::size_t    expectedSequenceEraRows = 19U;
    constexpr std::ptrdiff_t expectedRowsPerKind     = 1;

    // Every kind whose body must run on a worker: it blocks for an unbounded,
    // unknowable time.
    constexpr auto           opaqueBlockingKinds = std::to_array( {
        CommandKind::Capture,
        CommandKind::Doctor,
        CommandKind::Windows,
        CommandKind::Compare,
        CommandKind::Batch,
    } );

    // Opaque duration, but the call itself returns promptly.
    constexpr auto           opaqueNonBlockingKinds = std::to_array( {
        CommandKind::Watch,
        CommandKind::Daemon,
        CommandKind::Session,
        CommandKind::Focus,
        CommandKind::Place,
        CommandKind::Play,
    } );

    // One XTest round trip and a flush: measured, but never planned for.
    constexpr auto           instantKinds = std::to_array( {
        CommandKind::Type,
        CommandKind::Key,
        CommandKind::Click,
        CommandKind::Warp,
        CommandKind::Press,
        CommandKind::Release,
        CommandKind::Scroll,
        CommandKind::ClickAt,
        CommandKind::KeyDown,
        CommandKind::KeyUp,
        CommandKind::OverlayTrail,
        CommandKind::OverlayShape,
        CommandKind::OverlayFeedback,
        CommandKind::OverlaySketch,
        // The overlay steps. A mutation from the reactor thread averages
        // 0.02 ms; the frame is paid by the player's per-tick flush, so no
        // overlay step owns a frame's latency and none of them blocks.
        CommandKind::OverlayAdd,
        CommandKind::OverlayUpdate,
        CommandKind::OverlayRemove,
        CommandKind::OverlayClear,
        CommandKind::OverlayGrab,
        CommandKind::OverlayRelease,
        CommandKind::OverlayAttach,
        CommandKind::OverlayDetach,
    } );

    // A duration the document declares or the options imply, spent as
    // deadlines rather than as a blocked thread.
    constexpr auto           timedKinds = std::to_array( {
        CommandKind::Drag,
        CommandKind::DragCurve,
        CommandKind::Move,
        CommandKind::Follow,
        CommandKind::Wait,
    } );

    // The nineteen rows the sequence work added: eleven input-and-time ops,
    // then the eight overlay steps.
    constexpr auto           sequenceEraKinds = std::to_array( {
        CommandKind::Move,
        CommandKind::Warp,
        CommandKind::Follow,
        CommandKind::Press,
        CommandKind::Release,
        CommandKind::Scroll,
        CommandKind::ClickAt,
        CommandKind::KeyDown,
        CommandKind::KeyUp,
        CommandKind::Wait,
        CommandKind::Play,
        CommandKind::OverlayAdd,
        CommandKind::OverlayUpdate,
        CommandKind::OverlayRemove,
        CommandKind::OverlayClear,
        CommandKind::OverlayGrab,
        CommandKind::OverlayRelease,
        CommandKind::OverlayAttach,
        CommandKind::OverlayDetach,
    } );

    // The overlay steps that converge on the same end state however often they
    // run: removing an already-removed handle, clearing an empty scene,
    // releasing an ungrabbed pointer, detaching an unattached shape.
    constexpr auto           idempotentOverlayKinds = std::to_array( {
        CommandKind::OverlayRemove,
        CommandKind::OverlayClear,
        CommandKind::OverlayRelease,
        CommandKind::OverlayDetach,
    } );

    // A second add draws a second shape, so it is not a repeat of the first.
    constexpr auto           neverRetriedOverlayKinds = std::to_array( {
        CommandKind::OverlayAdd,
        CommandKind::OverlayUpdate,
        CommandKind::OverlayAttach,
        CommandKind::OverlayGrab,
    } );

    [[nodiscard]]
    constexpr const grab::CommandDescriptor*
    row_for( CommandKind kind ) noexcept
    {
        const auto&       commands = grab::list_commands();
        const auto* const found =
            std::ranges::find( commands, kind, &grab::CommandDescriptor::kind );
        return found == commands.end() ? nullptr : found;
    }

    template<std::size_t Size>
    [[nodiscard]]
    constexpr bool
    all_rows_are( const std::array<CommandKind,
                                   Size>& kinds,
                  TimingClass             timing,
                  bool                    blocking ) noexcept
    {
        for( const auto kind : kinds )
        {
            const auto* const row = row_for( kind );
            if( row == nullptr || row->timing != timing || row->blocking != blocking )
            {
                return false;
            }
        }
        return true;
    }

    // The four groups above must partition CommandKind exactly: no kind
    // classified twice, none left unclassified.
    [[nodiscard]]
    constexpr bool
    groups_partition_every_kind() noexcept
    {
        const std::size_t total = opaqueBlockingKinds.size() +
                                  opaqueNonBlockingKinds.size() +
                                  instantKinds.size() +
                                  timedKinds.size();
        if( total != expectedDescriptorCount )
        {
            return false;
        }

        for( std::size_t raw = 0U; raw < static_cast<std::size_t>( CommandKind::Count );
             ++raw )
        {
            const auto        kind = static_cast<CommandKind>( raw );
            const std::size_t seen =
                static_cast<std::size_t>( std::ranges::find( opaqueBlockingKinds,
                                                             kind ) !=
                                          opaqueBlockingKinds.end() ) +
                static_cast<std::size_t>( std::ranges::find( opaqueNonBlockingKinds,
                                                             kind ) !=
                                          opaqueNonBlockingKinds.end() ) +
                static_cast<std::size_t>( std::ranges::find( instantKinds, kind ) !=
                                          instantKinds.end() ) +
                static_cast<std::size_t>( std::ranges::find( timedKinds, kind ) !=
                                          timedKinds.end() );
            if( seen != 1U )
            {
                return false;
            }
        }
        return true;
    }

}    // namespace

TEST( CommandDescriptorExt,
      TableHoldsThirtyEightRows )
{
    static_assert( grab::list_commands().size() == expectedDescriptorCount );
    static_assert( grab::list_commands().size() ==
                   static_cast<std::size_t>( CommandKind::Count ) );
    EXPECT_EQ( grab::list_commands().size(), expectedDescriptorCount );
}

TEST( CommandDescriptorExt,
      EveryKindHasExactlyOneRow )
{
    for( std::size_t raw = 0U; raw < static_cast<std::size_t>( CommandKind::Count );
         ++raw )
    {
        const auto kind = static_cast<CommandKind>( raw );
        SCOPED_TRACE( raw );

        const auto& commands = grab::list_commands();
        EXPECT_EQ( std::ranges::count( commands, kind, &grab::CommandDescriptor::kind ),
                   expectedRowsPerKind );
        EXPECT_FALSE( grab::command_name( kind ).empty() );
    }
}

TEST( CommandDescriptorExt,
      TheSequenceEraInputOpsResolve )
{
    // Without key_down/key_up no sequence can express a chord: Keystroke
    // carries only shift and altgr, so Ctrl+C is unreachable through
    // input.type and input.key.
    static_assert( grab::command_kind( "input.key_down" ) == CommandKind::KeyDown );
    static_assert( grab::command_kind( "input.key_up" ) == CommandKind::KeyUp );
    static_assert( grab::command_kind( "input.press" ) == CommandKind::Press );
    static_assert( grab::command_kind( "input.release" ) == CommandKind::Release );
    static_assert( grab::command_kind( "input.scroll" ) == CommandKind::Scroll );

    static_assert( grab::command_name( CommandKind::KeyDown ) == "input.key_down" );
    static_assert( grab::command_name( CommandKind::KeyUp ) == "input.key_up" );
    static_assert( grab::command_name( CommandKind::Press ) == "input.press" );
    static_assert( grab::command_name( CommandKind::Release ) == "input.release" );
    static_assert( grab::command_name( CommandKind::Scroll ) == "input.scroll" );
    SUCCEED();
}

TEST( CommandDescriptorExt,
      EveryNewKindResolvesBothWays )
{
    for( const auto kind : sequenceEraKinds )
    {
        const auto name = grab::command_name( kind );
        SCOPED_TRACE( name );
        ASSERT_FALSE( name.empty() );
        EXPECT_EQ( grab::command_kind( name ), kind );
    }
    EXPECT_EQ( sequenceEraKinds.size(), expectedSequenceEraRows );
}

TEST( CommandDescriptorExt,
      MoveIsNotWarp )
{
    // An interpolated walk and a teleport are different operations. The
    // library only ever exposed the teleport (Input::move), and interpolation
    // only existed welded to a button press inside execute_drag.
    static_assert( grab::command_kind( "input.move" ) != CommandKind::Warp );
    static_assert( grab::command_kind( "input.warp" ) != CommandKind::Move );
    static_assert( grab::command_kind( "input.move" ) == CommandKind::Move );
    static_assert( grab::command_kind( "input.warp" ) == CommandKind::Warp );

    static_assert( grab::timing_class_of( CommandKind::Move ) == TimingClass::Timed );
    static_assert( grab::timing_class_of( CommandKind::Warp ) == TimingClass::Instant );
    SUCCEED();
}

TEST( CommandDescriptorExt,
      WaitIsTimed )
{
    static_assert( grab::timing_class_of( CommandKind::Wait ) == TimingClass::Timed );
    static_assert( !grab::is_blocking_command( CommandKind::Wait ) );
    static_assert( grab::command_name( CommandKind::Wait ) == "time.wait" );
    SUCCEED();
}

TEST( CommandDescriptorExt,
      CaptureIsOpaqueAndBlocking )
{
    // A synchronous screenshot is the archetypal body that must not run on the
    // thread owning deadlines.
    static_assert( grab::timing_class_of( CommandKind::Capture ) ==
                   TimingClass::Opaque );
    static_assert( grab::is_blocking_command( CommandKind::Capture ) );
    SUCCEED();
}

TEST( CommandDescriptorExt,
      WarpIsInstantAndNotBlocking )
{
    static_assert( grab::timing_class_of( CommandKind::Warp ) == TimingClass::Instant );
    static_assert( !grab::is_blocking_command( CommandKind::Warp ) );
    SUCCEED();
}

TEST( CommandDescriptorExt,
      TimingAndBlockingMatchTheClassificationOfEveryRow )
{
    static_assert( groups_partition_every_kind() );
    static_assert( all_rows_are( opaqueBlockingKinds, TimingClass::Opaque, true ) );
    static_assert( all_rows_are( opaqueNonBlockingKinds, TimingClass::Opaque, false ) );
    static_assert( all_rows_are( instantKinds, TimingClass::Instant, false ) );
    static_assert( all_rows_are( timedKinds, TimingClass::Timed, false ) );
    SUCCEED();
}

TEST( CommandDescriptorExt,
      TheOverlayStepsAreInstantMutatingAndNeverBlocking )
{
    // Not the four overlay.* CLI verbs: those are interactive tools with no
    // payload, and they keep their own names.
    static_assert( grab::command_kind( "overlay.add" ) == CommandKind::OverlayAdd );
    static_assert( grab::command_kind( "overlay.grab" ) == CommandKind::OverlayGrab );
    static_assert( grab::command_kind( "overlay.trail" ) == CommandKind::OverlayTrail );
    static_assert( grab::command_name( CommandKind::OverlayDetach ) ==
                   "overlay.detach" );

    for( const auto kind : idempotentOverlayKinds )
    {
        const auto* const row = row_for( kind );
        ASSERT_NE( row, nullptr ) << grab::command_name( kind );
        SCOPED_TRACE( grab::command_name( kind ) );
        EXPECT_EQ( row->retry, grab::RetryClass::Idempotent );
        EXPECT_TRUE( row->idempotent );
        EXPECT_EQ( row->mutability, grab::Mutability::Mutating );
        EXPECT_EQ( row->timing, TimingClass::Instant );
        EXPECT_FALSE( row->blocking );
    }

    for( const auto kind : neverRetriedOverlayKinds )
    {
        const auto* const row = row_for( kind );
        ASSERT_NE( row, nullptr ) << grab::command_name( kind );
        SCOPED_TRACE( grab::command_name( kind ) );
        EXPECT_EQ( row->retry, grab::RetryClass::Never );
        EXPECT_FALSE( row->idempotent );
        EXPECT_EQ( row->mutability, grab::Mutability::Mutating );
        EXPECT_EQ( row->timing, TimingClass::Instant );
        EXPECT_FALSE( row->blocking );
    }
}

TEST( CommandDescriptorExt,
      AnUnknownNameResolvesToNothing )
{
    constexpr std::string_view unknownOp = "input.telepathy";
    static_assert( !grab::command_kind( unknownOp ).has_value() );
    static_assert( !grab::command_kind( "" ).has_value() );

    // The Count sentinel is not a row, and asking for its metadata must not
    // read past the end of the table.
    static_assert( grab::command_name( CommandKind::Count ).empty() );
    static_assert( !grab::is_blocking_command( CommandKind::Count ) );
    SUCCEED();
}
