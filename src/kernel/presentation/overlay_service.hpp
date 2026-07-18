#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"
#include "kernel/presentation/overlay_scene.hpp"

#include <memory>
#include <mutex>

namespace grab::detail
{

    class SpaceGraph;

}

namespace grab::spi
{

    class OverlayDelegate;
    class Runtime;

}

namespace grab::kernel::presentation
{

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
            CoordinateSpaceId
            delegate_space() const noexcept
            {
                return delegate_space_;
            }

        private:

            OverlayService( spi::OverlayDelegate&     delegate,
                            const detail::SpaceGraph& graph,
                            CoordinateSpaceId         delegate_space,
                            OverlayScene::Clock       clock );

            void
            publish( const overlay::SceneDelta& delta );

            void
            recover_best_effort();

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
    };

}    // namespace grab::kernel::presentation
