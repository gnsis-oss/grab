#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/visitor.h -- walk::Visitor concept                            │
// └──────────────────────────────────────────────────────────────────────┘
//
// A Visitor has one method: on(knot) -> void.
// Called once per visited knot, in traversal order.
// The visitor is passed by mutable reference so it can accumulate state.

#include <web/knot.hpp>

namespace walk
{

    // Generic visitor: works with any knot type
    template<typename V, typename Knot>
    concept VisitorOf = requires( V& v, Knot knot ) {
        {
            v.on( knot )
        } -> std::same_as<void>;
    };

    // Legacy concept: hardcoded to web::Knot (used by existing tests)
    template<typename V>
    concept Visitor = VisitorOf<V, web::Knot>;

}    // namespace walk
