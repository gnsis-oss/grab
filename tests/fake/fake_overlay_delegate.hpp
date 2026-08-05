#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "spi/overlay_delegate.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace grab::testing
{

    enum class OverlayDelegateState : std::uint8_t
    {
        Closed,
        Synced,
        Desynced,
    };

    struct OverlayOpenCall
    {
            CoordinateSpaceId space{};
    };

    struct OverlayApplyCall
    {
            std::vector<overlay::SceneDelta> deltas{};
    };

    struct OverlayResyncCall
    {
            overlay::SceneSnapshot scene{};
    };

    struct OverlayFlushCall
    {
            overlay::Revision through{};
    };

    struct OverlayInputRegionCall
    {
            std::vector<geometry::Rectangle> rectangles{};
    };

    struct OverlayEditHandlerCall
    {
            bool installed{};
    };

    struct OverlayGrabPointerCall
    {
    };

    struct OverlayUngrabPointerCall
    {
    };

    struct OverlayCloseCall
    {
    };

    using OverlayCall     = std::variant<OverlayOpenCall,
                                         OverlayApplyCall,
                                         OverlayResyncCall,
                                         OverlayFlushCall,
                                         OverlayInputRegionCall,
                                         OverlayEditHandlerCall,
                                         OverlayGrabPointerCall,
                                         OverlayUngrabPointerCall,
                                         OverlayCloseCall>;
    using OverlayShapeMap = std::map<overlay::ShapeId, overlay::ShapeRecord>;

    class FakeOverlayDelegate final : public spi::OverlayDelegate
    {
        public:

            [[nodiscard]]
            Result<void>
            open( CoordinateSpaceId space ) override
            {
                calls_.emplace_back( OverlayOpenCall{ .space = space } );
                if( state_ != OverlayDelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "overlay delegate is already open" );
                }

                state_ = OverlayDelegateState::Synced;
                space_ = space;
                epoch_.reset();
                through_revision_ = {};
                shapes_.clear();
                input_region_.clear();
                edit_handler_    = {};
                pointer_grabbed_ = false;
                apply_failure_.reset();
                flush_failure_.reset();
                input_region_failure_.reset();
                grab_failure_.reset();
                return {};
            }

            [[nodiscard]]
            Result<void>
            apply( std::span<const overlay::SceneDelta> deltas ) override
            {
                calls_.emplace_back( OverlayApplyCall{
                    .deltas = { deltas.begin(), deltas.end() },
                } );

                if( state_ == OverlayDelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "overlay delegate is closed" );
                }
                if( state_ == OverlayDelegateState::Desynced )
                {
                    return fail( ErrorCode::ResyncRequired,
                                 "overlay delegate requires resync" );
                }
                if( apply_failure_.has_value() )
                {
                    auto failure = std::move( *apply_failure_ );
                    apply_failure_.reset();
                    state_ = OverlayDelegateState::Desynced;
                    return fail( failure.code, std::move( failure.message ) );
                }

                auto candidate_shapes   = shapes_;
                auto candidate_epoch    = epoch_;
                auto candidate_revision = through_revision_;
                for( const auto& delta : deltas )
                {
                    // A Clear delta opening a new epoch IS the explicit epoch
                    // transition: applied atomically, never treated as a gap.
                    if( candidate_epoch.has_value() &&
                        delta.epoch !=
                        *candidate_epoch &&
                        std::holds_alternative<overlay::Clear>( delta.change ) &&
                        delta.revision.value == firstRevisionValue )
                    {
                        candidate_shapes.clear();
                        candidate_epoch    = delta.epoch;
                        candidate_revision = delta.revision;
                        continue;
                    }
                    if( candidate_epoch.has_value() && delta.epoch != *candidate_epoch )
                    {
                        return desync( "overlay scene epoch changed" );
                    }

                    if( !candidate_epoch.has_value() )
                    {
                        if( delta.revision.value != firstRevisionValue )
                        {
                            return desync(
                                "overlay delta stream did not start at revision 1"
                            );
                        }
                        candidate_epoch = delta.epoch;
                    }
                    else if( delta.revision <= candidate_revision )
                    {
                        continue;
                    }
                    else if( delta.revision.value -
                             candidate_revision.value != revisionIncrement )
                    {
                        return desync( "overlay delta revision is non-contiguous" );
                    }

                    apply_change( candidate_shapes, delta );
                    candidate_revision = delta.revision;
                }

                shapes_           = std::move( candidate_shapes );
                epoch_            = candidate_epoch;
                through_revision_ = candidate_revision;
                return {};
            }

            [[nodiscard]]
            Result<void>
            resync( const overlay::SceneSnapshot& scene ) override
            {
                calls_.emplace_back( OverlayResyncCall{ .scene = scene } );
                if( state_ == OverlayDelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot resync a closed overlay delegate" );
                }

                OverlayShapeMap replacement;
                for( const auto& record : scene.shapes )
                {
                    replacement.insert_or_assign( record.id, record );
                }

                shapes_           = std::move( replacement );
                epoch_            = scene.epoch;
                through_revision_ = scene.through_revision;
                state_            = OverlayDelegateState::Synced;
                return {};
            }

            [[nodiscard]]
            Result<void>
            flush( overlay::Revision through ) override
            {
                calls_.emplace_back( OverlayFlushCall{ .through = through } );
                if( state_ == OverlayDelegateState::Closed )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot flush a closed overlay delegate" );
                }
                if( flush_failure_.has_value() && flush_failures_remaining_ > 0U )
                {
                    --flush_failures_remaining_;
                    auto failure = *flush_failure_;
                    if( flush_failures_remaining_ == 0U )
                    {
                        flush_failure_.reset();
                    }
                    state_ = OverlayDelegateState::Desynced;
                    return fail( failure.code, std::move( failure.message ) );
                }
                if( state_ == OverlayDelegateState::Desynced )
                {
                    return fail( ErrorCode::ResyncRequired,
                                 "overlay delegate requires resync" );
                }
                if( through > through_revision_ )
                {
                    return fail( ErrorCode::InvalidArgument,
                                 "cannot flush an unapplied overlay revision" );
                }
                return {};
            }

            [[nodiscard]]
            Result<void>
            set_input_region( std::span<const geometry::Rectangle> rectangles ) override
            {
                std::vector<geometry::Rectangle> candidate{
                    rectangles.begin(),
                    rectangles.end()
                };
                calls_.emplace_back( OverlayInputRegionCall{
                    .rectangles = candidate,
                } );
                if( state_ == OverlayDelegateState::Closed && !candidate.empty() )
                {
                    return fail(
                        ErrorCode::InvalidArgument,
                        "overlay delegate input region requires an open delegate"
                    );
                }
                if( input_region_failure_.has_value() )
                {
                    auto failure = std::move( *input_region_failure_ );
                    input_region_failure_.reset();
                    return fail( failure.code, std::move( failure.message ) );
                }
                input_region_ = std::move( candidate );
                return {};
            }

            [[nodiscard]]
            Result<void>
            set_edit_handler( spi::OverlayEditHandler handler ) override
            {
                calls_.emplace_back( OverlayEditHandlerCall{
                    .installed = static_cast<bool>( handler ),
                } );
                if( state_ == OverlayDelegateState::Closed && handler )
                {
                    return fail(
                        ErrorCode::InvalidArgument,
                        "overlay delegate edit handler requires an open delegate"
                    );
                }
                edit_handler_ = std::move( handler );
                return {};
            }

            [[nodiscard]]
            Result<void>
            grab_pointer() override
            {
                calls_.emplace_back( OverlayGrabPointerCall{} );
                if( state_ == OverlayDelegateState::Closed )
                {
                    return fail(
                        ErrorCode::InvalidArgument,
                        "overlay delegate pointer grab requires an open delegate"
                    );
                }
                if( grab_failure_.has_value() )
                {
                    auto failure = std::move( *grab_failure_ );
                    grab_failure_.reset();
                    return fail( failure.code, std::move( failure.message ) );
                }
                pointer_grabbed_ = true;
                return {};
            }

            [[nodiscard]]
            Result<void>
            ungrab_pointer() override
            {
                calls_.emplace_back( OverlayUngrabPointerCall{} );
                pointer_grabbed_ = false;
                return {};
            }

            void
            close() override
            {
                calls_.emplace_back( OverlayCloseCall{} );
                state_ = OverlayDelegateState::Closed;
                space_ = {};
                epoch_.reset();
                through_revision_ = {};
                shapes_.clear();
                input_region_.clear();
                edit_handler_    = {};
                pointer_grabbed_ = false;
                apply_failure_.reset();
                flush_failure_.reset();
                input_region_failure_.reset();
                grab_failure_.reset();
            }

            void
            fail_next_apply( ErrorCode   code,
                             std::string message )
            {
                apply_failure_ = InjectedFailure{
                    .code    = code,
                    .message = std::move( message ),
                };
            }

            // The injected flush failure also desynchronizes the delegate,
            // mirroring a real fence failure (X11: sync round-trip loss).
            void
            fail_next_flush( ErrorCode   code,
                             std::string message )
            {
                fail_next_flushes( 1U, code, std::move( message ) );
            }

            void
            fail_next_flushes( std::uint32_t count,
                               ErrorCode     code,
                               std::string   message )
            {
                flush_failures_remaining_ = count;
                flush_failure_            = InjectedFailure{
                    .code    = code,
                    .message = std::move( message ),
                };
            }

            void
            fail_next_input_region( ErrorCode   code,
                                    std::string message )
            {
                input_region_failure_ = InjectedFailure{
                    .code    = code,
                    .message = std::move( message ),
                };
            }

            void
            fail_next_grab( ErrorCode   code,
                            std::string message )
            {
                grab_failure_ = InjectedFailure{
                    .code    = code,
                    .message = std::move( message ),
                };
            }

            void
            emit_edit_event( const spi::OverlayEditEvent& event )
            {
                if( event.kind == spi::OverlayEditEventKind::NotifyUngrab )
                {
                    pointer_grabbed_ = false;
                }
                const auto handler = edit_handler_;
                if( handler )
                {
                    handler( event );
                }
            }

            [[nodiscard]]
            OverlayDelegateState
            state() const noexcept
            {
                return state_;
            }

            [[nodiscard]]
            bool
            closed() const noexcept
            {
                return state_ == OverlayDelegateState::Closed;
            }

            [[nodiscard]]
            bool
            synced() const noexcept
            {
                return state_ == OverlayDelegateState::Synced;
            }

            [[nodiscard]]
            bool
            desynced() const noexcept
            {
                return state_ == OverlayDelegateState::Desynced;
            }

            [[nodiscard]]
            CoordinateSpaceId
            space() const noexcept
            {
                return space_;
            }

            [[nodiscard]]
            overlay::SceneEpoch
            epoch() const noexcept
            {
                return epoch_.value_or( overlay::SceneEpoch{} );
            }

            [[nodiscard]]
            overlay::Revision
            through_revision() const noexcept
            {
                return through_revision_;
            }

            [[nodiscard]]
            const OverlayShapeMap&
            shapes() const noexcept
            {
                return shapes_;
            }

            [[nodiscard]]
            const std::vector<geometry::Rectangle>&
            input_region() const noexcept
            {
                return input_region_;
            }

            [[nodiscard]]
            bool
            edit_handler_installed() const noexcept
            {
                return static_cast<bool>( edit_handler_ );
            }

            [[nodiscard]]
            bool
            pointer_grabbed() const noexcept
            {
                return pointer_grabbed_;
            }

            [[nodiscard]]
            const std::vector<OverlayCall>&
            calls() const noexcept
            {
                return calls_;
            }

        private:

            struct InjectedFailure
            {
                    ErrorCode   code{ ErrorCode::InternalFault };
                    std::string message{};
            };

            [[nodiscard]]
            Result<void>
            desync( std::string message )
            {
                state_ = OverlayDelegateState::Desynced;
                return fail( ErrorCode::ResyncRequired, std::move( message ) );
            }

            static void
            apply_change( OverlayShapeMap&           shapes,
                          const overlay::SceneDelta& delta )
            {
                if( const auto* upsert = std::get_if<overlay::Upsert>( &delta.change ) )
                {
                    shapes.insert_or_assign( upsert->record.id, upsert->record );
                    return;
                }
                if( const auto* remove = std::get_if<overlay::Remove>( &delta.change ) )
                {
                    shapes.erase( remove->id );
                    return;
                }
                shapes.clear();
            }

            static constexpr std::uint64_t     firstRevisionValue = 1U;
            static constexpr std::uint64_t     revisionIncrement  = 1U;

            OverlayDelegateState               state_{ OverlayDelegateState::Closed };
            CoordinateSpaceId                  space_{};
            std::optional<overlay::SceneEpoch> epoch_{};
            overlay::Revision                  through_revision_{};
            OverlayShapeMap                    shapes_{};
            std::vector<geometry::Rectangle>   input_region_{};
            std::vector<OverlayCall>           calls_{};
            spi::OverlayEditHandler            edit_handler_{};
            std::optional<InjectedFailure>     apply_failure_{};
            std::optional<InjectedFailure>     flush_failure_{};
            std::optional<InjectedFailure>     input_region_failure_{};
            std::optional<InjectedFailure>     grab_failure_{};
            std::uint32_t                      flush_failures_remaining_{};
            bool                               pointer_grabbed_{};
    };

}    // namespace grab::testing
