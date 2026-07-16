#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/query.hpp"
#include "grab/result.hpp"
#include "grab/trace.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::spi
{

    enum class RouteKind : std::uint8_t
    {
        Semantic,
        Physical,
    };

    enum class RouteFidelity : std::uint8_t
    {
        Exact,
        Lossless,
        Approximate,
        BestEffort,
    };

    enum class RouteLatencyClass : std::uint8_t
    {
        Immediate,
        Interactive,
        Deferred,
    };

    struct RouteConstraint
    {
            std::string_view name;
            std::string_view detail;
    };

    struct RouteDescriptor
    {
            std::string_view  name;
            RouteKind         kind{ RouteKind::Physical };
            RouteFidelity     fidelity{ RouteFidelity::BestEffort };
            RouteLatencyClass latency_class{ RouteLatencyClass::Deferred };
            std::span<const RouteConstraint>
                constraints{};    // NOLINT(readability-redundant-member-init)
    };

    enum class ActionVerb : std::uint8_t
    {
        Click,
        TypeText,
        Drag,
        PressKey,
        Activate,
    };

    struct ActionRequest
    {
            ActionVerb               verb{ ActionVerb::Click };
            Match                    target{};
            std::string              text{};
            grab::geometry::Point    drag_from{};
            grab::geometry::Point    drag_to{};
            grab::input::DragOptions drag_options{};
            std::string              key_name{};
    };

    class RouteReservation
    {
        public:

            RouteReservation()                          = default;
            virtual ~RouteReservation()                 = default;
            RouteReservation( const RouteReservation& ) = delete;
            RouteReservation&
            operator=( const RouteReservation& )   = delete;
            RouteReservation( RouteReservation&& ) = delete;
            RouteReservation&
            operator=( RouteReservation&& ) = delete;

            [[nodiscard]]
            virtual std::span<const std::string_view>
            barriers() const noexcept = 0;

            [[nodiscard]]
            virtual Result<void>
            arm_barrier( std::string_view        barrier,
                         const OperationContext& context ) = 0;

            // ErrorCode::PossiblyCommitted is the only error that may mean the
            // dispatch crossed the input commit boundary.
            [[nodiscard]]
            virtual Result<void>
            commit( const OperationContext& context ) = 0;

            [[nodiscard]]
            virtual Result<std::vector<BarrierOutcome>>
            settle( const OperationContext& context ) = 0;

            [[nodiscard]]
            virtual Result<void>
            verify( const OperationContext& context ) = 0;
    };

    class ActionRoute
    {
        public:

            ActionRoute()                     = default;
            virtual ~ActionRoute()            = default;
            ActionRoute( const ActionRoute& ) = delete;
            ActionRoute&
            operator=( const ActionRoute& ) = delete;
            ActionRoute( ActionRoute&& )    = delete;
            ActionRoute&
            operator=( ActionRoute&& ) = delete;

            [[nodiscard]]
            virtual Result<std::unique_ptr<RouteReservation>>
            reserve( const ActionRequest&    action,
                     const OperationContext& context ) = 0;
    };

    class InputSeat
    {
        public:

            InputSeat()                   = default;
            virtual ~InputSeat()          = default;
            InputSeat( const InputSeat& ) = delete;
            InputSeat&
            operator=( const InputSeat& ) = delete;
            InputSeat( InputSeat&& )      = delete;
            InputSeat&
            operator=( InputSeat&& ) = delete;

            [[nodiscard]]
            virtual Result<NeutralizationOutcome>
            neutralize( const OperationContext& context ) = 0;
    };

}    // namespace grab::spi
