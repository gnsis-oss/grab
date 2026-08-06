// Contract tests for the sequence vocabulary: the identity a step carries, the
// capacity that identity implies, the time a step declares, and the five enum
// name tables the JSON format is written against.
//
// Type-level facts are asserted with static_assert inside TEST bodies: a
// compilation failure is a stronger signal than a runtime one, and every one of
// these is a compile-time property of the header rather than of a running
// program.

#include "grab/sequence_types.hpp"

// clang-format off
#include <gtest/gtest.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
// clang-format on

namespace
{

    using grab::sequence::PacingMode;
    using grab::sequence::PacingOptions;
    using grab::sequence::PlayState;
    using grab::sequence::Status;
    using grab::sequence::StepId;
    using grab::sequence::StepStatus;
    using grab::sequence::StepTiming;
    using grab::sequence::TimingClass;

    // 16-bit index with generation starting at 1, so index 0 is a usable
    // position rather than a second spelling of nil, and capacity is one more
    // than the naive reading of "16 bits".
    constexpr std::size_t      expectedMaxSteps = 65'536U;

    constexpr StepId::Half     firstIndex       = 0U;
    constexpr StepId::Half     secondIndex      = 1U;
    constexpr StepId::Half     thirdIndex       = 2U;
    constexpr StepId::Half     lastIndex{ 65'535U };

    constexpr StepId::Half     nilGeneration    = 0U;
    constexpr StepId::Half     firstGeneration  = 1U;
    constexpr StepId::Half     secondGeneration = 2U;

    constexpr StepId::Bits     firstLiveBits    = 0X00'01'00'00U;

    constexpr std::size_t      firstPayload     = 11U;
    constexpr std::size_t      secondPayload    = 22U;
    constexpr std::size_t      thirdPayload     = 33U;

    constexpr auto             declaredDuration = std::chrono::nanoseconds{ 5'000'000 };
    constexpr auto             measuredDuration = std::chrono::nanoseconds{ 7'000'000 };
    constexpr auto             observedOnServer = std::chrono::milliseconds{ 9 };
    constexpr auto             graceInterval    = std::chrono::milliseconds{ 25 };

    constexpr std::string_view unknownName      = "no.such.name";
    constexpr std::string_view emptyName        = "";

    // Every entry in a table must name its value, and the name must resolve
    // back to exactly that value. Count is excluded: it is a sentinel, not a
    // member of the vocabulary.
    template<typename Enum,
             typename NameFn,
             typename FromNameFn>
    [[nodiscard]]
    constexpr bool
    enum_names_round_trip( NameFn     name_of,
                           FromNameFn from_name ) noexcept
    {
        for( std::size_t raw = 0U; raw < static_cast<std::size_t>( Enum::Count ); ++raw )
        {
            const auto             value = static_cast<Enum>( raw );
            const std::string_view text  = name_of( value );
            if( text.empty() )
            {
                return false;
            }
            const std::optional<Enum> back = from_name( text );
            if( !back.has_value() || *back != value )
            {
                return false;
            }
        }
        return true;
    }

}    // namespace

TEST( StepId,
      PacksIndexAndGenerationIntoOneWord )
{
    static_assert( sizeof( StepId ) == sizeof( std::uint32_t ) );
    static_assert( std::is_same_v<StepId::Bits, std::uint32_t> );
    static_assert( sizeof( StepId ) == sizeof( StepId::Bits ) );
    static_assert( std::is_trivially_copyable_v<StepId> );
    SUCCEED();
}

TEST( StepId,
      DefaultIsNilAndTheFirstLivePositionIsNot )
{
    static_assert( StepId{}.is_nil() );
    static_assert( StepId{}.bits() == 0U );
    static_assert( StepId::nil() == StepId{} );
    static_assert( StepId{ firstIndex, nilGeneration }.is_nil() );

    // Generation starts at 1, so index 0 is a real, usable position. If nil
    // swallowed index 0 the first step of every document would be unnameable.
    static_assert( !StepId{ firstIndex, firstGeneration }.is_nil() );
    static_assert( StepId{ firstIndex, firstGeneration } != StepId::nil() );
    static_assert( StepId{ firstIndex, firstGeneration }.bits() == firstLiveBits );
    SUCCEED();
}

TEST( StepId,
      IndexAndGenerationRoundTrip )
{
    static_assert( StepId{ firstIndex, firstGeneration }.index() == firstIndex );
    static_assert( StepId{ firstIndex, firstGeneration }.generation() ==
                   firstGeneration );
    static_assert( StepId{ lastIndex, secondGeneration }.index() == lastIndex );
    static_assert( StepId{ lastIndex, secondGeneration }.generation() ==
                   secondGeneration );

    // The two halves never bleed into one another.
    static_assert( StepId{ lastIndex, firstGeneration }.generation() ==
                   firstGeneration );
    static_assert( StepId{ firstIndex, secondGeneration }.index() == firstIndex );

    static_assert( StepId::from_bits( StepId{ lastIndex, secondGeneration }.bits() ) ==
                   StepId{ lastIndex, secondGeneration } );
    static_assert( StepId::from_bits( StepId{}.bits() ).is_nil() );
    SUCCEED();
}

TEST( StepId,
      TwoDifferentPositionsGiveDifferentIds )
{
    // THE requirement that chose a packed index+generation over a content
    // hash. Two byte-identical unlabelled steps are different steps because
    // they sit at different positions, and a content hash would collide
    // exactly where the format needs them distinct.
    static_assert( StepId{ firstIndex, firstGeneration } !=
                   StepId{ secondIndex, firstGeneration } );
    static_assert( StepId{ secondIndex, firstGeneration } !=
                   StepId{ thirdIndex, firstGeneration } );
    static_assert( StepId{ firstIndex, firstGeneration }.bits() !=
                   StepId{ secondIndex, firstGeneration }.bits() );

    // Position also orders them, which is what makes a std::map keyed by
    // StepId enumerate in document order.
    static_assert( StepId{ firstIndex, firstGeneration } <
                   StepId{ secondIndex, firstGeneration } );
    static_assert( StepId{ secondIndex, firstGeneration } <
                   StepId{ thirdIndex, firstGeneration } );
    SUCCEED();
}

TEST( StepId,
      EveryPositionInTheIndexSpaceIsDistinct )
{
    std::unordered_set<StepId> seen;
    seen.reserve( expectedMaxSteps );

    for( std::size_t raw = 0U; raw < expectedMaxSteps; ++raw )
    {
        const StepId id{ static_cast<StepId::Half>( raw ), firstGeneration };
        ASSERT_FALSE( id.is_nil() ) << raw;
        EXPECT_TRUE( seen.insert( id ).second ) << raw;
    }

    EXPECT_EQ( seen.size(), expectedMaxSteps );
}

TEST( StepId,
      HashesAsAnUnorderedMapKey )
{
    // topological_order instantiates std::unordered_map<StepId, ...>; without
    // the std::hash specialisation that fails to compile at the point of use.
    static_assert( std::is_invocable_r_v<std::size_t, std::hash<StepId>, StepId> );

    const StepId                            first{ firstIndex, firstGeneration };
    const StepId                            second{ secondIndex, firstGeneration };
    const StepId                            third{ thirdIndex, firstGeneration };

    std::unordered_map<StepId, std::size_t> payloads;
    payloads[first]  = firstPayload;
    payloads[second] = secondPayload;
    payloads[third]  = thirdPayload;

    ASSERT_EQ( payloads.size(), 3U );
    EXPECT_EQ( payloads.at( first ), firstPayload );
    EXPECT_EQ( payloads.at( second ), secondPayload );
    EXPECT_EQ( payloads.at( third ), thirdPayload );
    EXPECT_FALSE( payloads.contains( StepId::nil() ) );

    // Equal ids hash equally however they were spelled.
    const std::hash<StepId> hasher;
    EXPECT_EQ( hasher( first ), hasher( StepId::from_bits( first.bits() ) ) );
}

TEST( StepId,
      CapacityIsOneMoreThanTheNaiveReadingOfSixteenBits )
{
    static_assert( grab::sequence::maxSteps == expectedMaxSteps );
    static_assert( grab::sequence::maxSteps == std::size_t{ lastIndex } + 1U );
    static_assert( grab::sequence::maxSteps != std::size_t{ lastIndex } );
    static_assert( StepId::firstGeneration == firstGeneration );
    SUCCEED();
}

TEST( StepTiming,
      DeclaredDefaultsToUnknownRatherThanZero )
{
    constexpr StepTiming timing{};

    // nullopt means UNKNOWN, SO MEASURE IT. A zero default would silently
    // reintroduce the zero-time assumption the optional exists to stop.
    static_assert( !timing.declared.has_value() );
    static_assert( timing.declared != std::chrono::nanoseconds::zero() );
    static_assert( !timing.server_observed.has_value() );

    // Only the duration we measure ourselves defaults to zero, because zero
    // measured time is a truthful statement about a call that has not run.
    static_assert( timing.call_duration == std::chrono::nanoseconds::zero() );
    SUCCEED();
}

TEST( StepTiming,
      DeclaredZeroIsDistinctFromUnknown )
{
    const StepTiming unknown{};
    const StepTiming zero{
        .declared        = std::chrono::nanoseconds::zero(),
        .call_duration   = {},
        .server_observed = {},
    };

    EXPECT_FALSE( unknown.declared.has_value() );
    ASSERT_TRUE( zero.declared.has_value() );
    EXPECT_EQ( *zero.declared, std::chrono::nanoseconds::zero() );
    EXPECT_NE( unknown.declared, zero.declared );
}

TEST( StepTiming,
      CarriesBothClocksSeparately )
{
    const StepTiming timing{
        .declared        = declaredDuration,
        .call_duration   = measuredDuration,
        .server_observed = observedOnServer,
    };

    ASSERT_TRUE( timing.declared.has_value() );
    EXPECT_EQ( *timing.declared, declaredDuration );
    EXPECT_EQ( timing.call_duration, measuredDuration );

    // server_observed is an X-server millisecond counter that shares no origin
    // with steady_clock; it is stored, never subtracted from call_duration.
    ASSERT_TRUE( timing.server_observed.has_value() );
    EXPECT_EQ( *timing.server_observed, observedOnServer );
}

TEST( PacingModeOptions,
      DefaultToStrictWithNoGrace )
{
    constexpr PacingOptions options{};
    static_assert( options.mode == PacingMode::Strict );
    static_assert( options.grace == std::chrono::milliseconds::zero() );

    const PacingOptions graceful{
        .mode  = PacingMode::Grace,
        .grace = graceInterval,
    };
    EXPECT_EQ( graceful.mode, PacingMode::Grace );
    EXPECT_EQ( graceful.grace, graceInterval );
}

TEST( CommandSequenceEnumNames,
      StatusRoundTrips )
{
    static_assert( enum_names_round_trip<Status>( &grab::sequence::status_name,
                                                  &grab::sequence::status_from_name ) );
    static_assert( grab::sequence::status_name( Status::Running ) == "running" );
    static_assert( grab::sequence::status_from_name( "failure" ) == Status::Failure );
    SUCCEED();
}

TEST( CommandSequenceEnumNames,
      PlayStateRoundTrips )
{
    static_assert(
        enum_names_round_trip<PlayState>( &grab::sequence::play_state_name,
                                          &grab::sequence::play_state_from_name )
    );
    static_assert( grab::sequence::play_state_name( PlayState::Idle ) == "idle" );
    static_assert( grab::sequence::play_state_from_name( "interrupted" ) ==
                   PlayState::Interrupted );
    SUCCEED();
}

TEST( CommandSequenceEnumNames,
      StepStatusRoundTrips )
{
    static_assert(
        enum_names_round_trip<StepStatus>( &grab::sequence::step_status_name,
                                           &grab::sequence::step_status_from_name )
    );
    static_assert( grab::sequence::step_status_name( StepStatus::Pending ) ==
                   "pending" );
    static_assert( grab::sequence::step_status_from_name( "skipped" ) ==
                   StepStatus::Skipped );
    SUCCEED();
}

TEST( CommandSequenceEnumNames,
      TimingClassRoundTrips )
{
    static_assert(
        enum_names_round_trip<TimingClass>( &grab::sequence::timing_class_name,
                                            &grab::sequence::timing_class_from_name )
    );
    static_assert( grab::sequence::timing_class_name( TimingClass::Instant ) ==
                   "instant" );
    static_assert( grab::sequence::timing_class_from_name( "opaque" ) ==
                   TimingClass::Opaque );
    SUCCEED();
}

TEST( CommandSequenceEnumNames,
      PacingModeRoundTrips )
{
    static_assert(
        enum_names_round_trip<PacingMode>( &grab::sequence::pacing_mode_name,
                                           &grab::sequence::pacing_mode_from_name )
    );
    static_assert( grab::sequence::pacing_mode_name( PacingMode::Strict ) == "strict" );
    static_assert( grab::sequence::pacing_mode_from_name( "precise" ) ==
                   PacingMode::Precise );
    SUCCEED();
}

TEST( CommandSequenceEnumNames,
      AnUnknownNameYieldsNullopt )
{
    static_assert( !grab::sequence::status_from_name( unknownName ).has_value() );
    static_assert( !grab::sequence::play_state_from_name( unknownName ).has_value() );
    static_assert( !grab::sequence::step_status_from_name( unknownName ).has_value() );
    static_assert( !grab::sequence::timing_class_from_name( unknownName ).has_value() );
    static_assert( !grab::sequence::pacing_mode_from_name( unknownName ).has_value() );

    // The empty string is not a name either: text_of() returns it as the
    // fallback for an out-of-table value, so it must never resolve back.
    static_assert( !grab::sequence::status_from_name( emptyName ).has_value() );
    static_assert( !grab::sequence::play_state_from_name( emptyName ).has_value() );
    static_assert( !grab::sequence::step_status_from_name( emptyName ).has_value() );
    static_assert( !grab::sequence::timing_class_from_name( emptyName ).has_value() );
    static_assert( !grab::sequence::pacing_mode_from_name( emptyName ).has_value() );
    SUCCEED();
}

TEST( CommandSequenceEnumNames,
      TheCountSentinelHasNoName )
{
    static_assert( grab::sequence::status_name( Status::Count ).empty() );
    static_assert( grab::sequence::play_state_name( PlayState::Count ).empty() );
    static_assert( grab::sequence::step_status_name( StepStatus::Count ).empty() );
    static_assert( grab::sequence::timing_class_name( TimingClass::Count ).empty() );
    static_assert( grab::sequence::pacing_mode_name( PacingMode::Count ).empty() );
    SUCCEED();
}
