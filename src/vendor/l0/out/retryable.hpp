#pragma once

#include <concepts>
#include <out/traits.hpp>

namespace out
{

    // ── Retryable concept ───────────────────────────────────────
    //
    // Binary, cross-domain classification: "should the caller
    // retry after seeing this error?"
    //
    // Customisation point: each error-owning library provides a
    // free-function overload of `retryable(E)` found via ADL.
    // `out` prescribes no category taxonomy beyond this binary.

    template<class E>
    concept Retryable = requires( const E& e ) {
        {
            retryable( e )
        } -> std::convertible_to<bool>;
    };

    // ── Default overload for out::Error ─────────────────────────

    [[nodiscard]]
    constexpr bool
    retryable( Error e ) noexcept
    {
        switch( e )
        {
            case Error::time :
            case Error::busy :
            case Error::stuck :
                return true;
            case Error::not_found :
            case Error::under :
            case Error::over :
            case Error::barred :
            case Error::wrong :
            case Error::hardware :
            case Error::unhandled :
            case Error::cant :
                return false;
        }
        return false;
    }

}    // namespace out
