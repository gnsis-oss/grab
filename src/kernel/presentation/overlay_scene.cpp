#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_scene.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        constexpr std::uint64_t revisionStep                     = 1U;
        constexpr std::uint64_t epochStep                        = 1U;
        constexpr std::uint64_t shapeSlotStep                    = 1U;
        constexpr std::size_t   slotOffset                       = 1U;
        constexpr std::size_t   immediatelyExpiringRevisionCount = 2U;
        constexpr double        zeroRadius{};

        static_assert( std::is_nothrow_move_constructible_v<overlay::ShapeRecord> );
        static_assert( std::is_nothrow_move_assignable_v<overlay::ShapeRecord> );
        static_assert( std::is_nothrow_move_constructible_v<overlay::SceneDelta> );
        static_assert( std::is_nothrow_move_assignable_v<overlay::SceneDelta> );

        [[nodiscard]]
        bool
        point_is_finite( const SpacePoint& point ) noexcept
        {
            return std::isfinite( point.x ) && std::isfinite( point.y );
        }

        [[nodiscard]]
        bool
        path_command_is_finite( const overlay::PathCommand& command ) noexcept
        {
            if( const auto* move = std::get_if<overlay::MoveTo>( &command ) )
            {
                return point_is_finite( move->point );
            }
            if( const auto* line = std::get_if<overlay::LineTo>( &command ) )
            {
                return point_is_finite( line->point );
            }
            if( const auto* bezier = std::get_if<overlay::BezierTo>( &command ) )
            {
                return std::ranges::all_of( bezier->control, point_is_finite );
            }
            return std::holds_alternative<overlay::ClosePath>( command );
        }

        [[nodiscard]]
        bool
        geometry_is_valid( const overlay::Geometry& geometry ) noexcept
        {
            if( const auto* path = std::get_if<overlay::Path>( &geometry ) )
            {
                return !path->commands.empty() &&
                       std::ranges::all_of( path->commands, path_command_is_finite );
            }
            if( const auto* rect = std::get_if<overlay::Rect>( &geometry ) )
            {
                return std::isfinite( rect->bounds.x ) &&
                       std::isfinite( rect->bounds.y ) &&
                       std::isfinite( rect->bounds.w ) &&
                       std::isfinite( rect->bounds.h );
            }
            if( const auto* ellipse = std::get_if<overlay::Ellipse>( &geometry ) )
            {
                return point_is_finite( ellipse->center ) &&
                       std::isfinite( ellipse->radius_x ) &&
                       std::isfinite( ellipse->radius_y ) &&
                       ellipse->radius_x >=
                       zeroRadius &&
                       ellipse->radius_y >= zeroRadius;
            }
            const auto* polygon = std::get_if<overlay::Polygon>( &geometry );
            return polygon !=
                   nullptr &&
                   std::ranges::all_of( polygon->points, point_is_finite );
        }

        [[nodiscard]]
        bool
        shape_is_valid( const overlay::Shape& shape ) noexcept
        {
            return ( shape.stroke.has_value() || shape.fill.has_value() ) &&
                   geometry_is_valid( shape.geometry );
        }

        [[nodiscard]]
        std::chrono::milliseconds
        saturating_add( std::chrono::milliseconds left,
                        std::chrono::milliseconds right ) noexcept
        {
            using Rep              = std::chrono::milliseconds::rep;
            constexpr auto maximum = std::numeric_limits<Rep>::max();
            constexpr auto minimum = std::numeric_limits<Rep>::min();
            const auto     lhs     = left.count();
            const auto     rhs     = right.count();
            if( rhs > Rep{} && lhs > maximum - rhs )
            {
                return std::chrono::milliseconds{ maximum };
            }
            if( rhs < Rep{} && lhs < minimum - rhs )
            {
                return std::chrono::milliseconds{ minimum };
            }
            return std::chrono::milliseconds{ lhs + rhs };
        }

        [[nodiscard]]
        std::optional<std::chrono::milliseconds>
        deadline_for( const overlay::ShapeRecord& record ) noexcept
        {
            if( const auto* ttl = std::get_if<overlay::Ttl>( &record.shape.lifetime ) )
            {
                return saturating_add( record.started_at, ttl->duration );
            }
            if( const auto* fade = std::get_if<overlay::Fade>( &record.shape.lifetime ) )
            {
                return saturating_add( record.started_at, fade->duration );
            }
            return std::nullopt;
        }

        [[nodiscard]]
        bool
        deadline_is_due( const overlay::ShapeRecord& record,
                         std::chrono::milliseconds   now ) noexcept
        {
            const auto deadline = deadline_for( record );
            return deadline.has_value() && *deadline <= now;
        }

        [[nodiscard]]
        constexpr std::size_t
        revision_count_for_mutation( bool expires_immediately ) noexcept
        {
            if( expires_immediately )
            {
                return immediatelyExpiringRevisionCount;
            }
            return slotOffset;
        }

    }    // namespace

    struct OverlayScene::Impl
    {
            struct Entry
            {
                    overlay::ShapeRecord record;
                    bool                 live{};
            };

            struct Deadline
            {
                    std::chrono::milliseconds expires_at{};
                    overlay::ShapeId          id{};
                    std::uint32_t             entry_slot{};
            };

            explicit Impl( Clock scene_clock ) :
                clock{ std::move( scene_clock ) }
            {
            }

            mutable std::mutex               mutex;
            Clock                            clock;
            std::shared_ptr<DeltaSink>       sink;
            std::vector<Entry>               entries;
            // Free slots index physical entries; public ShapeId slots do not repeat
            // until an epoch-changing clear.
            std::vector<std::uint32_t>       free_slots;
            std::vector<Deadline>            deadlines;
            std::vector<overlay::SceneDelta> pending_deltas;
            std::size_t                      publication_cursor{};
            overlay::SceneEpoch              epoch{};
            overlay::Revision                revision{};
            std::uint64_t                    next_shape_slot{};
            bool                             publishing{};
            std::atomic<std::uint64_t>       publication_failures;

            [[nodiscard]]
            static bool
            deadline_less( const Deadline& left,
                           const Deadline& right ) noexcept
            {
                return std::tie( left.expires_at, left.id.slot ) <
                       std::tie( right.expires_at, right.id.slot );
            }

            [[nodiscard]]
            std::optional<std::uint32_t>
            live_entry_slot( overlay::ShapeId id )
            {
                const auto found =
                    std::ranges::find_if( entries,
                                          [id]( const Entry& entry )
                                          {
                                              return entry.live && entry.record.id == id;
                                          } );
                if( found == entries.end() )
                {
                    return std::nullopt;
                }
                return static_cast<std::uint32_t>( found - entries.begin() );
            }

            void
            remove_deadline( overlay::ShapeId id )
            {
                std::erase_if( deadlines,
                               [id]( const Deadline& deadline )
                               {
                                   return deadline.id == id;
                               } );
            }

            void
            insert_deadline( const overlay::ShapeRecord& record,
                             std::uint32_t               entry_slot )
            {
                const auto expires_at = deadline_for( record );
                if( !expires_at.has_value() )
                {
                    return;
                }
                const Deadline deadline{
                    .expires_at = *expires_at,
                    .id         = record.id,
                    .entry_slot = entry_slot,
                };
                const auto insertion =
                    std::ranges::lower_bound( deadlines, deadline, deadline_less );
                deadlines.insert( insertion, deadline );
            }

            template<typename Change>
            void
            queue_change( Change change )
            {
                if( revision.value ==
                    std::numeric_limits<decltype( revision.value )>::max() )
                {
                    throw std::overflow_error{ "overlay revision overflow" };
                }
                const overlay::Revision next_revision{
                    .value = revision.value + revisionStep,
                };
                pending_deltas.push_back( overlay::SceneDelta{
                    .epoch    = epoch,
                    .revision = next_revision,
                    .change   = std::move( change ),
                } );
                revision = next_revision;
            }

            [[nodiscard]]
            bool
            revision_has_capacity( std::size_t count ) const noexcept
            {
                const auto remaining =
                    std::numeric_limits<decltype( revision.value )>::max() -
                    revision.value;
                return count <= remaining;
            }

            void
            drain_expired( std::chrono::milliseconds now )
            {
                const auto expired_end =
                    std::ranges::upper_bound( deadlines,
                                              now,
                                              {},
                                              &Deadline::expires_at );
                std::size_t expired_count{};
                for( auto current = deadlines.begin(); current != expired_end;
                     ++current )
                {
                    const auto& entry = entries.at( current->entry_slot );
                    if( entry.live && entry.record.id == current->id )
                    {
                        ++expired_count;
                    }
                }
                if( !revision_has_capacity( expired_count ) )
                {
                    throw std::overflow_error{ "overlay revision overflow" };
                }
                pending_deltas.reserve( pending_deltas.size() + expired_count );
                free_slots.reserve( free_slots.size() + expired_count );
                for( auto current = deadlines.begin(); current != expired_end;
                     ++current )
                {
                    auto& entry = entries.at( current->entry_slot );
                    if( !entry.live || entry.record.id != current->id )
                    {
                        continue;
                    }
                    const auto id = entry.record.id;
                    entry.live    = false;
                    entry.record  = {};
                    free_slots.push_back( current->entry_slot );
                    queue_change( overlay::Remove{ .id = id } );
                }
                deadlines.erase( deadlines.begin(), expired_end );
            }

            [[nodiscard]]
            bool
            begin_publication()
            {
                if( publishing || publication_cursor == pending_deltas.size() )
                {
                    return false;
                }
                publishing = true;
                return true;
            }

            void
            drain_publications() noexcept
            {
                for( ;; )
                {
                    overlay::SceneDelta        delta;
                    std::shared_ptr<DeltaSink> current_sink;
                    {
                        const std::scoped_lock lock{ mutex };
                        if( publication_cursor == pending_deltas.size() )
                        {
                            pending_deltas.clear();
                            publication_cursor = {};
                            publishing         = false;
                            return;
                        }
                        delta = std::move( pending_deltas.at( publication_cursor ) );
                        ++publication_cursor;
                        current_sink = sink;
                    }

                    if( current_sink == nullptr || !( *current_sink ) )
                    {
                        continue;
                    }
                    try
                    {
                        ( *current_sink )( delta );
                    }
                    catch( ... )
                    {
                        publication_failures.fetch_add( revisionStep,
                                                        std::memory_order_relaxed );
                    }
                }
            }
    };

    OverlayScene::OverlayScene( Clock clock ) :
        impl_{ std::make_unique<Impl>( std::move( clock ) ) }
    {
        if( !impl_->clock )
        {
            throw std::invalid_argument{ "overlay scene clock is required" };
        }
    }

    OverlayScene::~OverlayScene() = default;

    Result<overlay::ShapeId>
    OverlayScene::add( overlay::Shape shape )
    {
        const auto               now   = impl_->clock();
        const bool               valid = shape_is_valid( shape );
        Result<overlay::ShapeId> result{ overlay::ShapeId{} };
        bool                     should_publish{};
        {
            const std::scoped_lock lock{ impl_->mutex };
            impl_->drain_expired( now );
            if( !valid )
            {
                result = fail( ErrorCode::InvalidArgument,
                               "overlay shape geometry is invalid" );
            }
            else
            {
                constexpr auto maximumSlot = std::numeric_limits<std::uint32_t>::max();
                std::optional<std::uint32_t> entry_slot;
                bool                         reuses_entry{};
                if( impl_->next_shape_slot > maximumSlot )
                {
                    result =
                        fail( ErrorCode::Overflowed, "overlay shape ids are exhausted" );
                }
                else if( !impl_->free_slots.empty() )
                {
                    entry_slot   = impl_->free_slots.back();
                    reuses_entry = true;
                }
                else if( impl_->entries.size() > maximumSlot )
                {
                    result = fail( ErrorCode::Overflowed,
                                   "overlay shape storage is exhausted" );
                }
                else
                {
                    entry_slot = static_cast<std::uint32_t>( impl_->entries.size() );
                }

                if( entry_slot.has_value() )
                {
                    const overlay::ShapeId id{
                        .epoch = impl_->epoch,
                        .slot  = static_cast<std::uint32_t>( impl_->next_shape_slot ),
                    };
                    overlay::ShapeRecord record{
                        .id         = id,
                        .shape      = std::move( shape ),
                        .started_at = now,
                    };
                    auto       stored_record       = record;
                    const bool expires_immediately = deadline_is_due( record, now );
                    const auto revision_count =
                        revision_count_for_mutation( expires_immediately );
                    if( !impl_->revision_has_capacity( revision_count ) )
                    {
                        result = fail( ErrorCode::Overflowed,
                                       "overlay revisions are exhausted" );
                    }
                    else
                    {
                        if( *entry_slot == impl_->entries.size() )
                        {
                            impl_->entries.reserve( impl_->entries.size() + slotOffset );
                        }
                        if( deadline_for( record ).has_value() )
                        {
                            impl_->deadlines.reserve( impl_->deadlines.size() +
                                                      slotOffset );
                        }
                        impl_->pending_deltas.reserve( impl_->pending_deltas.size() +
                                                       revision_count );
                        if( expires_immediately )
                        {
                            impl_->free_slots.reserve( impl_->free_slots.size() +
                                                       slotOffset );
                        }
                        if( reuses_entry )
                        {
                            impl_->free_slots.pop_back();
                        }
                        if( *entry_slot == impl_->entries.size() )
                        {
                            impl_->entries.push_back( Impl::Entry{
                                .record = std::move( stored_record ),
                                .live   = true,
                            } );
                        }
                        else
                        {
                            impl_->entries.at( *entry_slot ) = Impl::Entry{
                                .record = std::move( stored_record ),
                                .live   = true,
                            };
                        }
                        impl_->insert_deadline( record, *entry_slot );
                        impl_->queue_change(
                            overlay::Upsert{ .record = std::move( record ) }
                        );
                        impl_->next_shape_slot += shapeSlotStep;
                        if( expires_immediately )
                        {
                            impl_->drain_expired( now );
                        }
                        result = id;
                    }
                }
            }
            should_publish = impl_->begin_publication();
        }
        if( should_publish )
        {
            impl_->drain_publications();
        }
        return result;
    }

    Result<void>
    OverlayScene::update( overlay::ShapeId id,
                          overlay::Shape   shape )
    {
        const auto   now   = impl_->clock();
        const bool   valid = shape_is_valid( shape );
        Result<void> result;
        bool         should_publish{};
        {
            const std::scoped_lock lock{ impl_->mutex };
            impl_->drain_expired( now );
            std::optional<std::uint32_t> entry_slot;
            if( id.epoch == impl_->epoch )
            {
                entry_slot = impl_->live_entry_slot( id );
            }
            if( !entry_slot.has_value() )
            {
                result = fail( ErrorCode::StaleShape, "overlay shape id is stale" );
            }
            else if( !valid )
            {
                result = fail( ErrorCode::InvalidArgument,
                               "overlay shape geometry is invalid" );
            }
            else
            {
                auto&      entry = impl_->entries.at( *entry_slot );
                const auto started_at =
                    entry.record.shape.lifetime.index() == shape.lifetime.index()
                        ? entry.record.started_at
                        : now;
                overlay::ShapeRecord record{
                    .id         = id,
                    .shape      = std::move( shape ),
                    .started_at = started_at,
                };
                auto       stored_record       = record;
                const bool expires_immediately = deadline_is_due( record, now );
                const auto revision_count =
                    revision_count_for_mutation( expires_immediately );
                if( !impl_->revision_has_capacity( revision_count ) )
                {
                    result =
                        fail( ErrorCode::Overflowed, "overlay revisions are exhausted" );
                }
                else
                {
                    if( deadline_for( record ).has_value() )
                    {
                        impl_->deadlines.reserve( impl_->deadlines.size() + slotOffset );
                    }
                    impl_->pending_deltas.reserve( impl_->pending_deltas.size() +
                                                   revision_count );
                    if( expires_immediately )
                    {
                        impl_->free_slots.reserve( impl_->free_slots.size() +
                                                   slotOffset );
                    }
                    impl_->remove_deadline( id );
                    entry.record = std::move( stored_record );
                    impl_->insert_deadline( record, *entry_slot );
                    impl_->queue_change(
                        overlay::Upsert{ .record = std::move( record ) }
                    );
                    if( expires_immediately )
                    {
                        impl_->drain_expired( now );
                    }
                }
            }
            should_publish = impl_->begin_publication();
        }
        if( should_publish )
        {
            impl_->drain_publications();
        }
        return result;
    }

    Result<void>
    OverlayScene::remove( overlay::ShapeId id )
    {
        const auto   now = impl_->clock();
        Result<void> result;
        bool         should_publish{};
        {
            const std::scoped_lock lock{ impl_->mutex };
            impl_->drain_expired( now );
            std::optional<std::uint32_t> entry_slot;
            if( id.epoch == impl_->epoch )
            {
                entry_slot = impl_->live_entry_slot( id );
            }
            if( !entry_slot.has_value() )
            {
                result = fail( ErrorCode::StaleShape, "overlay shape id is stale" );
            }
            else if( !impl_->revision_has_capacity( slotOffset ) )
            {
                result =
                    fail( ErrorCode::Overflowed, "overlay revisions are exhausted" );
            }
            else
            {
                impl_->pending_deltas.reserve( impl_->pending_deltas.size() +
                                               slotOffset );
                impl_->free_slots.reserve( impl_->free_slots.size() + slotOffset );
                auto& entry  = impl_->entries.at( *entry_slot );
                entry.live   = false;
                entry.record = {};
                impl_->remove_deadline( id );
                impl_->free_slots.push_back( *entry_slot );
                impl_->queue_change( overlay::Remove{ .id = id } );
            }
            should_publish = impl_->begin_publication();
        }
        if( should_publish )
        {
            impl_->drain_publications();
        }
        return result;
    }

    void
    OverlayScene::clear()
    {
        const auto now = impl_->clock();
        bool       should_publish{};
        {
            const std::scoped_lock lock{ impl_->mutex };
            if( impl_->epoch.value ==
                std::numeric_limits<decltype( impl_->epoch.value )>::max() )
            {
                throw std::overflow_error{ "overlay scene epoch overflow" };
            }
            impl_->drain_expired( now );
            impl_->free_slots.reserve( impl_->entries.size() );
            impl_->pending_deltas.reserve( impl_->pending_deltas.size() + slotOffset );
            impl_->free_slots.clear();
            for( auto index = impl_->entries.size(); index > 0U; --index )
            {
                auto& entry  = impl_->entries.at( index - slotOffset );
                entry.live   = false;
                entry.record = {};
                impl_->free_slots.push_back( static_cast<std::uint32_t>( index -
                                                                         slotOffset ) );
            }
            impl_->deadlines.clear();
            impl_->epoch.value     += epochStep;
            impl_->revision         = {};
            impl_->next_shape_slot  = {};
            impl_->queue_change( overlay::Clear{ .new_epoch = impl_->epoch } );
            should_publish = impl_->begin_publication();
        }
        if( should_publish )
        {
            impl_->drain_publications();
        }
    }

    overlay::SceneSnapshot
    OverlayScene::snapshot() const
    {
        const auto             now = impl_->clock();
        overlay::SceneSnapshot snapshot;
        bool                   should_publish{};
        {
            const std::scoped_lock lock{ impl_->mutex };
            impl_->drain_expired( now );
            snapshot.epoch            = impl_->epoch;
            snapshot.through_revision = impl_->revision;
            const auto live_count     = static_cast<std::size_t>(
                std::ranges::count_if( impl_->entries,
                                       []( const Impl::Entry& entry )
                                       {
                                           return entry.live;
                                       } )
            );
            snapshot.shapes.reserve( live_count );
            for( const auto& entry : impl_->entries )
            {
                if( entry.live )
                {
                    snapshot.shapes.push_back( entry.record );
                }
            }
            std::ranges::sort(
                snapshot.shapes,
                []( const overlay::ShapeRecord& left, const overlay::ShapeRecord& right )
                {
                    return std::tie( left.shape.band, left.shape.z, left.id.slot ) <
                           std::tie( right.shape.band, right.shape.z, right.id.slot );
                }
            );
            should_publish = impl_->begin_publication();
        }
        if( should_publish )
        {
            impl_->drain_publications();
        }
        return snapshot;
    }

    std::uint64_t
    OverlayScene::publication_failures() const noexcept
    {
        return impl_->publication_failures.load( std::memory_order_relaxed );
    }

    void
    OverlayScene::set_delta_sink( DeltaSink sink )
    {
        std::shared_ptr<DeltaSink> replacement;
        if( sink )
        {
            replacement = std::make_shared<DeltaSink>( std::move( sink ) );
        }
        const std::scoped_lock lock{ impl_->mutex };
        impl_->sink = std::move( replacement );
    }

}    // namespace grab::kernel::presentation
