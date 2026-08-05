#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/geometry/rectangle.hpp"
#include "grab/overlay.hpp"
#include "grab/result.hpp"
#include "grab/space.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace grab::spi
{

    enum class OverlayEditEventKind : std::uint8_t
    {
        ButtonPress,
        PointerMotion,
        ButtonRelease,
        NotifyUngrab,
    };

    struct OverlayEditEvent
    {
            OverlayEditEventKind kind{};
            SpacePoint           position{};
            std::uint8_t         button{};
    };

    using OverlayEditHandler = std::function<void( const OverlayEditEvent& )>;

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

            [[nodiscard]]
            virtual Result<void>
            set_input_region( std::span<const geometry::Rectangle> rectangles )
            {
                if( rectangles.empty() )
                {
                    return {};
                }
                return fail( ErrorCode::CapabilityUnavailable,
                             "overlay delegate has no editable input region" );
            }

            [[nodiscard]]
            virtual Result<void>
            set_edit_handler( OverlayEditHandler handler )
            {
                if( !handler )
                {
                    return {};
                }
                return fail( ErrorCode::CapabilityUnavailable,
                             "overlay delegate has no edit event source" );
            }

            [[nodiscard]]
            virtual Result<void>
            grab_pointer()
            {
                return fail( ErrorCode::CapabilityUnavailable,
                             "overlay delegate cannot grab the pointer" );
            }

            [[nodiscard]]
            virtual Result<void>
            ungrab_pointer()
            {
                return {};
            }

            virtual void
            close() = 0;
    };

}    // namespace grab::spi
