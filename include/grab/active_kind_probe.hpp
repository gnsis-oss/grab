#pragma once

#include "grab/event.hpp"

namespace grab
{

    class ActiveKindProbe
    {
        public:

            ActiveKindProbe()                         = default;
            virtual ~ActiveKindProbe()                = default;

            ActiveKindProbe( const ActiveKindProbe& ) = delete;
            ActiveKindProbe&
            operator=( const ActiveKindProbe& )  = delete;
            ActiveKindProbe( ActiveKindProbe&& ) = delete;
            ActiveKindProbe&
            operator=( ActiveKindProbe&& ) = delete;

            [[nodiscard]]
            virtual bool
            is_active( EventKind kind ) const noexcept = 0;
    };

}    // namespace grab
