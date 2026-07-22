#pragma once

#include <expected>
#include <log/writer.hpp>
#include <out/traits.hpp>
#include <type_traits>
#include <utility>

namespace out
{

    template<typename T, typename E>
    requires( std::is_scoped_enum_v<E> && ErrorTraits<E>::registered )
    class Put
    {
            static_assert( !std::is_same_v<T,
                                           E>,
                           "T and E must be distinct types" );

            std::expected<T, E> inner_;

        public:

            // NOLINTBEGIN(google-explicit-constructor,hicpp-explicit-conversions):
            // implicit conversion from T or E into Put is the API contract — the whole
            // point of the type is to let `return value;` and `return error;` round-trip
            // without wrapping.
            Put( T val ) noexcept( std::is_nothrow_move_constructible_v<T> ) :
                inner_( std::move( val ) )
            {
                logger::trace( logger::tag( "out.put" ),
                               "Put(T) constructed with value" );
            }

            Put( E err ) noexcept :
                inner_( std::unexpected( err ) )
            {
                logger::trace( logger::tag( "out.put" ),
                               "Put(E) constructed with error={}",
                               static_cast<int>( err ) );
            }

            // NOLINTEND(google-explicit-constructor,hicpp-explicit-conversions)

            Put( const Put& ) = default;
            Put( Put&& )      = default;
            ~Put()            = default;
            Put&
            operator=( const Put& ) = default;
            Put&
            operator=( Put&& ) = default;

            [[nodiscard]]
            T*
            ok() noexcept
            {
                T* result = inner_.has_value() ? &*inner_ : nullptr;
                logger::trace( logger::tag( "out.put" ),
                               "ok() returning ptr={}",
                               static_cast<const void*>( result ) );
                return result;
            }

            [[nodiscard]]
            const T*
            ok() const noexcept
            {
                const T* result = inner_.has_value() ? &*inner_ : nullptr;
                logger::trace( logger::tag( "out.put" ),
                               "ok() returning ptr={}",
                               static_cast<const void*>( result ) );
                return result;
            }

            [[nodiscard]]
            E
            error() const noexcept
            {
                return inner_.error();
            }

            [[nodiscard]]
            explicit
            operator bool() const noexcept
            {
                return inner_.has_value();
            }

            bool
            operator==( const Put& ) const = default;
    };

    // ── void specialization ─────────────────────────────────────

    template<typename E>
    requires( std::is_scoped_enum_v<E> && ErrorTraits<E>::registered )
    class Put<void, E>
    {
            std::expected<void, E> inner_;

        public:

            Put() noexcept :
                inner_()
            {
                logger::trace( logger::tag( "out.put" ), "Put<void>() constructed" );
            }

            // NOLINTBEGIN(google-explicit-constructor,hicpp-explicit-conversions):
            // implicit conversion from E into Put<void> is the API contract.
            Put( E err ) noexcept :
                inner_( std::unexpected( err ) )
            {
                logger::trace( logger::tag( "out.put" ),
                               "Put<void>(E) constructed with error={}",
                               static_cast<int>( err ) );
            }

            // NOLINTEND(google-explicit-constructor,hicpp-explicit-conversions)

            Put( const Put& ) = default;
            Put( Put&& )      = default;
            ~Put()            = default;
            Put&
            operator=( const Put& ) = default;
            Put&
            operator=( Put&& ) = default;

            [[nodiscard]]
            bool
            ok() const noexcept
            {
                return inner_.has_value();
            }

            [[nodiscard]]
            E
            error() const noexcept
            {
                return inner_.error();
            }

            [[nodiscard]]
            explicit
            operator bool() const noexcept
            {
                return inner_.has_value();
            }

            bool
            operator==( const Put& ) const = default;
    };

}    // namespace out
