#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

// The immutable half of the document/run split (weft's TreeDef to Player's
// TreeInstance). A Sequence holds no run state at all: every mutable thing
// lives in Player, so reverting a run is dropping the Player rather than
// rewinding the document.

#include "grab/command.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/graph/adjacency_graph.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace grab::kernel::sequence
{

    namespace detail
    {

        // Heterogeneous lookup, so resolve_label( string_view ) does not have
        // to allocate a std::string to ask a question.
        struct LabelHash
        {
                using is_transparent = void;

                [[nodiscard]]
                std::size_t
                operator()( std::string_view text ) const noexcept
                {
                    return std::hash<std::string_view>{}( text );
                }
        };

    }    // namespace detail

    using DependencyGraph = grab::kernel::AdjacencyGraph<grab::sequence::StepId,
                                                         grab::sequence::DependencyEdge>;

    class Sequence final
    {
        public:

            using LabelMap = std::unordered_map<std::string,
                                                grab::sequence::StepId,
                                                detail::LabelHash,
                                                std::equal_to<>>;

            // Identity is assigned HERE, positionally: step i becomes
            // StepId{ i, firstGeneration }. Callers filling `after` therefore
            // address predecessors by document position, and a document that
            // loads twice yields the same ids both times.
            //
            // Rejects, each with its own message: more than maxSteps steps; a
            // duplicate label; an `after` entry naming no step; a
            // self-dependency; a repeated `after` entry; and a cycle.
            [[nodiscard]]
            static grab::Result<Sequence>
            build( std::vector<grab::sequence::Step> steps,
                   grab::sequence::PacingOptions     pacing,
                   std::string                       name );

            Sequence()  = default;
            ~Sequence() = default;

            // AdjacencyGraph's copy constructor is deleted on purpose, so this
            // one rebuilds the graph from steps_ instead. O(E), and copies are
            // rare.
            Sequence( const Sequence& other );
            Sequence&
            operator=( const Sequence& other );
            Sequence( Sequence&& ) noexcept = default;
            Sequence&
            operator=( Sequence&& ) noexcept = default;

            [[nodiscard]]
            std::span<const grab::sequence::Step>
            steps() const noexcept;

            // One arbitrary Kahn linearization, cached at build. It is NOT a
            // schedule and NOT an ancestry order: skipping a prefix of it would
            // skip unrelated parallel branches.
            [[nodiscard]]
            std::span<const grab::sequence::StepId>
            order() const noexcept;

            [[nodiscard]]
            const grab::sequence::Step*
            find( grab::sequence::StepId id ) const;

            [[nodiscard]]
            std::optional<grab::sequence::StepId>
            resolve_label( std::string_view label ) const;

            // Every step that can reach `id`, ascending, excluding `id`
            // itself. goto_step is defined over this rather than over order().
            [[nodiscard]]
            std::vector<grab::sequence::StepId>
            ancestors_of( grab::sequence::StepId id ) const;

            [[nodiscard]]
            grab::sequence::PacingOptions
            pacing() const noexcept;

            [[nodiscard]]
            std::string_view
            name() const noexcept;

            [[nodiscard]]
            const DependencyGraph&
            graph() const noexcept;

        private:

            // Rebuilds graph_ and labels_ from steps_. Shared by build() and
            // the copy constructor; assumes steps_ already validated.
            void
                                                reindex();

            std::vector<grab::sequence::Step>   steps_;
            DependencyGraph                     graph_;
            std::vector<grab::sequence::StepId> order_;
            LabelMap                            labels_;
            grab::sequence::PacingOptions       pacing_{};
            std::string                         name_;
    };

}    // namespace grab::kernel::sequence
