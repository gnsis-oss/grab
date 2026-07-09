#include "grab/result.hpp"
#include "inventory/action.hpp"
#include "inventory/sample.hpp"
#include "inventory/surface_registry.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace grab::inventory
{

    namespace
    {

        [[nodiscard]]
        bool
        has_sample_prefix( std::string_view value ) noexcept
        {
            return !value.empty() && value.front() == sample_key_prefix;
        }

        [[nodiscard]]
        std::string
        missing_sample_message( std::string_view key )
        {
            std::string message{ "no sample data for '" };
            message += key;
            message += '\'';
            return message;
        }

        [[nodiscard]]
        grab::Result<std::string>
        resolve_sample_text( std::string_view root,
                             std::string_view value )
        {
            if( !has_sample_prefix( value ) )
            {
                return std::string{ value };
            }

            value.remove_prefix( 1U );
            auto resolved = resolve_sample( root, value );
            if( !resolved.has_value() )
            {
                return grab::fail( grab::ErrorCode::invalid_argument,
                                   missing_sample_message( value ) );
            }
            return *resolved;
        }

        [[nodiscard]]
        grab::Result<Step>
        resolve_one_step( const LoadFileStep& step,
                          std::string_view    root )
        {
            auto path = resolve_sample_text( root, step.path );
            if( !path.has_value() )
            {
                return grab::fail( path.error().code, path.error().message );
            }
            return Step{ LoadFileStep{ .path = std::move( *path ) } };
        }

        [[nodiscard]]
        grab::Result<Step>
        resolve_one_step( const TypeStep&  step,
                          std::string_view root )
        {
            auto text = resolve_sample_text( root, step.text );
            if( !text.has_value() )
            {
                return grab::fail( text.error().code, text.error().message );
            }
            return Step{ TypeStep{ .text = std::move( *text ) } };
        }

        template<typename StepType>
        [[nodiscard]]
        grab::Result<Step>
        resolve_one_step( const StepType&  step,
                          std::string_view root )
        {
            ( void )root;
            return Step{ step };
        }

    }    // namespace

    std::optional<std::string>
    resolve_sample( std::string_view root,
                    std::string_view key )
    {
        auto relative = surface_sample_rel( key );
        if( !relative.has_value() )
        {
            return std::nullopt;
        }

        std::error_code error;
        const auto      path = std::filesystem::absolute(
            std::filesystem::path{ std::string{ root } } /
                std::filesystem::path{ std::string{ *relative } },
            error
        );
        if( error )
        {
            return std::nullopt;
        }
        error.clear();
        if( !std::filesystem::is_regular_file( path, error ) )
        {
            return std::nullopt;
        }
        return path.string();
    }

    grab::Result<std::vector<Step>>
    resolve_step_samples( const std::vector<Step>& steps,
                          std::string_view         root )
    {
        std::vector<Step> resolved;
        resolved.reserve( steps.size() );
        for( const Step& step : steps )
        {
            auto next = std::visit(
                [root]( const auto& item ) -> grab::Result<Step>
                {
                    return resolve_one_step( item, root );
                },
                step
            );
            if( !next.has_value() )
            {
                return grab::fail( next.error().code, next.error().message );
            }
            resolved.push_back( std::move( *next ) );
        }
        return resolved;
    }

}    // namespace grab::inventory
