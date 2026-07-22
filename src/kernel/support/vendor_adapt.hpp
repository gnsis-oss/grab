#pragma once

#include "grab/result.hpp"

#include <out/put.hpp>
#include <out/utils.hpp>
#include <string>
#include <utility>

namespace grab::detail
{

    // Convert a vendored out::Put<T, E> into grab::Result<T>. out::Put exposes
    // `explicit operator bool`, `ok() -> T*` (nullptr on error), and
    // `error() -> E`. There is no `.value()`. Callers supply the grab ErrorCode;
    // the vendor error is rendered into Error::message.
    template<typename T,
             typename E>
    [[nodiscard]]
    Result<T>
    from_put( out::Put<T,
                       E>&& put,
              ErrorCode     code )
    {
        if( T* ok = put.ok() )
        {
            return std::move( *ok );
        }
        return std::unexpected( grab::Error{
            .code = code,
            .message =
                std::string{ "vendor: " } + std::string{ out::name( put.error() ) },
            .capability = {},
            .target     = {},
            .attempts   = {},
        } );
    }

    // Void specialization: out::Put<void, E> has no ok() payload; success is
    // the boolean state.
    template<typename E>
    [[nodiscard]]
    Result<void>
    from_put( out::Put<void,
                       E>&& put,
              ErrorCode     code )
    {
        if( static_cast<bool>( put ) )
        {
            return {};
        }
        return std::unexpected( grab::Error{
            .code = code,
            .message =
                std::string{ "vendor: " } + std::string{ out::name( put.error() ) },
            .capability = {},
            .target     = {},
            .attempts   = {},
        } );
    }

}
