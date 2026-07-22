#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  web/trait.h -- web::Trait<Edge>, Weighted concept                   │
// └──────────────────────────────────────────────────────────────────────┘
//
// Trait<Edge> provides weight extraction for typed edges. User specializes
// Trait<Edge> with a static weight() method. The Weighted concept validates
// the specialization at compile time.

#include <concepts>

namespace web
{

    // ── Primary template -- user must specialize ────────────────────────────

    template<typename Edge>
    struct Trait;

    // ── Weighted concept ────────────────────────────────────────────────────

    template<typename Edge>
    concept Weighted = requires( const Edge& e ) {
        {
            Trait<Edge>::weight( e )
        } -> std::totally_ordered;
    };

    // ── Labeled concept ─────────────────────────────────────────────────────
    //
    // Transitions carry semantic labels (e.g. action names in a state space).
    // Orthogonal to Weighted — an edge can be both.

    template<typename Edge>
    concept Labeled = requires( const Edge& e ) {
        {
            Trait<Edge>::label( e )
        } -> std::equality_comparable;
    };

}    // namespace web
