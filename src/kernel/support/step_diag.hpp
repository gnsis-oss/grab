#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// Step-shaped timing, as opposed to diag's frame-shaped FrameReport.
//
// A sequence has no cadence. Its steps last microseconds or seconds, and the
// question is never "was this frame late" but "where did the run spend its
// time, and what would be worth making faster". FrameReport cannot answer that:
// it summarises the last N frames of a fixed pipeline at a fixed rate, and a
// sequence has neither.
//
// So this accumulates by NAME — a CommandKind, a pump phase, a parse phase —
// and reports calls / total / min / max / mean per name. Names are expected to
// be `constexpr std::string_view`s, so the lookup compares the pointer first
// and only falls back to a content compare, which keeps the hot path to a
// handful of instructions.
//
// Recording never allocates: slots are a fixed array and a full instrument
// drops new names rather than growing. `overflowed()` reports that, because an
// instrument that silently stopped recording is worse than one that says so.
//
// Nothing here is computed when the compile level excludes it.
// `Measure<level>` is genuinely empty when compiled out — `sizeof` is 1, not
// 24 — because ALL THREE members are conditional, not just the clock. An
// earlier revision stored the `Instrument*` and the name unconditionally and
// was 24 bytes wide with two stores on a path that is supposed to vanish; the
// waypoint loop constructs one of these per waypoint, so that is not free.
//
// The three idle stand-ins are DISTINCT types on purpose.
// `[[no_unique_address]]` may only overlap members of different types — three
// members of one empty type still need distinct addresses, which costs a byte
// each and quietly makes the object 3 bytes instead of 1.

#include "kernel/support/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace grab::diag
{

    // Sized against the worst case rather than the typical one. The executor
    // tallies three phases per CommandKind (38 kinds -> 114) plus 16 seat verbs
    // plus the pump and load phases: ~139 distinct names if a single document
    // exercised everything. 64 would have overflowed silently on such a
    // document, and `overflowed()` reporting it honestly is a poor substitute
    // for having the slots. A Tally is 48 bytes, so this is ~9 KB per
    // instrument -- paid once per run, not per step.
    inline constexpr std::size_t maxInstrumentSlots = 192U;

    struct Tally
    {
            std::string_view         name;
            std::uint64_t            calls{};
            std::chrono::nanoseconds total{};
            std::chrono::nanoseconds shortest{ std::chrono::nanoseconds::max() };
            std::chrono::nanoseconds longest{};

            [[nodiscard]]
            std::chrono::nanoseconds
            mean() const noexcept
            {
                if( calls == 0U )
                {
                    return std::chrono::nanoseconds::zero();
                }
                return total / static_cast<std::chrono::nanoseconds::rep>( calls );
            }
    };

    // Single-threaded by design: one instrument per run, owned by whoever runs
    // it. Sharing one across threads would need a lock on the hot path, and the
    // pump is single-threaded anyway.
    class Instrument final
    {
        public:

            void
            record( std::string_view         name,
                    std::chrono::nanoseconds elapsed ) noexcept
            {
                Tally* const slot = find_or_add( name );
                if( slot == nullptr )
                {
                    overflowed_ = true;
                    return;
                }
                ++slot->calls;
                slot->total    += elapsed;
                slot->shortest  = std::min( slot->shortest, elapsed );
                slot->longest   = std::max( slot->longest, elapsed );
            }

            [[nodiscard]]
            std::span<const Tally>
            tallies() const noexcept
            {
                return std::span<const Tally>{ slots_.data(), used_ };
            }

            // The sum of every tally, which DOUBLE-COUNTS whenever tallies
            // nest: `enter:input.click` already contains the `seat.button`
            // round trips recorded inside it, and a per-command tally overlaps
            // the pump phase around it. This is a crude denominator, not a run
            // total. A report must either sum one level or say which levels it
            // mixed; presenting this as "time spent" is wrong.
            [[nodiscard]]
            std::chrono::nanoseconds
            total() const noexcept
            {
                std::chrono::nanoseconds sum{};
                for( std::size_t index = 0U; index < used_; ++index )
                {
                    sum += slots_[index].total;
                }
                return sum;
            }

            // True when a name was dropped because every slot was taken. A
            // report built from an overflowed instrument is incomplete and must
            // say so rather than reading as a full accounting.
            [[nodiscard]]
            bool
            overflowed() const noexcept
            {
                return overflowed_;
            }

            void
            reset() noexcept
            {
                used_       = 0U;
                overflowed_ = false;
            }

        private:

            [[nodiscard]]
            Tally*
            find_or_add( std::string_view name ) noexcept
            {
                for( std::size_t index = 0U; index < used_; ++index )
                {
                    // Pointer-equal first: names are constexpr string_views, so
                    // this hits on almost every call and skips the compare.
                    if( slots_[index].name.data() ==
                        name.data() ||
                        slots_[index].name == name )
                    {
                        return &slots_[index];
                    }
                }
                if( used_ == maxInstrumentSlots )
                {
                    return nullptr;
                }
                slots_[used_] = Tally{ .name = name };
                return &slots_[used_++];
            }

            std::array<Tally, maxInstrumentSlots> slots_{};
            std::size_t                           used_{};
            bool                                  overflowed_{};
    };

    // RAII timer that records into an Instrument slot on destruction.
    // Compiled out below `level`: no clock reads, no member, sizeof == 1.
    template<log::Level level>
    class Measure final
    {
        public:

            static constexpr bool enabled = log::enabled( level );

            Measure( Instrument&      into,
                     std::string_view name ) noexcept
            {
                if constexpr( enabled )
                {
                    into_    = &into;
                    name_    = name;
                    started_ = std::chrono::steady_clock::now();
                }
                else
                {
                    // Compiled out: the arguments are not merely unused, they
                    // are not stored, which is what keeps sizeof == 1.
                    ( void )into;
                    ( void )name;
                }
            }

            ~Measure()
            {
                if constexpr( enabled )
                {
                    into_->record( name_, std::chrono::steady_clock::now() - started_ );
                }
            }

            Measure( const Measure& ) = delete;
            Measure&
            operator=( const Measure& ) = delete;
            Measure( Measure&& )        = delete;
            Measure&
            operator=( Measure&& ) = delete;

        private:

            // Three DISTINCT empty types, not one reused three times.
            // [[no_unique_address]] may only overlap members of different
            // types -- two members of the same empty type still require
            // distinct addresses, which silently costs a byte each and makes
            // the compiled-out object 3 bytes rather than 1.
            struct IdleSink
            {
            };

            struct IdleName
            {
            };

            struct IdleClock
            {
            };

            // Every member is conditional, not just the clock. Storing an
            // Instrument* and a string_view unconditionally would leave the
            // compiled-out object 24 bytes wide and put two stores on a path
            // that is supposed to vanish -- and the waypoint loop constructs
            // one of these per waypoint.
            using Started = std::
                conditional_t<enabled, std::chrono::steady_clock::time_point, IdleClock>;
            using Sink = std::conditional_t<enabled, Instrument*, IdleSink>;
            using Name = std::conditional_t<enabled, std::string_view, IdleName>;

            [[no_unique_address]]
            Sink into_{};
            [[no_unique_address]]
            Name name_{};
            [[no_unique_address]]
            Started started_{};
    };

}    // namespace grab::diag
