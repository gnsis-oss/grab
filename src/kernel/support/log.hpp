#pragma once

// `compileLevel` arrives as an inline constexpr from a generated header, one
// per build directory, rather than as a preprocessor define. See
// cmake/Logging.cmake.
#include "kernel/support/log_config.hpp"

#include <concepts>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace grab::log
{

    // Compile levels: 0 disables all log calls; 1 enables nominal lifecycle
    // events; 2 adds verbose branch/candidate detail; 3 adds debug detail.
    inline constexpr int offLevel     = 0;
    inline constexpr int nominalLevel = 1;
    inline constexpr int verboseLevel = 2;
    inline constexpr int debugLevel   = 3;

    enum class Level : int
    {
        Off     = offLevel,
        Nominal = nominalLevel,
        Verbose = verboseLevel,
        Debug   = debugLevel,
    };

    [[nodiscard]]
    consteval bool
    enabled( Level level ) noexcept
    {
        return compileLevel >= static_cast<int>( level );
    }

    class Event
    {
        public:

            Event()               = default;

            Event( const Event& ) = delete;
            Event&
            operator=( const Event& ) = delete;
            Event( Event&& )          = delete;
            Event&
            operator=( Event&& ) = delete;

            ~Event()
            {
                ( void )std::fputc( '\n', stderr );
            }

            Event&
            tag( std::string_view tag_name ) noexcept
            {
                write_key_value_prefix( "tag" );
                write_text( tag_name );
                return *this;
            }

            template<typename Value>
            Event&
            value( std::string_view key,
                   const Value&     value ) noexcept
            {
                write_key_value_prefix( key );
                write_value( value );
                return *this;
            }

        private:

            bool first_ = true;

            void
            write_separator() noexcept
            {
                if( first_ )
                {
                    first_ = false;
                    return;
                }
                ( void )std::fputc( ' ', stderr );
            }

            void
            write_key_value_prefix( std::string_view key ) noexcept
            {
                write_separator();
                write_text( key );
                ( void )std::fputc( '=', stderr );
            }

            static void
            write_text( std::string_view text ) noexcept
            {
                ( void )std::fwrite( text.data(), sizeof( char ), text.size(), stderr );
            }

            static void
            write_value( std::string_view value ) noexcept
            {
                write_text( value );
            }

            static void
            write_value( const std::string& value ) noexcept
            {
                write_text( value );
            }

            static void
            write_value( const char* value ) noexcept
            {
                if( value == nullptr )
                {
                    write_text( "null" );
                    return;
                }
                ( void )std::fputs( value, stderr );
            }

            static void
            write_value( bool value ) noexcept
            {
                write_text( value ? "true" : "false" );
            }

            template<std::signed_integral Value>
            static void
            write_value( Value value ) noexcept
            {
                ( void )std::fprintf( stderr, "%lld", static_cast<long long>( value ) );
            }

            template<std::unsigned_integral Value>
            static void
            write_value( Value value ) noexcept
            {
                ( void )std::fprintf( stderr,
                                      "%llu",
                                      static_cast<unsigned long long>( value ) );
            }

            template<std::floating_point Value>
            static void
            write_value( Value value ) noexcept
            {
                ( void )std::fprintf( stderr, "%.17g", static_cast<double>( value ) );
            }

            template<typename Value>
            requires std::is_enum_v<Value>
            static void
            write_value( Value value ) noexcept
            {
                write_value( static_cast<std::underlying_type_t<Value>>( value ) );
            }
    };

    template<Level level,
             typename Emit>
    void
    emit( Emit&& emit_event )
    {
        if constexpr( enabled( level ) )
        {
            Event event;
            std::forward<Emit>( emit_event )( event );
        }
    }

    template<typename Emit>
    void
    nominal( Emit&& emit_event )
    {
        emit<Level::Nominal>( std::forward<Emit>( emit_event ) );
    }

    template<typename Emit>
    void
    verbose( Emit&& emit_event )
    {
        emit<Level::Verbose>( std::forward<Emit>( emit_event ) );
    }

    template<typename Emit>
    void
    debug( Emit&& emit_event )
    {
        emit<Level::Debug>( std::forward<Emit>( emit_event ) );
    }

}    // namespace grab::log
