#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_edit.hpp"
#include "spi/overlay_delegate.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace grab::kernel::presentation
{

    class OverlayEditSession final
        : public std::enable_shared_from_this<OverlayEditSession>
    {
        public:

            using EventSink = std::function<void( std::shared_ptr<OverlayEditSession>,
                                                  const spi::OverlayEditEvent& )>;

            [[nodiscard]]
            static Result<std::shared_ptr<OverlayEditSession>>
            start( spi::OverlayDelegate&                 delegate,
                   CoordinateSpaceId                     space,
                   std::span<const overlay::ShapeRecord> shapes,
                   std::vector<overlay::ShapeId>         editable,
                   EditCallbacks                         callbacks,
                   EventSink                             event_sink );

            ~OverlayEditSession();

            OverlayEditSession( const OverlayEditSession& ) = delete;
            OverlayEditSession&
            operator=( const OverlayEditSession& )     = delete;
            OverlayEditSession( OverlayEditSession&& ) = delete;
            OverlayEditSession&
            operator=( OverlayEditSession&& ) = delete;

            [[nodiscard]]
            Result<void>
            status() const;

            [[nodiscard]]
            bool
            live() const noexcept;

            [[nodiscard]]
            bool
            dragging() const noexcept;

            [[nodiscard]]
            bool
            begin( std::span<const overlay::ShapeRecord> shapes,
                   SpacePoint                            at,
                   std::uint8_t                          button );

            [[nodiscard]]
            std::optional<overlay::Shape>
            update( SpacePoint at );

            [[nodiscard]]
            std::optional<overlay::Shape>
            commit( SpacePoint at );

            void
            finish_drag();

            void
            cancel_interaction();

            [[nodiscard]]
            overlay::ShapeId
            target() const noexcept;

            [[nodiscard]]
            std::uint8_t
            button() const noexcept;

            [[nodiscard]]
            const std::optional<overlay::Shape>&
            original_shape() const noexcept;

            [[nodiscard]]
            std::span<const overlay::ShapeId>
            editable() const noexcept;

            [[nodiscard]]
            Result<void>
            refresh_region( std::span<const overlay::ShapeRecord> shapes );

            [[nodiscard]]
            Result<void>
            grab_pointer();

            [[nodiscard]]
            Result<void>
            release_pointer();

            void
            pointer_was_ungrabbed() noexcept;

            [[nodiscard]]
            Result<void>
            stop();

            // The delegate is owned by the runtime, not by this session.  The
            // service calls this after checked teardown succeeds, or after the
            // delegate itself has been closed, so a public EditSession may
            // safely outlive the runtime that created it.
            void
            detach_delegate() noexcept;

            void
            remember_error( Error error ) noexcept;

            [[nodiscard]]
            std::function<void( overlay::ShapeId,
                                const overlay::Shape& )>
            on_edit() const;

            [[nodiscard]]
            std::function<void( overlay::ShapeId )>
            on_cancelled() const;

        private:

            OverlayEditSession( spi::OverlayDelegate&         delegate,
                                CoordinateSpaceId             space,
                                std::vector<overlay::ShapeId> editable,
                                EditCallbacks                 callbacks,
                                EventSink                     event_sink );

            void
            dispatch( const spi::OverlayEditEvent& event ) noexcept;

            spi::OverlayDelegate*           delegate_{};
            CoordinateSpaceId               space_{};
            std::vector<overlay::ShapeId>   editable_;
            EditCallbacks                   callbacks_;
            EventSink                       event_sink_;
            EditInteraction                 interaction_;
            std::optional<overlay::Shape>   original_shape_;
            std::optional<overlay::ShapeId> target_;
            std::uint8_t                    button_{};
            std::atomic_bool                live_{};
            bool                            pointer_grabbed_{};
            bool                            edit_handler_installed_{};
            bool                            nonempty_region_installed_{};
            mutable std::mutex              event_mutex_;
            mutable std::mutex              error_mutex_;
            std::optional<Error>            error_;
    };

}    // namespace grab::kernel::presentation
