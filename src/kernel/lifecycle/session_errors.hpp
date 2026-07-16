#pragma once    // NOLINT(portability-avoid-pragma-once,llvm-header-guard)

#include "grab/result.hpp"

#include <exception>
#include <string>
#include <string_view>

namespace grab::kernel::lifecycle
{

    inline constexpr std::string_view sessionClosedMessage = "session is closed";
    inline constexpr std::string_view threadStartStep = "session reactor thread start";
    inline constexpr std::string_view reactorRunStep  = "session reactor run";

    [[nodiscard]]
    inline grab::Error
    internal_error( std::string_view step,
                    std::string_view message )
    {
        return grab::Error{
            .code       = grab::ErrorCode::InternalFault,
            .message    = std::string{ step } + ": " + std::string{ message },
            .capability = {},
            .target     = {},
            .attempts   = {},
        };
    }

    [[nodiscard]]
    inline grab::Error
    exception_error( std::string_view      step,
                     const std::exception& exception )
    {
        return internal_error( step, exception.what() );
    }

    [[nodiscard]]
    inline grab::Error
    unknown_exception_error( std::string_view step )
    {
        return internal_error( step, "unknown exception" );
    }

    [[nodiscard]]
    inline grab::Error
    session_closed_error()
    {
        return grab::Error{
            .code       = grab::ErrorCode::SessionClosed,
            .message    = std::string{ sessionClosedMessage },
            .capability = {},
            .target     = {},
            .attempts   = {},
        };
    }

}    // namespace grab::kernel::lifecycle
