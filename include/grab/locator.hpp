#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"
#include "grab/ui.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace grab
{

    enum class Cardinality : std::uint8_t
    {
        ExactlyOne,
        First,
        All,
    };

    enum class BoundaryPolicy : std::uint8_t
    {
        SameTree,
        SameProcess,
        CrossEmbeds,
    };

    enum class ConsistencyMode : std::uint8_t
    {
        Live,
        Revisioned,
        Pinned,
    };

    struct LocatorLimits
    {
            static constexpr std::size_t default_max_nodes = 8'192U;
            std::size_t                  max_nodes{ default_max_nodes };
    };

    class Locator;

    namespace kernel::query::detail
    {

        struct LocatorPlan;

        [[nodiscard]]
        Locator
        make_locator( std::shared_ptr<const LocatorPlan> plan,
                      BoundaryPolicy                     boundary,
                      ConsistencyMode                    consistency );

        [[nodiscard]]
        const LocatorPlan&
        plan_of( const Locator& locator ) noexcept;

        [[nodiscard]]
        std::size_t
        plan_node_count( const Locator& locator ) noexcept;

    }    // namespace kernel::query::detail

    class Locator
    {
        public:

            Locator();
            ~Locator()                = default;
            Locator( const Locator& ) = default;
            Locator&
            operator=( const Locator& ) = default;
            Locator( Locator&& other ) noexcept;
            Locator&
            operator=( Locator&& other ) noexcept;

            [[nodiscard]]
            Locator
            and_( Locator predicate ) const;

            [[nodiscard]]
            Locator
            with_boundary( BoundaryPolicy boundary ) const;

            [[nodiscard]]
            Locator
            with_consistency( ConsistencyMode consistency ) const;

            [[nodiscard]]
            BoundaryPolicy
            boundary() const noexcept;

            [[nodiscard]]
            ConsistencyMode
            consistency() const noexcept;

            [[nodiscard]]
            std::string
            to_string() const;

            [[nodiscard]]
            static Result<Locator>
            from_string( std::string_view serialized );

            friend bool
            operator==( const Locator& left,
                        const Locator& right );

        private:

            Locator( std::shared_ptr<const kernel::query::detail::LocatorPlan> plan,
                     BoundaryPolicy                                            boundary,
                     ConsistencyMode consistency ) noexcept;

            std::shared_ptr<const kernel::query::detail::LocatorPlan> plan_;
            BoundaryPolicy  boundary_{ BoundaryPolicy::SameTree };
            ConsistencyMode consistency_{ ConsistencyMode::Live };

            friend Locator
            kernel::query::detail::make_locator(
                std::shared_ptr<const kernel::query::detail::LocatorPlan> plan,
                BoundaryPolicy                                            boundary,
                ConsistencyMode                                           consistency
            );

            friend const kernel::query::detail::LocatorPlan&
            kernel::query::detail::plan_of( const Locator& locator ) noexcept;
    };

    namespace sel
    {

        [[nodiscard]]
        Locator
        role( RoleId value );

        [[nodiscard]]
        Locator
        state( NodeState value );

        [[nodiscard]]
        Locator
        state( std::uint32_t mask );

        [[nodiscard]]
        Locator
        property( PropertyId    property_id,
                  PropertyValue value );

        [[nodiscard]]
        Locator
        accessible_name( std::string_view value );

        [[nodiscard]]
        Locator
        text( std::string_view value );

        [[nodiscard]]
        Locator
        all( std::initializer_list<Locator> predicates );

        [[nodiscard]]
        Locator
        any( std::initializer_list<Locator> predicates );

        [[nodiscard]]
        Locator
        not_( Locator predicate );

        [[nodiscard]]
        Locator
        child_of( Locator parent );

        [[nodiscard]]
        Locator
        descendant_of( Locator ancestor );

        [[nodiscard]]
        Locator
        ancestor_of( Locator descendant );

        [[nodiscard]]
        Locator
        related( RelationId relation,
                 Locator    target );

        [[nodiscard]]
        Locator
        related_reverse( RelationId relation,
                         Locator    source );

    }    // namespace sel

}    // namespace grab

namespace std
{

    template<>
    struct hash<grab::Locator>
    {
            [[nodiscard]]
            size_t
            operator()( const grab::Locator& locator ) const;
    };

}    // namespace std
