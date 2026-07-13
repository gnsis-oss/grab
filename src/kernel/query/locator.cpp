#include "grab/locator.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "grab/ui.hpp"
#include "kernel/query/locator_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>    // IWYU pragma: keep
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace grab
{

    namespace
    {

        namespace plan_detail = kernel::query::detail;

        // The distro multi-header exposes ordered_json but the include-cleaner
        // database does not associate the alias with its umbrella header.
        using Json    = nlohmann::ordered_json;    // NOLINT(misc-include-cleaner)
        using Plan    = plan_detail::LocatorPlan;
        using PlanPtr = plan_detail::LocatorPlanPtr;
        using Op      = plan_detail::LocatorOp;

        constexpr std::uint64_t locatorVersion = 1U;

        [[nodiscard]]
        PlanPtr
        make_plan( Op operation )
        {
            auto plan = std::make_shared<Plan>();
            plan->op  = operation;
            return plan;
        }

        [[nodiscard]]
        PlanPtr
        copy_plan( const Locator& locator )
        {
            return std::make_shared<Plan>( plan_detail::plan_of( locator ) );
        }

        void
        append_predicate( std::vector<PlanPtr>& predicates,
                          PlanPtr               predicate,
                          Op                    operation )
        {
            if( predicate->op == operation )
            {
                predicates.insert( predicates.end(),
                                   predicate->children.begin(),
                                   predicate->children.end() );
                return;
            }
            predicates.push_back( std::move( predicate ) );
        }

        [[nodiscard]]
        std::string_view
        boundary_name( BoundaryPolicy boundary ) noexcept
        {
            switch( boundary )
            {
                case BoundaryPolicy::SameTree :
                    return "same_tree";
                case BoundaryPolicy::SameProcess :
                    return "same_process";
                case BoundaryPolicy::CrossEmbeds :
                    return "cross_embeds";
                default :
                    return "same_tree";
            }
        }

        [[nodiscard]]
        std::string_view
        consistency_name( ConsistencyMode consistency ) noexcept
        {
            switch( consistency )
            {
                case ConsistencyMode::Live :
                    return "live";
                case ConsistencyMode::Revisioned :
                    return "revisioned";
                case ConsistencyMode::Pinned :
                    return "pinned";
                default :
                    return "live";
            }
        }

        [[nodiscard]]
        Json
        encoded_number( double value )
        {
            Json encoded;
            if( value == 0.0 )
            {
                encoded = 0.0;
                return encoded;
            }
            if( std::isfinite( value ) )
            {
                encoded = value;
                return encoded;
            }
            if( std::isnan( value ) )
            {
                encoded = "nan";
                return encoded;
            }
            encoded = std::signbit( value ) ? "-infinity" : "+infinity";
            return encoded;
        }

        // nlohmann's object insertion API intentionally uses operator[].
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        void
        append_property_value( Json&                object,
                               const PropertyValue& value )
        {
            if( std::holds_alternative<std::monostate>( value ) )
            {
                object["type"]  = "null";
                object["value"] = nullptr;
                return;
            }
            if( const auto* boolean = std::get_if<bool>( &value ) )
            {
                object["type"]  = "bool";
                object["value"] = *boolean;
                return;
            }
            if( const auto* integer = std::get_if<std::int64_t>( &value ) )
            {
                object["type"]  = "integer";
                object["value"] = *integer;
                return;
            }
            if( const auto* number = std::get_if<double>( &value ) )
            {
                object["type"]  = "number";
                object["value"] = encoded_number( *number );
                return;
            }
            if( const auto* text_value = std::get_if<std::string>( &value ) )
            {
                object["type"]  = "text";
                object["value"] = *text_value;
                return;
            }

            const auto& rectangle   = std::get<SpaceRect>( value );
            object["type"]          = "rect";
            Json rectangle_json     = Json::object();
            rectangle_json["x"]     = encoded_number( rectangle.x );
            rectangle_json["y"]     = encoded_number( rectangle.y );
            rectangle_json["w"]     = encoded_number( rectangle.w );
            rectangle_json["h"]     = encoded_number( rectangle.h );
            rectangle_json["space"] = rectangle.space.value;
            object["value"]         = std::move( rectangle_json );
        }

        [[nodiscard]]
        Json
        expression_json( const Plan& plan )
        {
            Json object = Json::object();
            switch( plan.op )
            {
                case Op::MatchAll :
                    object["op"] = "match_all";
                    break;
                case Op::MatchNone :
                    object["op"] = "match_none";
                    break;
                case Op::Role :
                    object["op"]    = "role";
                    object["value"] = plan.role.value;
                    break;
                case Op::State :
                    object["op"]    = "state";
                    object["value"] = static_cast<std::uint32_t>( plan.state );
                    break;
                case Op::Property :
                    object["op"]       = "property";
                    object["property"] = plan.property.value;
                    append_property_value( object, plan.value );
                    break;
                case Op::AccessibleName :
                    object["op"]    = "accessible_name";
                    object["value"] = plan.text;
                    break;
                case Op::Text :
                    object["op"]    = "text";
                    object["value"] = plan.text;
                    break;
                case Op::All :
                case Op::Any :
                    {
                        object["op"]  = plan.op == Op::All ? "all" : "any";
                        Json children = Json::array();
                        for( const auto& child : plan.children )
                        {
                            children.push_back( expression_json( *child ) );
                        }
                        object["children"] = std::move( children );
                        break;
                    }
                case Op::Not :
                    object["op"]    = "not";
                    object["child"] = expression_json( *plan.children.front() );
                    break;
                case Op::ChildOf :
                    object["op"]    = "child_of";
                    object["child"] = expression_json( *plan.children.front() );
                    break;
                case Op::DescendantOf :
                    object["op"]    = "descendant_of";
                    object["child"] = expression_json( *plan.children.front() );
                    break;
                case Op::AncestorOf :
                    object["op"]    = "ancestor_of";
                    object["child"] = expression_json( *plan.children.front() );
                    break;
                case Op::Related :
                    object["op"]       = "related";
                    object["relation"] = plan.relation.value;
                    object["child"]    = expression_json( *plan.children.front() );
                    break;
                case Op::RelatedReverse :
                    object["op"]       = "related_reverse";
                    object["relation"] = plan.relation.value;
                    object["child"]    = expression_json( *plan.children.front() );
                    break;
                default :
                    object["op"] = "match_none";
                    break;
            }
            return object;
        }

        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

        [[nodiscard]]
        std::unexpected<Error>
        invalid_locator( std::string message )
        {
            return fail( ErrorCode::InvalidArgument, std::move( message ) );
        }

        [[nodiscard]]
        std::string
        syntax_error_message( std::string_view serialized,
                              std::size_t      byte )
        {
            const std::size_t offset =
                byte == 0U ? 0U : std::min( byte - 1U, serialized.size() );
            const std::size_t previous_newline =
                offset == 0U ? std::string_view::npos
                             : serialized.rfind( '\n', offset - 1U );
            const std::size_t line_start =
                previous_newline == std::string_view::npos ? 0U : previous_newline + 1U;
            const std::size_t found_end = serialized.find( '\n', offset );
            const std::size_t line_end =
                found_end == std::string_view::npos ? serialized.size() : found_end;

            std::ostringstream message;
            message << "invalid locator JSON syntax at byte " << byte << '\n'
                    << serialized.substr( line_start, line_end - line_start ) << '\n'
                    << std::string( offset - line_start, ' ' ) << '^';
            return message.str();
        }

        [[nodiscard]]
        Result<const Json*>
        required_member( const Json&      object,
                         std::string_view name,
                         std::string_view path )
        {
            if( !object.is_object() )
            {
                return invalid_locator( std::string{ path } + " must be an object" );
            }
            const auto found = object.find( std::string{ name } );
            if( found == object.end() )
            {
                return invalid_locator(
                    std::string{ path } + " is missing '" + std::string{ name } + "'"
                );
            }
            return std::addressof( *found );
        }

        [[nodiscard]]
        Result<std::uint32_t>
        unsigned_32( const Json&      value,
                     std::string_view path )
        {
            std::uint64_t number = 0U;
            if( value.is_number_unsigned() )
            {
                number = value.get<std::uint64_t>();
            }
            else if( value.is_number_integer() )
            {
                const auto signed_number = value.get<std::int64_t>();
                if( signed_number < 0 )
                {
                    return invalid_locator( std::string{ path } +
                                            " must be an unsigned integer" );
                }
                number = static_cast<std::uint64_t>( signed_number );
            }
            else
            {
                return invalid_locator( std::string{ path } +
                                        " must be an unsigned integer" );
            }

            if( number > std::numeric_limits<std::uint32_t>::max() )
            {
                return invalid_locator( std::string{ path } + " is out of range" );
            }
            return static_cast<std::uint32_t>( number );
        }

        [[nodiscard]]
        Result<std::string>
        json_string( const Json&      value,
                     std::string_view path )
        {
            if( !value.is_string() )
            {
                return invalid_locator( std::string{ path } + " must be a string" );
            }
            return value.get<std::string>();
        }

        [[nodiscard]]
        Result<double>
        json_number( const Json&      value,
                     std::string_view path )
        {
            if( value.is_number() )
            {
                const auto number = value.get<double>();
                if( !std::isfinite( number ) )
                {
                    return invalid_locator( std::string{ path } +
                                            " numeric value is out of range" );
                }
                return number == 0.0 ? 0.0 : number;
            }
            if( value.is_string() )
            {
                const auto token = value.get<std::string>();
                if( token == "nan" )
                {
                    return std::numeric_limits<double>::quiet_NaN();
                }
                if( token == "+infinity" )
                {
                    return std::numeric_limits<double>::infinity();
                }
                if( token == "-infinity" )
                {
                    return -std::numeric_limits<double>::infinity();
                }
            }
            return invalid_locator( std::string{ path } + " must be a number" );
        }

        [[nodiscard]]
        Result<BoundaryPolicy>
        parse_boundary( const Json& value )
        {
            const auto name = json_string( value, "$.boundary" );
            if( !name )
            {
                return std::unexpected( name.error() );
            }
            if( *name == "same_tree" )
            {
                return BoundaryPolicy::SameTree;
            }
            if( *name == "same_process" )
            {
                return BoundaryPolicy::SameProcess;
            }
            if( *name == "cross_embeds" )
            {
                return BoundaryPolicy::CrossEmbeds;
            }
            return invalid_locator( "$.boundary has an unknown value" );
        }

        [[nodiscard]]
        Result<ConsistencyMode>
        parse_consistency( const Json& value )
        {
            const auto name = json_string( value, "$.consistency" );
            if( !name )
            {
                return std::unexpected( name.error() );
            }
            if( *name == "live" )
            {
                return ConsistencyMode::Live;
            }
            if( *name == "revisioned" )
            {
                return ConsistencyMode::Revisioned;
            }
            if( *name == "pinned" )
            {
                return ConsistencyMode::Pinned;
            }
            return invalid_locator( "$.consistency has an unknown value" );
        }

        [[nodiscard]]
        Result<PropertyValue>
        parse_property_value( const Json&      object,
                              std::string_view path )
        {
            const auto type_member = required_member( object, "type", path );
            if( !type_member )
            {
                return std::unexpected( type_member.error() );
            }
            const auto value_member = required_member( object, "value", path );
            if( !value_member )
            {
                return std::unexpected( value_member.error() );
            }
            const auto type =
                json_string( **type_member, std::string{ path } + ".type" );
            if( !type )
            {
                return std::unexpected( type.error() );
            }

            const Json& value = **value_member;
            if( *type == "null" )
            {
                if( !value.is_null() )
                {
                    return invalid_locator( std::string{ path } +
                                            ".value must be null" );
                }
                return PropertyValue{ std::monostate{} };
            }
            if( *type == "bool" )
            {
                if( !value.is_boolean() )
                {
                    return invalid_locator( std::string{ path } +
                                            ".value must be a boolean" );
                }
                return PropertyValue{ value.get<bool>() };
            }
            if( *type == "integer" )
            {
                if( value.is_number_unsigned() )
                {
                    const auto number = value.get<std::uint64_t>();
                    if( number > static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max()
                                 ) )
                    {
                        return invalid_locator( std::string{ path } +
                                                ".value is out of range" );
                    }
                    return PropertyValue{ static_cast<std::int64_t>( number ) };
                }
                if( !value.is_number_integer() )
                {
                    return invalid_locator( std::string{ path } +
                                            ".value must be an integer" );
                }
                return PropertyValue{ value.get<std::int64_t>() };
            }
            if( *type == "number" )
            {
                auto number = json_number( value, std::string{ path } + ".value" );
                if( !number )
                {
                    return std::unexpected( number.error() );
                }
                return PropertyValue{ *number };
            }
            if( *type == "text" )
            {
                const auto text_value =
                    json_string( value, std::string{ path } + ".value" );
                if( !text_value )
                {
                    return std::unexpected( text_value.error() );
                }
                return PropertyValue{ *text_value };
            }
            if( *type != "rect" || !value.is_object() )
            {
                return invalid_locator( std::string{ path } +
                                        " has an unknown property type" );
            }

            const auto x     = required_member( value, "x", path );
            const auto y     = required_member( value, "y", path );
            const auto w     = required_member( value, "w", path );
            const auto h     = required_member( value, "h", path );
            const auto space = required_member( value, "space", path );
            if( !x || !y || !w || !h || !space )
            {
                return invalid_locator( std::string{ path } +
                                        ".value is missing a rectangle field" );
            }
            auto x_value = json_number( **x, std::string{ path } + ".value.x" );
            auto y_value = json_number( **y, std::string{ path } + ".value.y" );
            auto w_value = json_number( **w, std::string{ path } + ".value.w" );
            auto h_value = json_number( **h, std::string{ path } + ".value.h" );
            if( !x_value || !y_value || !w_value || !h_value )
            {
                return invalid_locator( std::string{ path } +
                                        ".value rectangle fields must be numbers" );
            }
            const auto space_id =
                unsigned_32( **space, std::string{ path } + ".value.space" );
            if( !space_id )
            {
                return std::unexpected( space_id.error() );
            }
            return PropertyValue{
                SpaceRect{
                          .x     = *x_value,
                          .y     = *y_value,
                          .w     = *w_value,
                          .h     = *h_value,
                          .space = CoordinateSpaceId{ *space_id },
                          }
            };
        }

        [[nodiscard]]
        Result<PlanPtr>
        parse_expression( const Json&  object,
                          std::size_t& node_count );

        [[nodiscard]]
        Result<PlanPtr>
        parse_unary( const Json&  object,
                     Op           operation,
                     std::size_t& node_count )
        {
            const auto child_member = required_member( object, "child", "$.expr" );
            if( !child_member )
            {
                return std::unexpected( child_member.error() );
            }
            auto child = parse_expression( **child_member, node_count );
            if( !child )
            {
                return std::unexpected( child.error() );
            }
            auto plan = std::make_shared<Plan>();
            plan->op  = operation;
            plan->children.push_back( std::move( *child ) );
            return plan;
        }

        [[nodiscard]]
        Result<PlanPtr>
        parse_relation( const Json&  object,
                        Op           operation,
                        std::size_t& node_count )
        {
            const auto relation_member = required_member( object, "relation", "$.expr" );
            if( !relation_member )
            {
                return std::unexpected( relation_member.error() );
            }
            const auto relation = unsigned_32( **relation_member, "$.expr.relation" );
            if( !relation )
            {
                return std::unexpected( relation.error() );
            }
            auto parsed = parse_unary( object, operation, node_count );
            if( !parsed )
            {
                return std::unexpected( parsed.error() );
            }
            auto plan      = std::make_shared<Plan>( **parsed );
            plan->relation = RelationId{ *relation };
            return plan;
        }

        [[nodiscard]]
        Result<PlanPtr>
        // NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
        parse_expression( const Json&  object,
                          std::size_t& node_count )
        {
            constexpr std::size_t defaultNodeLimit = LocatorLimits::default_max_nodes;
            if( node_count >= defaultNodeLimit )
            {
                return invalid_locator(
                    "locator exceeds the default complexity budget"
                );
            }
            ++node_count;

            const auto op_member = required_member( object, "op", "$.expr" );
            if( !op_member )
            {
                return std::unexpected( op_member.error() );
            }
            const auto operation = json_string( **op_member, "$.expr.op" );
            if( !operation )
            {
                return std::unexpected( operation.error() );
            }
            if( *operation == "match_all" )
            {
                return make_plan( Op::MatchAll );
            }
            if( *operation == "match_none" )
            {
                return make_plan( Op::MatchNone );
            }
            if( *operation == "role" || *operation == "state" )
            {
                const auto value_member = required_member( object, "value", "$.expr" );
                if( !value_member )
                {
                    return std::unexpected( value_member.error() );
                }
                const auto value = unsigned_32( **value_member, "$.expr.value" );
                if( !value )
                {
                    return std::unexpected( value.error() );
                }
                auto plan = std::make_shared<Plan>();
                if( *operation == "role" )
                {
                    plan->op   = Op::Role;
                    plan->role = RoleId{ *value };
                }
                else
                {
                    plan->op    = Op::State;
                    plan->state = static_cast<NodeState>( *value );
                }
                return plan;
            }
            if( *operation == "property" )
            {
                const auto property_member =
                    required_member( object, "property", "$.expr" );
                if( !property_member )
                {
                    return std::unexpected( property_member.error() );
                }
                const auto property_id =
                    unsigned_32( **property_member, "$.expr.property" );
                if( !property_id )
                {
                    return std::unexpected( property_id.error() );
                }
                auto property_value = parse_property_value( object, "$.expr" );
                if( !property_value )
                {
                    return std::unexpected( property_value.error() );
                }
                auto plan      = std::make_shared<Plan>();
                plan->op       = Op::Property;
                plan->property = PropertyId{ *property_id };
                plan->value    = std::move( *property_value );
                return plan;
            }
            if( *operation == "accessible_name" || *operation == "text" )
            {
                const auto value_member = required_member( object, "value", "$.expr" );
                if( !value_member )
                {
                    return std::unexpected( value_member.error() );
                }
                auto value = json_string( **value_member, "$.expr.value" );
                if( !value )
                {
                    return std::unexpected( value.error() );
                }
                auto plan = std::make_shared<Plan>();
                plan->op =
                    *operation == "accessible_name" ? Op::AccessibleName : Op::Text;
                plan->text = std::move( *value );
                return plan;
            }
            if( *operation == "all" || *operation == "any" )
            {
                const auto children_member =
                    required_member( object, "children", "$.expr" );
                if( !children_member )
                {
                    return std::unexpected( children_member.error() );
                }
                if( !( **children_member ).is_array() )
                {
                    return invalid_locator( "$.expr.children must be an array" );
                }
                auto plan = std::make_shared<Plan>();
                plan->op  = *operation == "all" ? Op::All : Op::Any;
                plan->children.reserve( ( **children_member ).size() );
                for( const auto& child_object : **children_member )
                {
                    auto child = parse_expression( child_object, node_count );
                    if( !child )
                    {
                        return std::unexpected( child.error() );
                    }
                    plan->children.push_back( std::move( *child ) );
                }
                return plan;
            }
            if( *operation == "not" )
            {
                return parse_unary( object, Op::Not, node_count );
            }
            if( *operation == "child_of" )
            {
                return parse_unary( object, Op::ChildOf, node_count );
            }
            if( *operation == "descendant_of" )
            {
                return parse_unary( object, Op::DescendantOf, node_count );
            }
            if( *operation == "ancestor_of" )
            {
                return parse_unary( object, Op::AncestorOf, node_count );
            }
            if( *operation == "related" )
            {
                return parse_relation( object, Op::Related, node_count );
            }
            if( *operation == "related_reverse" )
            {
                return parse_relation( object, Op::RelatedReverse, node_count );
            }
            return invalid_locator( "$.expr.op has an unknown value" );
        }

        [[nodiscard]]
        std::size_t
        count_plan_nodes( const Plan& plan ) noexcept
        {
            std::size_t count = 1U;
            for( const auto& child : plan.children )
            {
                const std::size_t child_count = count_plan_nodes( *child );
                if( child_count > std::numeric_limits<std::size_t>::max() - count )
                {
                    return std::numeric_limits<std::size_t>::max();
                }
                count += child_count;
            }
            return count;
        }

        [[nodiscard]]
        Locator
        unary_locator( Op      operation,
                       Locator child )
        {
            const auto boundary    = child.boundary();
            const auto consistency = child.consistency();
            auto       plan        = std::make_shared<Plan>();
            plan->op               = operation;
            plan->children.push_back( copy_plan( child ) );
            return plan_detail::make_locator( std::move( plan ), boundary, consistency );
        }

        [[nodiscard]]
        Locator
        relation_locator( Op         operation,
                          RelationId relation,
                          Locator    child )
        {
            const auto boundary    = child.boundary();
            const auto consistency = child.consistency();
            auto       plan        = std::make_shared<Plan>();
            plan->op               = operation;
            plan->relation         = relation;
            plan->children.push_back( copy_plan( child ) );
            return plan_detail::make_locator( std::move( plan ), boundary, consistency );
        }

    }    // namespace

    Locator::Locator() :
        plan_( make_plan( Op::MatchAll ) )
    {
    }

    Locator::Locator( Locator&& other ) noexcept :
        plan_(
            other.plan_
        ),    // NOLINT(performance-move-constructor-init,cert-oop11-cpp)
        boundary_( other.boundary_ ),
        consistency_( other.consistency_ )
    {
    }

    Locator&
    Locator::operator=( Locator&& other ) noexcept
    {
        if( this != std::addressof( other ) )
        {
            plan_        = other.plan_;
            boundary_    = other.boundary_;
            consistency_ = other.consistency_;
        }
        return *this;
    }

    Locator::Locator( std::shared_ptr<const plan_detail::LocatorPlan> plan,
                      BoundaryPolicy                                  boundary,
                      ConsistencyMode consistency ) noexcept :
        plan_( std::move( plan ) ),
        boundary_( boundary ),
        consistency_( consistency )
    {
    }

    Locator
    Locator::and_( Locator predicate ) const
    {
        auto plan = std::make_shared<Plan>();
        plan->op  = Op::All;
        append_predicate( plan->children, plan_, Op::All );
        append_predicate( plan->children, predicate.plan_, Op::All );
        return plan_detail::make_locator( std::move( plan ),
                                          std::max( boundary_, predicate.boundary_ ),
                                          std::max( consistency_,
                                                    predicate.consistency_ ) );
    }

    Locator
    Locator::with_boundary( BoundaryPolicy boundary ) const
    {
        return plan_detail::make_locator( plan_, boundary, consistency_ );
    }

    Locator
    Locator::with_consistency( ConsistencyMode consistency ) const
    {
        return plan_detail::make_locator( plan_, boundary_, consistency );
    }

    BoundaryPolicy
    Locator::boundary() const noexcept
    {
        return boundary_;
    }

    ConsistencyMode
    Locator::consistency() const noexcept
    {
        return consistency_;
    }

    std::string
    Locator::to_string() const
    {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        Json object           = Json::object();
        object["boundary"]    = boundary_name( boundary_ );
        object["consistency"] = consistency_name( consistency_ );
        object["expr"]        = expression_json( *plan_ );
        object["version"]     = locatorVersion;
        auto serialized       = object.dump();
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        return serialized;
    }

    Result<Locator>
    Locator::from_string( std::string_view serialized )
    {
        try
        {
            const auto object = Json::parse( serialized.begin(), serialized.end() );
            if( !object.is_object() )
            {
                return invalid_locator( "locator JSON root must be an object" );
            }
            const auto version_member  = required_member( object, "version", "$" );
            const auto boundary_member = required_member( object, "boundary", "$" );
            const auto consistency_member =
                required_member( object, "consistency", "$" );
            const auto expression_member = required_member( object, "expr", "$" );
            if( !version_member ||
                !boundary_member ||
                !consistency_member ||
                !expression_member )
            {
                return invalid_locator( "locator JSON is missing a required field" );
            }
            const auto version = unsigned_32( **version_member, "$.version" );
            if( !version )
            {
                return std::unexpected( version.error() );
            }
            if( *version != locatorVersion )
            {
                return invalid_locator( "unsupported locator serialization version" );
            }
            const auto boundary = parse_boundary( **boundary_member );
            if( !boundary )
            {
                return std::unexpected( boundary.error() );
            }
            const auto consistency = parse_consistency( **consistency_member );
            if( !consistency )
            {
                return std::unexpected( consistency.error() );
            }
            std::size_t node_count = 0U;
            auto        plan       = parse_expression( **expression_member, node_count );
            if( !plan )
            {
                return std::unexpected( plan.error() );
            }
            return plan_detail::make_locator( std::move( *plan ),
                                              *boundary,
                                              *consistency );
        }
        catch( const Json::parse_error& error )
        {
            return invalid_locator( syntax_error_message( serialized, error.byte ) );
        }
        catch( const Json::exception& error )
        {
            return invalid_locator( std::string{ "invalid locator JSON value: " } +
                                    error.what() );
        }
    }

    bool
    operator==( const Locator& left,
                const Locator& right )
    {
        if( left.plan_ ==
            right.plan_ &&
            left.boundary_ ==
            right.boundary_ &&
            left.consistency_ == right.consistency_ )
        {
            return true;
        }
        return left.to_string() == right.to_string();
    }

    namespace kernel::query::detail
    {

        Locator
        make_locator( std::shared_ptr<const LocatorPlan> plan,
                      BoundaryPolicy                     boundary,
                      ConsistencyMode                    consistency )
        {
            if( !plan )
            {
                plan = make_plan( LocatorOp::MatchNone );
            }
            return Locator{ std::move( plan ), boundary, consistency };
        }

        const LocatorPlan&
        plan_of( const Locator& locator ) noexcept
        {
            return *locator.plan_;
        }

        std::size_t
        plan_node_count( const Locator& locator ) noexcept
        {
            return count_plan_nodes( plan_of( locator ) );
        }

    }    // namespace kernel::query::detail

    namespace sel
    {

        Locator
        role( RoleId value )
        {
            auto plan  = std::make_shared<Plan>();
            plan->op   = Op::Role;
            plan->role = value;
            return plan_detail::make_locator( std::move( plan ),
                                              BoundaryPolicy::SameTree,
                                              ConsistencyMode::Live );
        }

        Locator
        state( NodeState value )
        {
            auto plan   = std::make_shared<Plan>();
            plan->op    = Op::State;
            plan->state = value;
            return plan_detail::make_locator( std::move( plan ),
                                              BoundaryPolicy::SameTree,
                                              ConsistencyMode::Live );
        }

        Locator
        state( std::uint32_t mask )
        {
            return state( static_cast<NodeState>( mask ) );
        }

        Locator
        property( PropertyId    property_id,
                  PropertyValue value )
        {
            auto plan      = std::make_shared<Plan>();
            plan->op       = Op::Property;
            plan->property = property_id;
            plan->value    = std::move( value );
            return plan_detail::make_locator( std::move( plan ),
                                              BoundaryPolicy::SameTree,
                                              ConsistencyMode::Live );
        }

        Locator
        accessible_name( std::string_view value )
        {
            auto plan  = std::make_shared<Plan>();
            plan->op   = Op::AccessibleName;
            plan->text = value;
            return plan_detail::make_locator( std::move( plan ),
                                              BoundaryPolicy::SameTree,
                                              ConsistencyMode::Live );
        }

        Locator
        text( std::string_view value )
        {
            auto plan  = std::make_shared<Plan>();
            plan->op   = Op::Text;
            plan->text = value;
            return plan_detail::make_locator( std::move( plan ),
                                              BoundaryPolicy::SameTree,
                                              ConsistencyMode::Live );
        }

        Locator
        all( std::initializer_list<Locator> predicates )
        {
            if( predicates.size() == 0U )
            {
                return plan_detail::make_locator( make_plan( Op::MatchAll ),
                                                  BoundaryPolicy::SameTree,
                                                  ConsistencyMode::Live );
            }
            auto plan        = std::make_shared<Plan>();
            plan->op         = Op::All;
            auto boundary    = BoundaryPolicy::SameTree;
            auto consistency = ConsistencyMode::Live;
            for( const auto& predicate : predicates )
            {
                append_predicate( plan->children, copy_plan( predicate ), Op::All );
                boundary    = std::max( boundary, predicate.boundary() );
                consistency = std::max( consistency, predicate.consistency() );
            }
            return plan_detail::make_locator( std::move( plan ), boundary, consistency );
        }

        Locator
        any( std::initializer_list<Locator> predicates )
        {
            if( predicates.size() == 0U )
            {
                return plan_detail::make_locator( make_plan( Op::MatchNone ),
                                                  BoundaryPolicy::SameTree,
                                                  ConsistencyMode::Live );
            }
            auto plan        = std::make_shared<Plan>();
            plan->op         = Op::Any;
            auto boundary    = BoundaryPolicy::SameTree;
            auto consistency = ConsistencyMode::Live;
            for( const auto& predicate : predicates )
            {
                append_predicate( plan->children, copy_plan( predicate ), Op::Any );
                boundary    = std::max( boundary, predicate.boundary() );
                consistency = std::max( consistency, predicate.consistency() );
            }
            return plan_detail::make_locator( std::move( plan ), boundary, consistency );
        }

        Locator
        not_( Locator predicate )
        {
            return unary_locator( Op::Not, std::move( predicate ) );
        }

        Locator
        child_of( Locator parent )
        {
            return unary_locator( Op::ChildOf, std::move( parent ) );
        }

        Locator
        descendant_of( Locator ancestor )
        {
            return unary_locator( Op::DescendantOf, std::move( ancestor ) );
        }

        Locator
        ancestor_of( Locator descendant )
        {
            return unary_locator( Op::AncestorOf, std::move( descendant ) );
        }

        Locator
        related( RelationId relation,
                 Locator    target )
        {
            return relation_locator( Op::Related, relation, std::move( target ) );
        }

        Locator
        related_reverse( RelationId relation,
                         Locator    source )
        {
            return relation_locator( Op::RelatedReverse, relation, std::move( source ) );
        }

    }    // namespace sel

}    // namespace grab

std::size_t
std::hash<grab::Locator>::operator()( const grab::Locator& locator ) const
{
    return std::hash<std::string>{}( locator.to_string() );
}
