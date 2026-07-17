#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/context.hpp"
#include "grab/result.hpp"
#include "spi/route.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace grab::spi
{

    class EventSource;
    class OverlayDelegate;
    class TopologySource;
    class TreeSource;

    struct ProbeReport
    {
            bool        usable{};
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
            ErrorCode   reason{};
            std::string detail;
    };

    class Runtime
    {
        public:

            Runtime()                 = default;
            virtual ~Runtime()        = default;
            Runtime( const Runtime& ) = delete;
            Runtime&
            operator=( const Runtime& ) = delete;
            Runtime( Runtime&& )        = delete;
            Runtime&
            operator=( Runtime&& ) = delete;

            [[nodiscard]]
            virtual std::string_view
            name() const = 0;

            [[nodiscard]]
            virtual std::uint32_t
            generation() const = 0;

            [[nodiscard]]
            virtual Result<void>
            start( const OperationContext& context ) = 0;

            [[nodiscard]]
            virtual Result<void>
            stop() = 0;

            [[nodiscard]]
            virtual TreeSource*
            tree_source()
            {
                return nullptr;
            }

            [[nodiscard]]
            virtual TopologySource*
            topology_source()
            {
                return nullptr;
            }

            [[nodiscard]]
            virtual EventSource*
            event_source()
            {
                return nullptr;
            }

            [[nodiscard]]
            virtual OverlayDelegate*
            overlay_delegate()
            {
                return nullptr;
            }

            [[nodiscard]]
            virtual std::span<const RouteDescriptor>
            routes() const = 0;

            [[nodiscard]]
            virtual ActionRoute*
            action_route( [[maybe_unused]] std::size_t index )
            {
                return nullptr;
            }

            [[nodiscard]]
            virtual InputSeat*
            input_seat()
            {
                return nullptr;
            }
    };

}    // namespace grab::spi
