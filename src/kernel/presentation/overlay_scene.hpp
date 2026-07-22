#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"

#include <chrono>
#include <functional>
#include <memory>

namespace grab::kernel::presentation
{

    class OverlayScene final
    {
        public:

            using Clock     = std::function<std::chrono::milliseconds()>;
            using DeltaSink = std::function<void( const overlay::SceneDelta& )>;

            explicit OverlayScene( Clock clock );
            ~OverlayScene();

            OverlayScene( const OverlayScene& ) = delete;
            OverlayScene&
            operator=( const OverlayScene& ) = delete;
            OverlayScene( OverlayScene&& )   = delete;
            OverlayScene&
            operator=( OverlayScene&& ) = delete;

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
            overlay::SceneSnapshot
            snapshot() const;

            void
            set_delta_sink( DeltaSink sink );

            // Number of sink invocations that threw; deltas are still
            // considered published (the scene never blocks on a sink).
            [[nodiscard]]
            std::uint64_t
            publication_failures() const noexcept;

        private:

            struct Impl;
            std::unique_ptr<Impl> impl_;
    };

}    // namespace grab::kernel::presentation
