#include "grab/command.hpp"
#include "grab/result.hpp"
#include "grab/sequence_types.hpp"
#include "kernel/graph/topological_order.hpp"
#include "kernel/sequence/sequence.hpp"
#include "kernel/support/log.hpp"
#include "kernel/support/log_tags.hpp"
#include "kernel/support/step_diag.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::kernel::sequence
{

    namespace
    {

        // How a step is named in an error message: the author's label when
        // there is one, and its document position otherwise. A document may be
        // entirely unlabelled, so "step 'x'" alone is not enough to locate a
        // fault.
        [[nodiscard]]
        std::string
        step_name( const grab::sequence::Step& step )
        {
            if( !step.label.empty() )
            {
                std::string name{ "'" };
                name.append( step.label );
                name.append( "'" );
                return name;
            }
            std::string name{ "at index " };
            name.append( std::to_string( step.id.index() ) );
            return name;
        }

        void
        fill_graph( const std::vector<grab::sequence::Step>& steps,
                    DependencyGraph&                         graph )
        {
            graph.clear();
            for( const auto& step : steps )
            {
                ( void )graph.add_node( step.id );
            }
            for( const auto& step : steps )
            {
                for( const auto predecessor : step.after )
                {
                    ( void )graph.add_edge( predecessor,
                                            step.id,
                                            grab::sequence::DependencyEdge{} );
                }
            }
        }

    }    // namespace

    namespace detail
    {

        diag::Instrument&
        load_instrument_storage() noexcept
        {
            // One per thread, function-local so nothing depends on static
            // initialisation order. A shared instrument would need a lock on
            // the recording path, and loading is not a shared activity.
            static thread_local diag::Instrument instrument;
            return instrument;
        }

    }    // namespace detail

    const diag::Instrument&
    load_instrument() noexcept
    {
        return detail::load_instrument_storage();
    }

    void
    reset_load_instrument() noexcept
    {
        detail::load_instrument_storage().reset();
    }

    Sequence::Sequence( const Sequence& other ) :
        steps_( other.steps_ ),
        order_( other.order_ ),
        pacing_( other.pacing_ ),
        name_( other.name_ )
    {
        reindex();
    }

    Sequence&
    Sequence::operator=( const Sequence& other )
    {
        if( this != &other )
        {
            steps_  = other.steps_;
            order_  = other.order_;
            pacing_ = other.pacing_;
            name_   = other.name_;
            reindex();
        }
        return *this;
    }

    void
    Sequence::reindex()
    {
        fill_graph( steps_, graph_ );
        labels_.clear();
        for( const auto& step : steps_ )
        {
            if( !step.label.empty() )
            {
                labels_.emplace( step.label, step.id );
            }
        }
    }

    grab::Result<Sequence>
    Sequence::build( std::vector<grab::sequence::Step> steps,
                     grab::sequence::PacingOptions     pacing,
                     std::string                       name )
    {
        diag::Instrument& instrument = detail::load_instrument_storage();
        // Covers the rejecting paths too: a document that fails to build still
        // spent the time, and hiding that would make a slow rejection
        // invisible.
        const diag::Measure<log::Level::Nominal> buildTiming{ instrument, phase::build };

        if( steps.size() > grab::sequence::maxSteps )
        {
            std::string message{ "sequence has " };
            message.append( std::to_string( steps.size() ) );
            message.append( " steps; the maximum is " );
            message.append( std::to_string( grab::sequence::maxSteps ) );
            return grab::fail( grab::ErrorCode::InvalidArgument, message );
        }

        // Identity is positional and assigned here, so it does not have to be
        // written into the serialized document and two loads of the same bytes
        // agree.
        for( std::size_t index = 0U; index < steps.size(); ++index )
        {
            steps[index].id = grab::sequence::StepId{
                static_cast<grab::sequence::StepId::Half>( index ),
                grab::sequence::StepId::firstGeneration
            };
        }

        Sequence built;
        built.steps_  = std::move( steps );
        built.pacing_ = pacing;
        built.name_   = std::move( name );

        {
            // Tallied under the same name as the loader's label pre-pass, so
            // `load.labels` prices label indexing wherever it happens; a load
            // through parse() therefore shows two calls, not one.
            const diag::Measure<log::Level::Nominal> labelTiming{
                instrument,
                phase::labels
            };
            for( const auto& step : built.steps_ )
            {
                if( step.label.empty() )
                {
                    continue;
                }
                if( built.labels_.contains( step.label ) )
                {
                    std::string message{ "duplicate step label '" };
                    message.append( step.label );
                    message.append( "'" );
                    return grab::fail( grab::ErrorCode::InvalidArgument, message );
                }
                built.labels_.emplace( step.label, step.id );
            }
        }

        {
            // Node insertion plus one add_edge per declared dependency: the
            // O(V + E) half of a build, and the half that grows with how
            // BRANCHED a document is rather than with how long it is.
            const diag::Measure<log::Level::Nominal> graphTiming{
                instrument,
                phase::graph
            };

            for( const auto& step : built.steps_ )
            {
                ( void )built.graph_.add_node( step.id );
            }

            for( const auto& step : built.steps_ )
            {
                for( const auto predecessor : step.after )
                {
                    if( !built.graph_.contains_node( predecessor ) )
                    {
                        std::string message{ "step " };
                        message.append( step_name( step ) );
                        message.append( " depends on step index " );
                        message.append( std::to_string( predecessor.index() ) );
                        message.append( ", which does not exist" );
                        return grab::fail( grab::ErrorCode::InvalidArgument, message );
                    }
                    if( predecessor == step.id )
                    {
                        std::string message{ "step " };
                        message.append( step_name( step ) );
                        message.append( " depends on itself" );
                        return grab::fail( grab::ErrorCode::InvalidArgument, message );
                    }
                    // add_edge also returns false for a duplicate edge, and
                    // the edge simply never enters the graph, so the
                    // topological sort could never see it. Treat every false
                    // as a rejection.
                    if( !built.graph_.add_edge( predecessor,
                                                step.id,
                                                grab::sequence::DependencyEdge{} ) )
                    {
                        std::string message{ "step " };
                        message.append( step_name( step ) );
                        message.append( " lists step index " );
                        message.append( std::to_string( predecessor.index() ) );
                        message.append( " as a dependency twice" );
                        return grab::fail( grab::ErrorCode::InvalidArgument, message );
                    }
                }
            }
        }

        {
            // Kahn over the whole graph. Timed apart from construction
            // because "the topological sort is most of a load" and "building
            // the adjacency lists is" are different findings with different
            // fixes.
            const diag::Measure<log::Level::Nominal> orderTiming{
                instrument,
                phase::topology
            };

            auto order = grab::kernel::topological_order( built.graph_ );
            if( !order.has_value() )
            {
                return grab::fail( grab::ErrorCode::InvalidArgument,
                                   "sequence has a dependency cycle" );
            }
            built.order_ = std::move( *order );
        }

        log::nominal(
            [&built]( auto& event )
            {
                event.tag( log::tags::sequence )
                    .value( "built", built.name_ )
                    .value( "steps", built.steps_.size() )
                    .value( "edges", built.graph_.edge_count() )
                    .value( "pacing",
                            grab::sequence::pacing_mode_name( built.pacing_.mode ) );
            }
        );

        return built;
    }

    std::span<const grab::sequence::Step>
    Sequence::steps() const noexcept
    {
        return std::span<const grab::sequence::Step>{ steps_ };
    }

    std::span<const grab::sequence::StepId>
    Sequence::order() const noexcept
    {
        return std::span<const grab::sequence::StepId>{ order_ };
    }

    const grab::sequence::Step*
    Sequence::find( grab::sequence::StepId id ) const
    {
        const auto index = static_cast<std::size_t>( id.index() );
        if( id.is_nil() || index >= steps_.size() )
        {
            return nullptr;
        }
        const auto& step = steps_[index];
        if( step.id != id )
        {
            return nullptr;
        }
        return &step;
    }

    std::optional<grab::sequence::StepId>
    Sequence::resolve_label( std::string_view label ) const
    {
        const auto found = labels_.find( label );
        if( found == labels_.end() )
        {
            return std::nullopt;
        }
        return found->second;
    }

    std::vector<grab::sequence::StepId>
    Sequence::ancestors_of( grab::sequence::StepId id ) const
    {
        std::vector<grab::sequence::StepId> found;
        if( !graph_.contains_node( id ) )
        {
            return found;
        }

        std::vector<bool>                   seen( steps_.size(), false );
        std::vector<grab::sequence::StepId> pending{ id };
        for( std::size_t cursor = 0U; cursor < pending.size(); ++cursor )
        {
            const auto node = pending[cursor];
            // in_edges() reports the edge's *source* in `target`.
            for( const auto& edge : graph_.in_edges( node ) )
            {
                const auto index = static_cast<std::size_t>( edge.target.index() );
                if( index >= seen.size() || seen[index] )
                {
                    continue;
                }
                seen[index] = true;
                found.push_back( edge.target );
                pending.push_back( edge.target );
            }
        }

        std::ranges::sort( found );
        return found;
    }

    grab::sequence::PacingOptions
    Sequence::pacing() const noexcept
    {
        return pacing_;
    }

    std::string_view
    Sequence::name() const noexcept
    {
        return name_;
    }

    const DependencyGraph&
    Sequence::graph() const noexcept
    {
        return graph_;
    }

}    // namespace grab::kernel::sequence
