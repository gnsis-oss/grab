#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/trace.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace grab
{

    enum class RoutePolicy : std::uint8_t
    {
        PreferSemantic,
        SemanticOnly,
        PhysicalOnly,
    };

    struct ActionOptions
    {
            std::chrono::nanoseconds deadline{ std::chrono::seconds{ 5 } };
            Cardinality              cardinality{ Cardinality::ExactlyOne };
            RoutePolicy              routing{ RoutePolicy::PreferSemantic };
            RetryClass               retry{ RetryClass::ResolveOnly };
            bool                     force{};
    };

    using ActionTarget = std::variant<Locator, Match>;

    struct Click
    {
            ActionTarget target;
    };

    struct TypeText
    {
            ActionTarget target;
            std::string  text;
    };

    using Action = std::variant<Click, TypeText>;

}    // namespace grab
