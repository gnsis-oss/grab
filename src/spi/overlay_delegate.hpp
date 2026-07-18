#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"

#include <span>

namespace grab::spi
{

    // Delegates apply enveloped deltas contiguously within an epoch. A Clear
    // delta that opens a new epoch (revision 1 of that epoch) is the explicit
    // epoch transition and is applied atomically — state empties, the new
    // epoch is adopted. Any other epoch change or revision discontinuity is a
    // gap: the delegate desynchronizes until resync().
    class OverlayDelegate
    {
        public:

            OverlayDelegate()                         = default;
            virtual ~OverlayDelegate()                = default;
            OverlayDelegate( const OverlayDelegate& ) = delete;
            OverlayDelegate&
            operator=( const OverlayDelegate& )  = delete;
            OverlayDelegate( OverlayDelegate&& ) = delete;
            OverlayDelegate&
            operator=( OverlayDelegate&& ) = delete;

            [[nodiscard]]
            virtual Result<void>
            open( CoordinateSpaceId space ) = 0;

            [[nodiscard]]
            virtual Result<void>
            apply( std::span<const overlay::SceneDelta> deltas ) = 0;

            [[nodiscard]]
            virtual Result<void>
            resync( const overlay::SceneSnapshot& scene ) = 0;

            [[nodiscard]]
            virtual Result<void>
            flush( overlay::Revision through ) = 0;

            virtual void
            close() = 0;
    };

}    // namespace grab::spi
