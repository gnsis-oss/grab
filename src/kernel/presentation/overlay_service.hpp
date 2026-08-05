#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/overlay_edit.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_scene.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace grab::detail
{

    class SpaceGraph;

}

namespace grab::spi
{

    struct OverlayEditEvent;
    class OverlayDelegate;
    class Runtime;

}

namespace grab::kernel::presentation
{

    class OverlayEditSession;

    class OverlayService final
    {
        public:

            [[nodiscard]]
            static Result<std::unique_ptr<OverlayService>>
            create( spi::Runtime&             runtime,
                    const detail::SpaceGraph& graph,
                    CoordinateSpaceId         delegate_space,
                    OverlayScene::Clock       clock );

            ~OverlayService();

            OverlayService( const OverlayService& ) = delete;
            OverlayService&
            operator=( const OverlayService& ) = delete;
            OverlayService( OverlayService&& ) = delete;
            OverlayService&
            operator=( OverlayService&& ) = delete;

            [[nodiscard]]
            Result<overlay::ShapeId>
            add( overlay::Shape shape );

            [[nodiscard]]
            Result<std::vector<overlay::ShapeId>>
            add_many( std::span<overlay::Shape> shapes );

            [[nodiscard]]
            Result<void>
            update( overlay::ShapeId id,
                    overlay::Shape   shape );

            [[nodiscard]]
            Result<void>
            remove( overlay::ShapeId id );

            void
            clear();

            [[nodiscard]]
            Result<void>
            flush();

            [[nodiscard]]
            Result<std::shared_ptr<OverlayEditSession>>
            start_edit( std::span<const overlay::ShapeId> editable,
                        EditCallbacks                     callbacks );

            [[nodiscard]]
            Result<void>
            stop_edit( const std::shared_ptr<OverlayEditSession>& session );

            [[nodiscard]]
            CoordinateSpaceId
            delegate_space() const noexcept
            {
                return delegate_space_;
            }

        private:

            struct EditNotification
            {
                    std::shared_ptr<OverlayEditSession> session;
                    overlay::ShapeId                    id{};
                    std::optional<overlay::Shape>       shape;
            };

            OverlayService( spi::OverlayDelegate&     delegate,
                            const detail::SpaceGraph& graph,
                            CoordinateSpaceId         delegate_space,
                            OverlayScene::Clock       clock );

            void
            publish( const overlay::SceneDelta& delta );

            void
            recover_best_effort();

            void
            handle_edit_event( std::shared_ptr<OverlayEditSession> session,
                               const spi::OverlayEditEvent&        event ) noexcept;

            void
            abort_edit_after_exception( std::shared_ptr<OverlayEditSession> session,
                                        Error error ) noexcept;

            [[nodiscard]]
            std::optional<EditNotification>
            cancel_drag_locked( std::shared_ptr<OverlayEditSession> session,
                                bool                                restore_original );

            [[nodiscard]]
            std::optional<EditNotification>
            refresh_edit_locked();

            // Stops all delegate-side edit state.  Successful cleanup detaches
            // the session's non-owning delegate pointer and releases service
            // ownership; a failure remains owned as cleanup-pending so a later
            // verb can retry it safely on the reactor thread.
            [[nodiscard]]
            Result<void>
            terminate_edit_locked( const std::shared_ptr<OverlayEditSession>& session );

            static void
            invoke_notification( std::optional<EditNotification> notification ) noexcept;

            [[nodiscard]]
            Result<void>
                                      recover( const overlay::SceneSnapshot& snapshot );

            spi::OverlayDelegate*     delegate_{};
            const detail::SpaceGraph* graph_{};
            CoordinateSpaceId         delegate_space_{};
            OverlayScene              scene_;
            std::mutex                mutex_;
            bool                      opened_{};
            bool                      desynchronized_{};
            std::shared_ptr<OverlayEditSession> edit_session_;
    };

}    // namespace grab::kernel::presentation
