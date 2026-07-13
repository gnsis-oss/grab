#include "grab/presentation.hpp"
#include "grab/result.hpp"
#include "kernel/graph/target_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace grab::kernel
{
    namespace
    {

        [[nodiscard]]
        bool
        valid_grade( TargetGrade grade ) noexcept
        {
            switch( grade )
            {
                case TargetGrade::Application :
                case TargetGrade::Window :
                    return true;
            }
            return false;
        }

        [[nodiscard]]
        bool
        valid_confidence( AliasConfidence confidence ) noexcept
        {
            switch( confidence )
            {
                case AliasConfidence::Candidate :
                case AliasConfidence::Exact :
                    return true;
            }
            return false;
        }

        [[nodiscard]]
        bool
        valid_validity( AliasValidity validity ) noexcept
        {
            switch( validity )
            {
                case AliasValidity::Active :
                case AliasValidity::Inactive :
                    return true;
            }
            return false;
        }

        [[nodiscard]]
        Result<void>
        validate_alias( const AliasEdge& alias )
        {
            if( alias.authority.value.empty() )
            {
                return fail( ErrorCode::InvalidArgument,
                             "target alias authority must not be empty" );
            }
            if( alias.native_id.value.empty() )
            {
                return fail( ErrorCode::InvalidArgument,
                             "target alias native id must not be empty" );
            }
            if( !valid_confidence( alias.confidence ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "target alias has an invalid confidence" );
            }
            if( !valid_validity( alias.validity ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "target alias has an invalid validity" );
            }
            return {};
        }

        [[nodiscard]]
        Result<void>
        validate( const TargetObservation& observation )
        {
            if( !valid_grade( observation.grade ) )
            {
                return fail( ErrorCode::InvalidArgument,
                             "target observation has an invalid grade" );
            }

            if( !observation.alias.has_value() )
            {
                return {};
            }

            return validate_alias( *observation.alias );
        }

        template<typename T>
        [[nodiscard]]
        Result<T>
        registry_failure( const std::exception& error )
        {
            return fail( ErrorCode::InternalFault,
                         std::string{ "target registry failed: " } + error.what() );
        }

    }    // namespace

    bool
    TargetRegistry::is_fusing( const AliasEdge& alias ) noexcept
    {
        return alias.confidence ==
               AliasConfidence::Exact &&
               alias.validity == AliasValidity::Active;
    }

    Result<TargetId>
    TargetRegistry::observe( TargetObservation observation )
    {
        auto validation = validate( observation );
        if( !validation.has_value() )
        {
            return std::unexpected( std::move( validation.error() ) );
        }

        try
        {
            const std::scoped_lock lock{ mutex_ };
            if( observation.alias.has_value() && is_fusing( *observation.alias ) )
            {
                const AliasKey key{
                    .authority = observation.alias->authority,
                    .native_id = observation.alias->native_id,
                };
                const auto existing = exact_active_aliases_.find( key );
                if( existing != exact_active_aliases_.end() )
                {
                    auto record = targets_.find( existing->second );
                    if( record == targets_.end() )
                    {
                        return fail( ErrorCode::InternalFault,
                                     "target alias index is inconsistent" );
                    }
                    if( record->second.grade != observation.grade )
                    {
                        return fail(
                            ErrorCode::InvalidArgument,
                            "exact target alias was observed at a different grade"
                        );
                    }

                    if( std::ranges::find( record->second.aliases,
                                           *observation.alias ) ==
                        record->second.aliases.end() )
                    {
                        record->second.aliases.push_back( *observation.alias );
                    }
                    record->second.observations.push_back( std::move( observation ) );
                    return record->first;
                }
            }

            if( next_id_ == std::numeric_limits<std::uint64_t>::max() )
            {
                return fail( ErrorCode::Overflowed,
                             "target registry exhausted its identity space" );
            }

            const TargetId id{ .value = next_id_ };
            TargetRecord   record{
                .id           = id,
                .grade        = observation.grade,
                .aliases      = {},
                .observations = {},
                .surfaces     = {},
            };
            if( observation.alias.has_value() )
            {
                record.aliases.push_back( *observation.alias );
            }
            record.observations.push_back( std::move( observation ) );

            const auto [inserted, did_insert] =
                targets_.emplace( id, std::move( record ) );
            if( !did_insert )
            {
                return fail( ErrorCode::InternalFault,
                             "target registry generated a duplicate identity" );
            }

            const auto& alias = inserted->second.observations.back().alias;
            if( alias.has_value() && is_fusing( *alias ) )
            {
                try
                {
                    const auto [alias_position, alias_inserted] =
                        exact_active_aliases_.emplace(
                            AliasKey{
                                .authority = alias->authority,
                                .native_id = alias->native_id,
                            },
                            id
                        );
                    static_cast<void>( alias_position );
                    if( !alias_inserted )
                    {
                        targets_.erase( inserted );
                        return fail( ErrorCode::InternalFault,
                                     "target alias index generated a duplicate key" );
                    }
                }
                catch( ... )
                {
                    targets_.erase( inserted );
                    throw;
                }
            }

            ++next_id_;
            return id;
        }
        catch( const std::exception& error )
        {
            return registry_failure<TargetId>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    Result<void>
    TargetRegistry::attach_alias( TargetId  target_id,
                                  AliasEdge alias )
    {
        auto validation = validate_alias( alias );
        if( !validation.has_value() )
        {
            return std::unexpected( std::move( validation.error() ) );
        }

        try
        {
            const std::scoped_lock lock{ mutex_ };
            auto                   target_record = targets_.find( target_id );
            if( target_record == targets_.end() )
            {
                return fail( ErrorCode::TargetDetached,
                             "target is not present in the registry" );
            }

            const AliasKey key{
                .authority = alias.authority,
                .native_id = alias.native_id,
            };
            if( is_fusing( alias ) )
            {
                const auto existing = exact_active_aliases_.find( key );
                if( existing !=
                    exact_active_aliases_.end() &&
                    existing->second != target_id )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "exact alias already identifies another target" );
                }
            }

            const bool already_attached =
                std::ranges::find( target_record->second.aliases, alias ) !=
                target_record->second.aliases.end();
            if( already_attached )
            {
                return {};
            }

            target_record->second.aliases.push_back( alias );
            try
            {
                if( is_fusing( alias ) )
                {
                    exact_active_aliases_.emplace( key, target_id );
                }
            }
            catch( ... )
            {
                target_record->second.aliases.pop_back();
                throw;
            }
            return {};
        }
        catch( const std::exception& error )
        {
            return registry_failure<void>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    Result<void>
    TargetRegistry::invalidate_alias( const AliasAuthority& authority,
                                      const NativeAliasId&  native_id )
    {
        if( authority.value.empty() || native_id.value.empty() )
        {
            return fail( ErrorCode::InvalidArgument,
                         "target alias key must not be empty" );
        }

        try
        {
            const std::scoped_lock lock{ mutex_ };
            const AliasKey         key{
                .authority = authority,
                .native_id = native_id,
            };
            const auto alias_position = exact_active_aliases_.find( key );
            if( alias_position == exact_active_aliases_.end() )
            {
                return fail( ErrorCode::TargetDetached, "target alias is not active" );
            }

            auto target_record = targets_.find( alias_position->second );
            if( target_record == targets_.end() )
            {
                return fail( ErrorCode::InternalFault,
                             "target alias index is inconsistent" );
            }
            for( auto& alias : target_record->second.aliases )
            {
                if( alias.authority ==
                    authority &&
                    alias.native_id ==
                    native_id &&
                    alias.validity == AliasValidity::Active )
                {
                    alias.validity = AliasValidity::Inactive;
                }
            }
            exact_active_aliases_.erase( alias_position );
            return {};
        }
        catch( const std::exception& error )
        {
            return registry_failure<void>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    Result<void>
    TargetRegistry::register_surface( TargetId      target_id,
                                      SurfaceRecord surface )
    {
        if( surface.id.value == 0U )
        {
            return fail( ErrorCode::InvalidArgument, "surface id must not be zero" );
        }

        try
        {
            const std::scoped_lock lock{ mutex_ };
            auto                   target_record = targets_.find( target_id );
            if( target_record == targets_.end() )
            {
                return fail( ErrorCode::TargetDetached,
                             "target is not present in the registry" );
            }

            auto owner = surface_owners_.find( surface.id );
            if( owner != surface_owners_.end() && owner->second != target_id )
            {
                return fail( ErrorCode::InvalidArgument,
                             "surface already belongs to another target" );
            }

            auto existing =
                std::ranges::find_if( target_record->second.surfaces,
                                      [&surface]( const SurfaceRecord& record )
                                      {
                                          return record.id == surface.id;
                                      } );
            if( owner != surface_owners_.end() )
            {
                if( existing == target_record->second.surfaces.end() )
                {
                    return fail( ErrorCode::InternalFault,
                                 "surface ownership index is inconsistent" );
                }
                *existing = surface;
                return {};
            }
            if( existing != target_record->second.surfaces.end() )
            {
                return fail( ErrorCode::InternalFault,
                             "target surface record is missing its ownership index" );
            }

            target_record->second.surfaces.push_back( surface );
            try
            {
                const auto [position, inserted] =
                    surface_owners_.emplace( surface.id, target_id );
                static_cast<void>( position );
                if( !inserted )
                {
                    target_record->second.surfaces.pop_back();
                    return fail( ErrorCode::InternalFault,
                                 "surface ownership index generated a duplicate id" );
                }
            }
            catch( ... )
            {
                target_record->second.surfaces.pop_back();
                throw;
            }
            return {};
        }
        catch( const std::exception& error )
        {
            return registry_failure<void>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    Result<void>
    TargetRegistry::remove_surface( TargetId  target_id,
                                    SurfaceId surface_id )
    {
        if( surface_id.value == 0U )
        {
            return fail( ErrorCode::InvalidArgument, "surface id must not be zero" );
        }

        try
        {
            const std::scoped_lock lock{ mutex_ };
            auto                   target_record = targets_.find( target_id );
            if( target_record == targets_.end() )
            {
                return fail( ErrorCode::TargetDetached,
                             "target is not present in the registry" );
            }

            auto owner = surface_owners_.find( surface_id );
            if( owner == surface_owners_.end() )
            {
                return fail( ErrorCode::TargetDetached,
                             "surface is not present in the registry" );
            }
            if( owner->second != target_id )
            {
                return fail( ErrorCode::InvalidArgument,
                             "surface belongs to another target" );
            }

            const auto existing =
                std::ranges::find_if( target_record->second.surfaces,
                                      [surface_id]( const SurfaceRecord& record )
                                      {
                                          return record.id == surface_id;
                                      } );
            if( existing == target_record->second.surfaces.end() )
            {
                return fail( ErrorCode::InternalFault,
                             "surface ownership index is inconsistent" );
            }

            target_record->second.surfaces.erase( existing );
            surface_owners_.erase( owner );
            return {};
        }
        catch( const std::exception& error )
        {
            return registry_failure<void>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    Result<TargetRecord>
    TargetRegistry::target( TargetId id ) const
    {
        try
        {
            const std::scoped_lock lock{ mutex_ };
            const auto             found = targets_.find( id );
            if( found == targets_.end() )
            {
                return fail( ErrorCode::TargetDetached,
                             "target is not present in the registry" );
            }
            return found->second;
        }
        catch( const std::exception& error )
        {
            return registry_failure<TargetRecord>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    Result<std::vector<TargetRecord>>
    TargetRegistry::targets() const
    {
        try
        {
            const std::scoped_lock    lock{ mutex_ };
            std::vector<TargetRecord> result;
            result.reserve( targets_.size() );
            for( const auto& entry : targets_ )
            {
                result.push_back( entry.second );
            }
            return result;
        }
        catch( const std::exception& error )
        {
            return registry_failure<std::vector<TargetRecord>>( error );
        }
        catch( ... )
        {
            return fail( ErrorCode::InternalFault,
                         "target registry failed with an unknown error" );
        }
    }

    std::size_t
    TargetRegistry::size() const
    {
        const std::scoped_lock lock{ mutex_ };
        return targets_.size();
    }

}    // namespace grab::kernel
