#pragma once
// ┌──────────────────────────────────────────────────────────────────────┐
// │  walk/each_tie.h -- walk::each_tie recursive edge-DFS               │
// └──────────────────────────────────────────────────────────────────────┘
//
// Recursive depth-first walk over edges of a weighted web::Web.
// Intended for tree-shaped graphs (JSON webs, PDF webs) where
// the full visitor pattern does not map cleanly to serialization.
//
// Visitor hooks (all optional, detected via if constexpr + requires):
//   on_knot(knot, depth)                       -- node entered
//   on_tie(parent, Neighbor<Edge>, depth)      -- edge being processed
//   on_done(knot, depth)                       -- subtree finished
//   skip(parent, Neighbor<Edge>, depth) -> bool -- return true to skip recursion
//
// Unlike walk::delve, this is recursive so on_done truly fires
// after the entire subtree is processed (correct for bracket matching).
// Unlike walk::delve, no cycle detection — intended for DAGs/trees.

#include <type_traits>
#include <web/knot.hpp>
#include <web/web.hpp>

namespace walk
{

    namespace detail
    {

        template<typename Policy,
                 typename Edge,
                 typename V>
        void
        each_tie_impl( const web::Web<Policy,
                                      Edge>& graph,
                       web::Knot             knot,
                       V&                    visitor,
                       int                   depth )
        {
            static_assert( !std::is_void_v<Edge>, "each_tie requires weighted edges" );

            if constexpr( requires { visitor.on_knot( knot, depth ); } )
            {
                visitor.on_knot( knot, depth );
            }

            auto edges = graph.out( knot );
            for( const auto& edge : edges )
            {
                if constexpr( requires { visitor.on_tie( knot, edge, depth ); } )
                {
                    visitor.on_tie( knot, edge, depth );
                }

                bool descend = true;
                if constexpr( requires {
                                  {
                                      visitor.skip( knot, edge, depth )
                                  } -> std::same_as<bool>;
                              } )
                {
                    descend = !visitor.skip( knot, edge, depth );
                }
                if( descend )
                {
                    each_tie_impl( graph, edge.target, visitor, depth + 1 );
                }
            }

            if constexpr( requires { visitor.on_done( knot, depth ); } )
            {
                visitor.on_done( knot, depth );
            }
        }

    }    // namespace detail

    template<typename Policy,
             typename Edge,
             typename V>
    void
    each_tie( const web::Web<Policy,
                             Edge>& graph,
              web::Knot             start,
              V&                    visitor )
    {
        static_assert( !std::is_void_v<Edge>, "each_tie requires weighted edges" );
        if( !graph.has( start ) )
        {
            return;
        }
        detail::each_tie_impl( graph, start, visitor, 0 );
    }

}    // namespace walk
