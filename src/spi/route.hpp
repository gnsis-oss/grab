#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include <cstdint>
#include <span>
#include <string_view>

namespace grab::spi
{

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
            RouteFidelity     fidelity{ RouteFidelity::BestEffort };
            RouteLatencyClass latency_class{ RouteLatencyClass::Deferred };
            std::span<const RouteConstraint>
                constraints{};    // NOLINT(readability-redundant-member-init)
    };

}    // namespace grab::spi
