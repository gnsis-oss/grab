#include "grab/geometry/rectangle.hpp"
#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_edit.hpp"
#include "kernel/presentation/overlay_edit_session.hpp"
#include "spi/overlay_delegate.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace grab::kernel::presentation
{
    namespace
    {

        [[nodiscard]]
        Error
        event_exception( const std::exception& exception )
        {
            return Error{
                .code = ErrorCode::InternalFault,
                .message =
                    std::string{ "overlay edit event handler: " } + exception.what(),
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

        [[nodiscard]]
        Error
        unknown_event_exception()
        {
            return Error{
                .code       = ErrorCode::InternalFault,
                .message    = "overlay edit event handler failed",
                .capability = {},
                .target     = {},
                .attempts   = {},
            };
        }

    }    // namespace

    OverlayEditSession::OverlayEditSession( spi::OverlayDelegate&         delegate,
                                            CoordinateSpaceId             space,
                                            std::vector<overlay::ShapeId> editable,
                                            EditCallbacks                 callbacks,
                                            EventSink                     event_sink ) :
        delegate_{ &delegate },
        space_{ space },
        editable_{ std::move( editable ) },
        callbacks_{ std::move( callbacks ) },
        event_sink_{ std::move( event_sink ) }
    {
    }

    Result<std::shared_ptr<OverlayEditSession>>
    OverlayEditSession::start( spi::OverlayDelegate&                 delegate,
                               CoordinateSpaceId                     space,
                               std::span<const overlay::ShapeRecord> shapes,
                               std::vector<overlay::ShapeId>         editable,
                               EditCallbacks                         callbacks,
                               EventSink                             event_sink )
    {
        auto       session = std::shared_ptr<OverlayEditSession>( new OverlayEditSession{
            delegate,
            space,
            std::move( editable ),
            std::move( callbacks ),
            std::move( event_sink )
        } );
        const auto rollback = [&session]() noexcept
        {
            const auto attempt_stop = [&session]() noexcept
            {
                try
                {
                    return session->stop().has_value();
                }
                catch( ... )
                {
                    return false;
                }
            };
            if( !attempt_stop() )
            {
                // Retry once while the delegate is still alive.  This handles
                // one-shot/transient rollback failures without handing a raw
                // delegate pointer to later off-reactor destruction.
                static_cast<void>( attempt_stop() );
            }
            session->detach_delegate();
        };

        try
        {
            const std::weak_ptr<OverlayEditSession> weak = session;
            // Pessimistically require a clear before asking the provider to
            // install the handler.  This also covers a provider that throws
            // after partially applying the request.
            session->edit_handler_installed_ = true;
            auto selected                    = delegate.set_edit_handler(
                [weak]( const spi::OverlayEditEvent& event )
                {
                    if( const auto locked = weak.lock() )
                    {
                        locked->dispatch( event );
                    }
                }
            );
            if( !selected.has_value() )
            {
                auto error = std::move( selected.error() );
                rollback();
                return std::unexpected( std::move( error ) );
            }

            auto installed = session->refresh_region( shapes );
            if( !installed.has_value() )
            {
                auto error = std::move( installed.error() );
                session->remember_error( error );
                rollback();
                return std::unexpected( std::move( error ) );
            }
            session->live_.store( true, std::memory_order_release );
            return session;
        }
        catch( ... )
        {
            rollback();
            throw;
        }
    }

    OverlayEditSession::~OverlayEditSession() = default;

    Result<void>
    OverlayEditSession::status() const
    {
        const std::scoped_lock lock{ error_mutex_ };
        if( error_.has_value() )
        {
            return std::unexpected( *error_ );
        }
        return {};
    }

    bool
    OverlayEditSession::live() const noexcept
    {
        return live_.load( std::memory_order_acquire );
    }

    bool
    OverlayEditSession::dragging() const noexcept
    {
        return interaction_.active();
    }

    bool
    OverlayEditSession::begin( std::span<const overlay::ShapeRecord> shapes,
                               SpacePoint                            at,
                               std::uint8_t                          button )
    {
        if( !live() ||
            at.space !=
            space_ ||
            !interaction_.begin( shapes, editable_, at, EditGeometryOptions{} ) )
        {
            return false;
        }
        const auto selected = interaction_.target();
        const auto record =
            std::ranges::find( shapes, selected, &overlay::ShapeRecord::id );
        if( record == shapes.end() )
        {
            interaction_.cancel();
            return false;
        }
        target_         = selected;
        original_shape_ = record->shape;
        button_         = button;
        return true;
    }

    std::optional<overlay::Shape>
    OverlayEditSession::update( SpacePoint at )
    {
        return interaction_.update( at );
    }

    std::optional<overlay::Shape>
    OverlayEditSession::commit( SpacePoint at )
    {
        return interaction_.commit( at );
    }

    void
    OverlayEditSession::finish_drag()
    {
        original_shape_.reset();
        target_.reset();
        button_ = {};
    }

    void
    OverlayEditSession::cancel_interaction()
    {
        interaction_.cancel();
        finish_drag();
    }

    overlay::ShapeId
    OverlayEditSession::target() const noexcept
    {
        return target_.value_or( overlay::ShapeId{} );
    }

    std::uint8_t
    OverlayEditSession::button() const noexcept
    {
        return button_;
    }

    const std::optional<overlay::Shape>&
    OverlayEditSession::original_shape() const noexcept
    {
        return original_shape_;
    }

    std::span<const overlay::ShapeId>
    OverlayEditSession::editable() const noexcept
    {
        return editable_;
    }

    Result<void>
    OverlayEditSession::refresh_region( std::span<const overlay::ShapeRecord> shapes )
    {
        if( delegate_ == nullptr )
        {
            return fail( ErrorCode::InvalidArgument,
                         "overlay edit session is detached" );
        }
        const auto region =
            edit_input_region( shapes, editable_, EditGeometryOptions{} );
        if( !region.empty() )
        {
            // Preserve the cleanup obligation if a provider throws or reports
            // failure after partially applying a non-empty region.
            nonempty_region_installed_ = true;
        }
        auto installed = delegate_->set_input_region( region );
        if( installed.has_value() )
        {
            nonempty_region_installed_ = !region.empty();
        }
        return installed;
    }

    Result<void>
    OverlayEditSession::grab_pointer()
    {
        if( pointer_grabbed_ )
        {
            return {};
        }
        if( delegate_ == nullptr )
        {
            return fail( ErrorCode::InvalidArgument,
                         "overlay edit session is detached" );
        }
        // Treat a failed provider call as potentially having installed the
        // grab.  Teardown can safely issue an ungrab even when the server
        // refused it, and this preserves the transactional cleanup obligation
        // when a transport failure makes the result ambiguous.
        pointer_grabbed_ = true;
        return delegate_->grab_pointer();
    }

    Result<void>
    OverlayEditSession::release_pointer()
    {
        if( !pointer_grabbed_ )
        {
            return {};
        }
        if( delegate_ == nullptr )
        {
            pointer_grabbed_ = false;
            return {};
        }
        auto released = delegate_->ungrab_pointer();
        if( !released.has_value() )
        {
            remember_error( released.error() );
        }
        else
        {
            pointer_grabbed_ = false;
        }
        return released;
    }

    void
    OverlayEditSession::pointer_was_ungrabbed() noexcept
    {
        pointer_grabbed_ = false;
    }

    Result<void>
    OverlayEditSession::stop()
    {
        live_.store( false, std::memory_order_release );
        {
            const std::scoped_lock lock{ event_mutex_ };
            event_sink_ = {};
        }
        interaction_.cancel();
        finish_drag();

        if( delegate_ == nullptr )
        {
            pointer_grabbed_           = false;
            edit_handler_installed_    = false;
            nonempty_region_installed_ = false;
            return {};
        }

        std::optional<Error> first_error;
        if( pointer_grabbed_ )
        {
            auto released = delegate_->ungrab_pointer();
            if( !released.has_value() )
            {
                first_error = std::move( released.error() );
            }
            else
            {
                pointer_grabbed_ = false;
            }
        }
        if( edit_handler_installed_ || nonempty_region_installed_ )
        {
            auto emptied =
                delegate_->set_input_region( std::span<const geometry::Rectangle>{} );
            if( !emptied.has_value() && !first_error.has_value() )
            {
                first_error = std::move( emptied.error() );
            }
            if( emptied.has_value() )
            {
                nonempty_region_installed_ = false;
            }
        }
        if( edit_handler_installed_ )
        {
            auto cleared = delegate_->set_edit_handler( spi::OverlayEditHandler{} );
            if( !cleared.has_value() && !first_error.has_value() )
            {
                first_error = std::move( cleared.error() );
            }
            if( cleared.has_value() )
            {
                edit_handler_installed_ = false;
            }
        }
        if( first_error.has_value() )
        {
            remember_error( *first_error );
            return std::unexpected( std::move( *first_error ) );
        }
        return {};
    }

    void
    OverlayEditSession::detach_delegate() noexcept
    {
        live_.store( false, std::memory_order_release );
        // Null the non-owning pointer first: even an unexpected late
        // destruction can no longer re-enter a dead runtime.
        delegate_                  = nullptr;
        pointer_grabbed_           = false;
        edit_handler_installed_    = false;
        nonempty_region_installed_ = false;
        interaction_.cancel();
        finish_drag();
        try
        {
            const std::scoped_lock lock{ event_mutex_ };
            event_sink_ = {};
        }
        catch( ... )
        {
            // live_ is already false and the delegate is detached; a copied
            // sink cannot be reached by any newly dispatched event.
            return;
        }
    }

    void
    OverlayEditSession::remember_error( Error error ) noexcept
    {
        try
        {
            const std::scoped_lock lock{ error_mutex_ };
            if( !error_.has_value() )
            {
                error_ = std::move( error );
            }
        }
        catch( ... )
        {
            return;
        }
    }

    std::function<void( overlay::ShapeId,
                        const overlay::Shape& )>
    OverlayEditSession::on_edit() const
    {
        return callbacks_.on_edit;
    }

    std::function<void( overlay::ShapeId )>
    OverlayEditSession::on_cancelled() const
    {
        return callbacks_.on_cancelled;
    }

    void
    OverlayEditSession::dispatch( const spi::OverlayEditEvent& event ) noexcept
    {
        if( !live() )
        {
            return;
        }
        try
        {
            EventSink sink;
            {
                const std::scoped_lock lock{ event_mutex_ };
                if( !live() )
                {
                    return;
                }
                sink = event_sink_;
            }
            if( sink )
            {
                sink( shared_from_this(), event );
            }
        }
        catch( const std::exception& exception )
        {
            remember_error( event_exception( exception ) );
        }
        catch( ... )
        {
            remember_error( unknown_event_exception() );
        }
    }

}    // namespace grab::kernel::presentation
