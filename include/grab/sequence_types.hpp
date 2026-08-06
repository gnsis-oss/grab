#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Vocabulary shared by everything that loads, holds or plays a command
// sequence: the identity a step carries, the states a run moves through, and
// the time a step declares.
//
// This header deliberately does NOT live in ids.hpp. The identities there are
// runtime/session identity — they name things that exist while a display is
// attached. A StepId names a position in a *document*, has a different
// lifetime, and depends on none of them.

#include "grab/enum_table.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace grab::sequence
{

    // 16-bit index, so a sequence holds at most this many steps. Index 0 is
    // usable because nil is (0,0) and generation starts at 1; the naive
    // reading of "16 bits" is off by one.
    inline constexpr std::size_t maxSteps = 65'536U;

    // Index + generation packed into one 32-bit word, the shape WidgetRef
    // already spells out longhand.
    //
    // Identity is POSITIONAL, never content-derived: two byte-identical
    // input.click steps are different steps because they sit at different
    // positions, so a content hash would collide exactly where the format
    // needs them distinct. Positional identity is also deterministic, which is
    // what lets a document round-trip through JSON without ids being written
    // into the file.
    //
    // The generation half is RESERVED and inert. Nothing in the current design
    // bumps it: a Sequence is immutable, splice() appends above the high-water
    // index, and no removal exists, so every live StepId carries generation 1.
    // It is a reservation for a future remove(), not an active safety
    // mechanism, and must not be described as one.
    class StepId final
    {
        public:

            using Bits                      = std::uint32_t;
            using Half                      = std::uint16_t;

            static constexpr Bits laneShift = 16U;
            static constexpr Bits indexMask = 0X00'00'FF'FFU;

            // Generation 0 exists only so that (0,0) can be nil; a live step
            // starts here.
            static constexpr Half firstGeneration = 1U;

            constexpr StepId() noexcept           = default;

            constexpr StepId( Half index,
                              Half generation ) noexcept :
                bits_( static_cast<Bits>( static_cast<Bits>( generation )
                                          << laneShift ) |
                       static_cast<Bits>( index ) )
            {
            }

            [[nodiscard]]
            static constexpr StepId
            from_bits( Bits bits ) noexcept
            {
                StepId id;
                id.bits_ = bits;
                return id;
            }

            [[nodiscard]]
            static constexpr StepId
            nil() noexcept
            {
                return StepId{};
            }

            [[nodiscard]]
            constexpr Bits
            bits() const noexcept
            {
                return bits_;
            }

            [[nodiscard]]
            constexpr Half
            index() const noexcept
            {
                return static_cast<Half>( bits_ & indexMask );
            }

            [[nodiscard]]
            constexpr Half
            generation() const noexcept
            {
                return static_cast<Half>( bits_ >> laneShift );
            }

            [[nodiscard]]
            constexpr bool
            is_nil() const noexcept
            {
                return bits_ == 0U;
            }

            [[nodiscard]]
            friend constexpr auto
            operator<=>( StepId lhs,
                         StepId rhs ) noexcept = default;

        private:

            Bits bits_{ 0U };
    };

    static_assert( sizeof( StepId ) == sizeof( StepId::Bits ) );

    // What one command reports back to the pump. Ported in meaning from
    // weft::Status: "Running until not Running" is the single mechanism a wait
    // and a slow screenshot share.
    enum class Status : std::uint8_t
    {
        Running,
        Success,
        Failure,
        Count,
    };

    // Named PlayState rather than RunState because grab::config::RunState
    // already exists, with different values.
    enum class PlayState : std::uint8_t
    {
        Idle,
        Playing,
        Paused,
        Interrupted,
        Done,
        Count,
    };

    enum class StepStatus : std::uint8_t
    {
        Pending,
        Ready,
        Running,
        Succeeded,
        Failed,
        Skipped,
        Count,
    };

    // Where a step's duration comes from. Instant still gets measured — an
    // XTest round trip and a flush are not free.
    enum class TimingClass : std::uint8_t
    {
        Instant,
        Timed,
        Opaque,
        Count,
    };

    // The mode is the SOLE authority on inter-step pacing. Per-step
    // extra_grace is ignored outside Precise rather than silently honoured,
    // because otherwise "Strict allows no gaps" would be a lie any single step
    // could tell.
    enum class PacingMode : std::uint8_t
    {
        Strict,
        Grace,
        Precise,
        Count,
    };

    struct PacingOptions
    {
            PacingMode                mode{ PacingMode::Strict };
            std::chrono::milliseconds grace{ std::chrono::milliseconds::zero() };
    };

    // The governing rule: no duration defaults to zero.
    //
    // `declared == nullopt` means UNKNOWN, SO MEASURE IT — never zero. Making
    // it optional rather than a zero sentinel is what stops the zero-time
    // assumption reappearing silently.
    //
    // call_duration and server_observed are TWO DIFFERENT CLOCKS and must
    // never be subtracted from each other: an XInput2 timestamp is a 32-bit
    // X-server millisecond counter that shares no origin with steady_clock.
    struct StepTiming
    {
            std::optional<std::chrono::nanoseconds>  declared{};
            std::chrono::nanoseconds                 call_duration{};
            std::optional<std::chrono::milliseconds> server_observed{};
    };

    // A dependency edge carries no payload: the dependency itself is the whole
    // meaning. It exists as a named type so AdjacencyGraph's Payload parameter
    // says what the graph is, rather than std::monostate saying nothing.
    struct DependencyEdge
    {
    };

    namespace detail
    {

        inline constexpr auto statusNames = EnumTable{
            std::to_array( {
                enum_entry( Status::Running, "running" ),
                enum_entry( Status::Success, "success" ),
                enum_entry( Status::Failure, "failure" ),
            } ),
        };
        static_assert( enum_table_has_count( statusNames,
                                             Status::Count ) );

        inline constexpr auto playStateNames = EnumTable{
            std::to_array( {
                enum_entry( PlayState::Idle, "idle" ),
                enum_entry( PlayState::Playing, "playing" ),
                enum_entry( PlayState::Paused, "paused" ),
                enum_entry( PlayState::Interrupted, "interrupted" ),
                enum_entry( PlayState::Done, "done" ),
            } ),
        };
        static_assert( enum_table_has_count( playStateNames,
                                             PlayState::Count ) );

        inline constexpr auto stepStatusNames = EnumTable{
            std::to_array( {
                enum_entry( StepStatus::Pending, "pending" ),
                enum_entry( StepStatus::Ready, "ready" ),
                enum_entry( StepStatus::Running, "running" ),
                enum_entry( StepStatus::Succeeded, "succeeded" ),
                enum_entry( StepStatus::Failed, "failed" ),
                enum_entry( StepStatus::Skipped, "skipped" ),
            } ),
        };
        static_assert( enum_table_has_count( stepStatusNames,
                                             StepStatus::Count ) );

        inline constexpr auto timingClassNames = EnumTable{
            std::to_array( {
                enum_entry( TimingClass::Instant, "instant" ),
                enum_entry( TimingClass::Timed, "timed" ),
                enum_entry( TimingClass::Opaque, "opaque" ),
            } ),
        };
        static_assert( enum_table_has_count( timingClassNames,
                                             TimingClass::Count ) );

        inline constexpr auto pacingModeNames = EnumTable{
            std::to_array( {
                enum_entry( PacingMode::Strict, "strict" ),
                enum_entry( PacingMode::Grace, "grace" ),
                enum_entry( PacingMode::Precise, "precise" ),
            } ),
        };
        static_assert( enum_table_has_count( pacingModeNames,
                                             PacingMode::Count ) );

    }    // namespace detail

    [[nodiscard]]
    constexpr std::string_view
    status_name( Status value ) noexcept
    {
        return detail::statusNames.text_of( value, "" );
    }

    [[nodiscard]]
    constexpr std::optional<Status>
    status_from_name( std::string_view text ) noexcept
    {
        return detail::statusNames.value_of( text );
    }

    [[nodiscard]]
    constexpr std::string_view
    play_state_name( PlayState value ) noexcept
    {
        return detail::playStateNames.text_of( value, "" );
    }

    [[nodiscard]]
    constexpr std::optional<PlayState>
    play_state_from_name( std::string_view text ) noexcept
    {
        return detail::playStateNames.value_of( text );
    }

    [[nodiscard]]
    constexpr std::string_view
    step_status_name( StepStatus value ) noexcept
    {
        return detail::stepStatusNames.text_of( value, "" );
    }

    [[nodiscard]]
    constexpr std::optional<StepStatus>
    step_status_from_name( std::string_view text ) noexcept
    {
        return detail::stepStatusNames.value_of( text );
    }

    [[nodiscard]]
    constexpr std::string_view
    timing_class_name( TimingClass value ) noexcept
    {
        return detail::timingClassNames.text_of( value, "" );
    }

    [[nodiscard]]
    constexpr std::optional<TimingClass>
    timing_class_from_name( std::string_view text ) noexcept
    {
        return detail::timingClassNames.value_of( text );
    }

    [[nodiscard]]
    constexpr std::string_view
    pacing_mode_name( PacingMode value ) noexcept
    {
        return detail::pacingModeNames.text_of( value, "" );
    }

    [[nodiscard]]
    constexpr std::optional<PacingMode>
    pacing_mode_from_name( std::string_view text ) noexcept
    {
        return detail::pacingModeNames.value_of( text );
    }

}    // namespace grab::sequence

// Without this, std::unordered_map<StepId, ...> — which topological_order
// instantiates — fails to compile at the point of use rather than here.
template<>
struct std::hash<grab::sequence::StepId>
{
        [[nodiscard]]
        std::size_t
        operator()( grab::sequence::StepId id ) const noexcept
        {
            return std::hash<grab::sequence::StepId::Bits>{}( id.bits() );
        }
};
