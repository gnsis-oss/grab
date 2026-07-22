#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/drag.hpp"
#include "grab/geometry/point.hpp"
#include "grab/locator.hpp"
#include "grab/query.hpp"
#include "grab/trace.hpp"

#include <chrono>
#include <cstdint>
#include <stop_token>
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
            std::stop_token stop{};    // NOLINT(readability-redundant-member-init)
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

    struct Drag
    {
            ActionTarget             target;
            grab::geometry::Point    from;
            grab::geometry::Point    to;
            grab::input::DragOptions options;
    };

    struct PressKey
    {
            ActionTarget target;
            std::string  key_name;
    };

    struct Activate
    {
            ActionTarget target;
    };

    using Action = std::variant<Click, TypeText, Drag, PressKey, Activate>;

}    // namespace grab
